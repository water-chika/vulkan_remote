#!/usr/bin/env python3
"""CTS regression tracking for the remoting driver.

dEQP-VK.api.info.* alone is 8167 cases, and this project implements a small
fraction of the Vulkan command set, so most of them are expected to not pass
and will stay that way for a long time. Comparing the whole 8167-case result
against a fixed number (as the README used to) tells you nothing about
whether *this change* broke anything. What matters is the delta against the
last known-good run, so this script records a set of case names that were
seen passing and only reports movement relative to that set:

  REGRESSIONS       - baseline said this case passes; it does not now.
                       This is the only thing that makes the run fail.
  NEW PASSES        - passing now, not yet in the baseline. Not an error;
                       run again with --update to adopt them (review the
                       diff before committing it, the same as any other
                       change to recorded behaviour).
  STILL NOT PASSING - neither passing nor in the baseline. Informational.
                       This is the expected state for most of the suite
                       until the corresponding command is implemented.

Baseline file format (tests/cts_baseline.txt by default): one fully
qualified case name per line, sorted, blank lines and lines starting with
'#' ignored. Regenerate a subset with, e.g.:

    python3 tests/run_cts.py --caselist 'dEQP-VK.api.smoke.*' --update
    python3 tests/run_cts.py --caselist 'dEQP-VK.api.info.*'  --update

Each --update only touches cases inside --caselist's scope: it drops
baseline entries in scope that regressed, adds new passes in scope, and
leaves every baseline entry outside scope untouched. So running smoke.*
then info.* with --update accumulates a baseline covering both, rather
than each overwriting the other.

Stdlib only, so it runs anywhere the server and deqp-vk build.
"""

import argparse
import fnmatch
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_wire import Server, Failure  # reuse the same server-lifecycle helper

DEFAULT_DEQP = ('/mnt/worktrees/VK-GL-CTS/build/external/vulkancts/modules/'
                'vulkan/deqp-vk')

# QualityWarning/CompatibilityWarning are dEQP's way of saying "passed, but
# noted something" - they are not failures and must not be treated as such.
PASSING_STATUSES = {'Pass', 'QualityWarning', 'CompatibilityWarning'}

CASE_RESULT_RE = re.compile(
    r'#beginTestCaseResult (\S+).*?<Result StatusCode="(\w+)"',
    re.S)


def parse_qpa(path):
    """Map case name -> dEQP status code from a TestResults.qpa log."""
    with open(path) as handle:
        text = handle.read()
    results = {}
    for name, status in CASE_RESULT_RE.findall(text):
        results[name] = status
    return results


def load_baseline(path):
    if not os.path.exists(path):
        return set()
    cases = set()
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if line and not line.startswith('#'):
                cases.add(line)
    return cases


def write_baseline(path, cases):
    with open(path, 'w') as handle:
        handle.write('# Cases known to pass dEQP-VK through the remoting driver.\n')
        handle.write('# One fully qualified case name per line, sorted.\n')
        handle.write('# Regenerate a subset with:\n')
        handle.write('#   python3 tests/run_cts.py --caselist \'<pattern>\' --update\n')
        handle.write('# See tests/run_cts.py\'s docstring for what --update does to\n')
        handle.write('# entries outside the given caselist.\n')
        for case in sorted(cases):
            handle.write(case + '\n')


