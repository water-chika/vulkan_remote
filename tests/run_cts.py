#!/usr/bin/env python3
"""CTS regression tracking for the remoting driver.

Results are compared by case name against a reviewed known-pass baseline. Only a
known pass becoming non-passing is a regression. New passes are reported and are
adopted only with an explicit, scoped ``--update`` run.

For attributable runs, this runner selects the remoting ICD through both loader
overrides, can enable loader diagnostics and server validation, asserts an exact
matched-case count, reports every dEQP status separately, and can preserve the
QPA and server logs.
"""

import argparse
import collections
import fnmatch
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

DEFAULT_DEQP = ('/mnt/worktrees/VK-GL-CTS/build/external/vulkancts/modules/'
                'vulkan/deqp-vk')
PASSING_STATUSES = {'Pass', 'QualityWarning', 'CompatibilityWarning'}
CASE_RESULT_RE = re.compile(
    r'#beginTestCaseResult (\S+).*?<Result StatusCode="(\w+)"', re.S)


class Failure(Exception):
    pass


def parse_qpa(path):
    """Map case name to dEQP status code from a TestResults.qpa log."""
    with open(path) as handle:
        text = handle.read()
    return {name: status for name, status in CASE_RESULT_RE.findall(text)}


def load_baseline(path):
    if not os.path.exists(path):
        return set()
    with open(path) as handle:
        return {line.strip() for line in handle
                if line.strip() and not line.lstrip().startswith('#')}


def write_baseline(path, cases):
    with open(path, 'w') as handle:
        handle.write('# Cases known to pass dEQP-VK through the remoting driver.\n')
        handle.write('# One fully qualified case name per line, sorted.\n')
        handle.write('# Regenerate a reviewed subset with:\n')
        handle.write("#   python3 tests/run_cts.py --caselist '<pattern>' --update\n")
        handle.write('# Updates leave entries outside the selected scope untouched.\n')
        for case in sorted(cases):
            handle.write(case + '\n')


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


class CtsServer:
    def __init__(self, binary, port, validate, log_path):
        self.binary = binary
        self.port = port or free_port()
        self.validate = validate
        self.log_path = log_path
        self.process = None
        self.log_handle = None

    def start(self):
        server_env = dict(os.environ)
        for name in ('VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_REMOTING_HOST',
                     'VK_REMOTING_PORT', 'VK_LOADER_DEBUG'):
            server_env.pop(name, None)
        argv = [self.binary, '--port', str(self.port)]
        if self.validate:
            argv.append('--validate')
        self.log_handle = open(self.log_path, 'w')
        self.process = subprocess.Popen(
            argv, stdout=self.log_handle, stderr=subprocess.STDOUT,
            text=True, env=server_env)
        deadline = time.time() + 15.0
        while time.time() < deadline:
            if self.process.poll() is not None:
                self.stop()
                raise Failure('server exited early; see {}'.format(self.log_path))
            try:
                with socket.create_connection(('127.0.0.1', self.port), timeout=0.5):
                    return
            except OSError:
                time.sleep(0.2)
        self.stop()
        raise Failure('server did not start listening; see {}'.format(self.log_path))

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.log_handle:
            self.log_handle.close()
            self.log_handle = None