def run_deqp(deqp_path, caselist, log_path, port, timeout):
    """Run deqp-vk against the ICD, cwd set to the binary's own directory.

    deqp-vk looks up its data files (shaders, images) relative to its
    working directory, not its own path, so anywhere else it fails every
    case with a file-not-found rather than a real result.
    """
    deqp_dir = os.path.dirname(os.path.abspath(deqp_path))
    env = dict(os.environ)
    env['VK_DRIVER_FILES'] = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        'build', 'vulkan_remoting_icd.json')
    env['VK_REMOTING_HOST'] = '127.0.0.1'
    env['VK_REMOTING_PORT'] = str(port)

    cmd = [
        os.path.abspath(deqp_path),
        '--deqp-case=' + caselist,
        '--deqp-log-filename=' + log_path,
        '--deqp-log-images=disable',
        '--deqp-log-shader-sources=disable',
    ]
    result = subprocess.run(cmd, cwd=deqp_dir, env=env, capture_output=True,
                             text=True, timeout=timeout)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--caselist', required=True,
                         help="dEQP case filter, e.g. 'dEQP-VK.api.smoke.*'")
    parser.add_argument('--baseline', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'cts_baseline.txt'))
    parser.add_argument('--deqp', default=DEFAULT_DEQP,
                         help='path to the deqp-vk binary')
    parser.add_argument('--update', action='store_true',
                         help='rewrite the baseline from this run instead of just checking it')
    parser.add_argument('--server-port', type=int, default=24680)
    parser.add_argument('--no-server', action='store_true',
                         help='do not start a server; one must already be listening on '
                              '--server-port (useful to run several caselists against one '
                              'long-lived server and dodge the TIME_WAIT wait between runs)')
    parser.add_argument('--timeout', type=float, default=1800.0,
                         help='seconds to allow deqp-vk to run before giving up')
    args = parser.parse_args()

    if not os.path.exists(args.deqp):
        print('deqp-vk not found at {}'.format(args.deqp))
        return 2

    build_dir = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build')
    server_binary = os.path.join(build_dir, 'vulkan_remoting_server')

    log_path = '/tmp/cts_run_{}.qpa'.format(os.getpid())

    try:
        if args.no_server:
            result = run_deqp(args.deqp, args.caselist, log_path, args.server_port,
                               args.timeout)
        else:
            if not os.path.exists(server_binary):
                print('server binary not found at {}'.format(server_binary))
                return 2
            with Server(server_binary, args.server_port) as server:
                result = run_deqp(args.deqp, args.caselist, log_path, server.port,
                                   args.timeout)
    except Failure as error:
        print('server: {}'.format(error))
        return 2
    except subprocess.TimeoutExpired:
        print('deqp-vk did not finish within {:.0f}s'.format(args.timeout))
        return 2

    if not os.path.exists(log_path):
        print('deqp-vk produced no log; stderr follows:')
        print(result.stderr)
        return 2

    results = parse_qpa(log_path)
    if not results:
        print('no cases in {} matched {}'.format(args.deqp, args.caselist))
        return 2

    passing = {case for case, status in results.items() if status in PASSING_STATUSES}
    not_passing = {case for case, status in results.items() if status not in PASSING_STATUSES}

    baseline = load_baseline(args.baseline)
    # "In scope" means the caselist pattern covers the name, not "deqp-vk
    # happened to produce a result for it" - a baseline case that a rename,
    # removal or crash upstream makes vanish from the results must still
    # count as a regression, not be silently dropped from consideration.
    in_scope_baseline = {case for case in baseline
                          if fnmatch.fnmatchcase(case, args.caselist)}

    regressions = sorted(in_scope_baseline - passing)
    new_passes = sorted(passing - baseline)
    still_not_passing = sorted(not_passing - baseline)

    print('{} cases run ({} passing, {} not passing)'.format(
        len(results), len(passing), len(not_passing)))

    if regressions:
        print('\nREGRESSIONS ({}):'.format(len(regressions)))
        for case in regressions:
            print('  {}  ({})'.format(case, results.get(case, 'missing from run')))

    if new_passes:
        print('\nNEW PASSES ({}):'.format(len(new_passes)))
        for case in new_passes:
            print('  ' + case)

    if still_not_passing:
        print('\nSTILL NOT PASSING ({}, informational):'.format(len(still_not_passing)))
        for case in still_not_passing[:20]:
            print('  {}  ({})'.format(case, results[case]))
        if len(still_not_passing) > 20:
            print('  ... and {} more'.format(len(still_not_passing) - 20))

    if args.update:
        updated = (baseline - in_scope_baseline) | passing
        write_baseline(args.baseline, updated)
        print('\nbaseline updated: {} cases ({:+d})'.format(
            len(updated), len(updated) - len(baseline)))

    os.remove(log_path)

    if regressions:
        print('\n{} regression(s)'.format(len(regressions)))
        return 1
    print('\nno regressions')
    return 0


if __name__ == '__main__':
    sys.exit(main())