def run_deqp(deqp_path, caselist, log_path, port, timeout, manifest, loader_debug):
    """Run deqp-vk with its working directory set beside its data files."""
    env = dict(os.environ)
    env['VK_DRIVER_FILES'] = manifest
    env['VK_ICD_FILENAMES'] = manifest
    env['VK_REMOTING_HOST'] = '127.0.0.1'
    env['VK_REMOTING_PORT'] = str(port)
    if loader_debug:
        env['VK_LOADER_DEBUG'] = 'all'
    else:
        env.pop('VK_LOADER_DEBUG', None)
    cmd = [
        os.path.abspath(deqp_path),
        '--deqp-case=' + caselist,
        '--deqp-log-filename=' + log_path,
        '--deqp-log-images=disable',
        '--deqp-log-shader-sources=disable',
    ]
    return subprocess.run(
        cmd, cwd=os.path.dirname(os.path.abspath(deqp_path)), env=env,
        capture_output=True, text=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--caselist', required=True)
    parser.add_argument('--baseline', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'cts_baseline.txt'))
    parser.add_argument('--deqp', default=DEFAULT_DEQP)
    parser.add_argument('--update', action='store_true')
    parser.add_argument('--server-port', type=int, default=0,
                        help='server port; zero selects an ephemeral port')
    parser.add_argument('--no-server', action='store_true',
                        help='use an already-running server on --server-port')
    parser.add_argument('--validate-server', action='store_true')
    parser.add_argument('--loader-debug', action='store_true')
    parser.add_argument('--expected-count', type=int)
    parser.add_argument('--log', help='preserve the QPA at this path')
    parser.add_argument('--server-log', help='preserve server output at this path')
    parser.add_argument('--timeout', type=float, default=1800.0)
    args = parser.parse_args()

    if not os.path.exists(args.deqp):
        print('deqp-vk not found at {}'.format(args.deqp))
        return 2
    if args.expected_count is not None and args.expected_count <= 0:
        print('--expected-count must be positive')
        return 2
    if args.no_server and not args.server_port:
        print('--no-server requires a nonzero --server-port')
        return 2

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build_dir = os.path.join(root, 'build')
    server_binary = os.path.join(build_dir, 'vulkan_remoting_server')
    manifest = os.path.abspath(os.path.join(build_dir, 'vulkan_remoting_icd.json'))
    if not os.path.exists(manifest):
        print('ICD manifest not found at {}'.format(manifest))
        return 2

    temp_paths = []
    if args.log:
        log_path = os.path.abspath(args.log)
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
    else:
        fd, log_path = tempfile.mkstemp(prefix='cts-remoting-', suffix='.qpa')
        os.close(fd)
        os.remove(log_path)
        temp_paths.append(log_path)
    if args.server_log:
        server_log = os.path.abspath(args.server_log)
        os.makedirs(os.path.dirname(server_log), exist_ok=True)
    else:
        fd, server_log = tempfile.mkstemp(prefix='cts-server-', suffix='.log')
        os.close(fd)
        temp_paths.append(server_log)

    server = None
    result = None
    try:
        if args.no_server:
            port = args.server_port
        else:
            if not os.path.exists(server_binary):
                raise Failure('server binary not found at {}'.format(server_binary))
            server = CtsServer(server_binary, args.server_port,
                               args.validate_server, server_log)
            server.start()
            port = server.port
        result = run_deqp(args.deqp, args.caselist, log_path, port, args.timeout,
                          manifest, args.loader_debug)
    except (Failure, subprocess.TimeoutExpired) as error:
        print('CTS infrastructure failure: {}'.format(error))
        return 2
    finally:
        if server:
            server.stop()

    if result.stdout:
        print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, file=sys.stderr, end='')
    if result.returncode != 0:
        print('deqp-vk exited {}'.format(result.returncode))
        return 2
    if not os.path.exists(log_path):
        print('deqp-vk produced no QPA log')
        return 2

    results = parse_qpa(log_path)
    if not results:
        print('no cases matched {}'.format(args.caselist))
        return 2
    if args.expected_count is not None and len(results) != args.expected_count:
        print('case count mismatch: expected {}, got {}'.format(
            args.expected_count, len(results)))
        return 2

    histogram = collections.Counter(results.values())
    print('{} cases run'.format(len(results)))
    print('statuses: ' + ', '.join('{}={}'.format(name, histogram[name])
                                  for name in sorted(histogram)))

    if args.validate_server:
        with open(server_log) as handle:
            server_text = handle.read()
        if 'VK_LAYER_KHRONOS_validation enabled' not in server_text:
            print('server validation was requested but not enabled; see {}'.format(server_log))
            return 2
        if 'validation:' in server_text:
            print('server validation diagnostics found; see {}'.format(server_log))
            return 1

    passing = {case for case, status in results.items() if status in PASSING_STATUSES}
    not_passing = set(results) - passing
    baseline = load_baseline(args.baseline)
    in_scope_baseline = {case for case in baseline
                         if fnmatch.fnmatchcase(case, args.caselist)}
    regressions = sorted(in_scope_baseline - passing)
    new_passes = sorted(passing - baseline)
    still_not_passing = sorted(not_passing - baseline)

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

    if args.update and regressions:
        print('\nbaseline not updated because regressions were found')
    elif args.update:
        updated = (baseline - in_scope_baseline) | passing
        write_baseline(args.baseline, updated)
        print('\nbaseline updated: {} cases ({:+d})'.format(
            len(updated), len(updated) - len(baseline)))

    if args.log:
        print('QPA: ' + log_path)
    if args.server_log:
        print('server log: ' + server_log)
    for path in temp_paths:
        try:
            os.remove(path)
        except FileNotFoundError:
            pass

    if regressions:
        print('\n{} regression(s)'.format(len(regressions)))
        return 1
    print('\nno regressions')
    return 0


if __name__ == '__main__':
    sys.exit(main())
