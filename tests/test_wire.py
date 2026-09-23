#!/usr/bin/env python3
"""Failure-path tests for the Vulkan remoting wire.

The happy path is already demonstrated by the probe and by vulkaninfo. What
those do not cover is what a network does that a function call never does:
peers that vanish mid-call, peers built from a different registry, truncated
messages, and garbage. Every one of those must produce a clean error rather
than a crash, a hang, or a silent wrong answer, because on a socket they are
routine rather than exceptional.

Stdlib only, so it runs anywhere the server builds.
"""

import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import time

HEADER = struct.Struct('<II')

OPCODE_INVALID = 0
OPCODE_HANDSHAKE = 1

STATUS_OK = 0
STATUS_UNSUPPORTED = 1
STATUS_DECODE_ERROR = 2


class Failure(Exception):
    pass


def read_digest(build_dir):
    """The digest is generated, so the test must read it rather than hardcode it."""
    path = os.path.join(build_dir, 'remoting_commands.inl')
    with open(path) as handle:
        match = re.search(r'kCommandSetDigest = "([0-9a-f]+)"', handle.read())
    if not match:
        raise Failure('no command set digest in {}'.format(path))
    return match.group(1)


def read_opcode(build_dir, name):
    path = os.path.join(build_dir, 'remoting_commands.inl')
    with open(path) as handle:
        match = re.search(r'^\s*{} = (\d+),'.format(re.escape(name)), handle.read(), re.M)
    if not match:
        raise Failure('opcode {} not found'.format(name))
    return int(match.group(1))


def unhandled_opcode(build_dir):
    """A real Vulkan command the server has no handler for, found rather than named.

    Naming one here dates the test: this asserted on vkCreateBuffer, and the day
    that handler was written the test stopped exercising the unsupported path and
    started hanging on a real reply that never came.
    """
    server_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'server')
    registered = set()
    for entry in sorted(os.listdir(server_dir)):
        if entry.endswith('.cpp'):
            with open(os.path.join(server_dir, entry)) as handle:
                registered.update(re.findall(r'REGISTER_HANDLER\(\s*([A-Za-z0-9_]+)', handle.read()))

    with open(os.path.join(build_dir, 'remoting_commands.inl')) as handle:
        table = re.findall(r'^\s*(vk[A-Za-z0-9_]+) = (\d+),', handle.read(), re.M)

    for name, opcode in table:
        if name not in registered:
            return name, int(opcode)
    raise Failure('every command has a handler; this test needs a new premise')


def encode_string(text):
    raw = text.encode('utf-8')
    return struct.pack('<I', len(raw)) + raw


def decode_u32(payload, offset=0):
    return struct.unpack_from('<I', payload, offset)[0]


def send_message(sock, opcode, payload=b''):
    sock.sendall(HEADER.pack(opcode, len(payload)) + payload)


def recv_message(sock):
    header = b''
    while len(header) < HEADER.size:
        chunk = sock.recv(HEADER.size - len(header))
        if not chunk:
            return None, None
        header += chunk
    opcode, size = HEADER.unpack(header)

    payload = b''
    while len(payload) < size:
        chunk = sock.recv(size - len(payload))
        if not chunk:
            return None, None
        payload += chunk
    return opcode, payload


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def connect(port, host='127.0.0.1', timeout=5.0):
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


class Server:
    def __init__(self, binary, port):
        self.binary = binary
        self.port = port
        self.process = None

    def __enter__(self):
        self.process = subprocess.Popen(
            [self.binary, '--port', str(self.port)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.time() + 15.0
        while time.time() < deadline:
            if self.process.poll() is not None:
                output = self.process.communicate()[0]
                raise Failure('server exited early\n' + output)
            try:
                connect(self.port, timeout=0.5).close()
                return self
            except OSError:
                time.sleep(0.2)
        output = self.stop()
        raise Failure('server did not start listening\n' + output)

    def stop(self):
        if not self.process:
            return ''
        if self.process.poll() is None:
            self.process.terminate()
        try:
            return self.process.communicate(timeout=5)[0]
        except subprocess.TimeoutExpired:
            self.process.kill()
            return self.process.communicate()[0]

    def __exit__(self, *exc):
        self.stop()
        return False

    def alive(self):
        return self.process.poll() is None


def handshake(sock, digest):
    send_message(sock, OPCODE_HANDSHAKE, encode_string(digest))
    opcode, payload = recv_message(sock)
    if opcode != OPCODE_HANDSHAKE:
        raise Failure('handshake reply had opcode {}'.format(opcode))
    return decode_u32(payload)


def test_handshake_accepts_matching_digest(server, build_dir):
    with connect(server.port) as sock:
        status = handshake(sock, read_digest(build_dir))
        if status != STATUS_OK:
            raise Failure('server rejected its own digest (status {})'.format(status))


def test_handshake_rejects_foreign_digest(server, build_dir):
    """A peer built from a different vk.xml must be refused, not tolerated.

    Tolerating it would be the worst outcome: opcode numbering would differ, so
    every later call would invoke some other command and appear to work.
    """
    with connect(server.port) as sock:
        status = handshake(sock, 'deadbeefdeadbeef')
        if status == STATUS_OK:
            raise Failure('server accepted a foreign command set digest')

        send_message(sock, read_opcode(build_dir, 'vkEnumeratePhysicalDevices'))
        try:
            opcode, _ = recv_message(sock)
        except (ConnectionResetError, TimeoutError):
            opcode = None
        if opcode is not None:
            raise Failure('server kept serving a rejected peer')


def test_unknown_opcode_is_reported(server, build_dir):
    with connect(server.port) as sock:
        if handshake(sock, read_digest(build_dir)) != STATUS_OK:
            raise Failure('handshake failed')

        name, unhandled = unhandled_opcode(build_dir)
        send_message(sock, unhandled, b'\x00' * 8)
        opcode, payload = recv_message(sock)
        if opcode is None:
            raise Failure('server closed the connection on ' + name)
        if decode_u32(payload) != STATUS_UNSUPPORTED:
            raise Failure('{} did not report UnsupportedCommand'.format(name))


def test_reserved_opcode_zero_is_rejected(server, build_dir):
    with connect(server.port) as sock:
        if handshake(sock, read_digest(build_dir)) != STATUS_OK:
            raise Failure('handshake failed')
        send_message(sock, OPCODE_INVALID)
        opcode, payload = recv_message(sock)
        if opcode is None:
            raise Failure('server closed the connection on opcode 0')
        if decode_u32(payload) != STATUS_UNSUPPORTED:
            raise Failure('opcode 0 was not reported as unsupported')


def test_truncated_payload_does_not_hang(server, build_dir):
    """Announce more bytes than are sent, then disconnect."""
    sock = connect(server.port)
    sock.sendall(HEADER.pack(read_opcode(build_dir, 'vkGetPhysicalDeviceProperties'), 64))
    sock.sendall(b'\x01\x00')
    sock.close()

    time.sleep(0.5)
    if not server.alive():
        raise Failure('server died on a truncated message')


def test_oversized_payload_is_refused(server, build_dir):
    """A length field larger than the cap must not be believed.

    Without the cap this is a trivial denial of service: the peer says four
    gigabytes and the server tries to allocate it.
    """
    sock = connect(server.port)
    sock.sendall(HEADER.pack(read_opcode(build_dir, 'vkGetPhysicalDeviceProperties'), 0xFFFFFFFF))
    sock.close()

    time.sleep(0.5)
    if not server.alive():
        raise Failure('server died on an oversized length field')


def test_short_payload_for_known_command(server, build_dir):
    """A command whose payload is too short to decode must report, not guess."""
    with connect(server.port) as sock:
        if handshake(sock, read_digest(build_dir)) != STATUS_OK:
            raise Failure('handshake failed')

        send_message(sock, read_opcode(build_dir, 'vkGetPhysicalDeviceProperties'), b'\x01\x00\x00')
        opcode, payload = recv_message(sock)
        if opcode is None:
            raise Failure('server closed the connection on a short payload')
        if decode_u32(payload) != STATUS_DECODE_ERROR:
            raise Failure('short payload was not reported as a decode error')


def test_abrupt_disconnect_is_survivable(server, build_dir):
    """Half a message, then vanish: the common case when a machine sleeps."""
    sock = connect(server.port)
    sock.sendall(HEADER.pack(OPCODE_HANDSHAKE, 32)[:4])
    sock.close()

    time.sleep(0.5)
    if not server.alive():
        raise Failure('server died when a peer vanished mid-header')

    with connect(server.port) as good:
        if handshake(good, read_digest(build_dir)) != STATUS_OK:
            raise Failure('server stopped serving after a peer vanished')


def test_client_without_server_fails_cleanly(server, build_dir):
    """The ICD must report a Vulkan error, not crash, when nothing is listening.

    A driver that segfaults here would take the application down with it.
    """
    icd = os.path.join(build_dir, 'vulkan_remoting_icd.json')
    if not os.path.exists(icd):
        return 'skipped: ICD manifest not built'

    env = dict(os.environ)
    env['VK_DRIVER_FILES'] = icd
    env['VK_REMOTING_PORT'] = '24699'  # nothing listens here

    try:
        result = subprocess.run(['vulkaninfo', '--summary'], env=env, capture_output=True,
                                text=True, timeout=60)
    except FileNotFoundError:
        return 'skipped: vulkaninfo not installed'

    if result.returncode < 0:
        raise Failure('ICD crashed with signal {} when no server was present'.format(
            -result.returncode))
    if 'no server at' not in result.stdout + result.stderr:
        raise Failure('ICD did not explain the missing server')
    return None


def test_wsi_decode_errors_reply(server, build_dir):
    """Every synchronous WSI handler must answer malformed input promptly."""
    names = [
        'vkCreateWaylandSurfaceKHR',
        'vkCreateWin32SurfaceKHR',
        'vkGetPhysicalDeviceWaylandPresentationSupportKHR',
        'vkGetPhysicalDeviceSurfaceSupportKHR',
        'vkGetPhysicalDeviceSurfaceCapabilitiesKHR',
        'vkGetPhysicalDeviceSurfaceFormatsKHR',
        'vkGetPhysicalDeviceSurfacePresentModesKHR',
        'vkCreateSwapchainKHR',
        'vkGetSwapchainImagesKHR',
        'vkAcquireNextImageKHR',
        'vkQueuePresentKHR',
    ]
    digest = read_digest(build_dir)
    with connect(server.port) as sock:
        if handshake(sock, digest) != STATUS_OK:
            raise Failure('handshake failed')
        for name in names:
            expected_opcode = read_opcode(build_dir, name)
            send_message(sock, expected_opcode, b'\x01')
            opcode, response = recv_message(sock)
            if opcode != expected_opcode:
                raise Failure('{} reply had opcode {}'.format(name, opcode))
            if len(response) < 4 or decode_u32(response) != STATUS_DECODE_ERROR:
                raise Failure('{} did not report DecodeError'.format(name))


def test_own_window_surface(server, build_dir):
    """vkCreateWin32SurfaceKHR must reply, never hang, even without Wayland.

    The server under test was started with no --wayland and no guarantee of a
    real compositor being reachable, so this only proves the handler always
    sends back a reply for a request whose HWND cannot possibly mean anything
    here. If a compositor happens to be reachable it may report VK_SUCCESS;
    if not, VK_ERROR_INITIALIZATION_FAILED with handle 0 is just as
    acceptable - the property under test is "replies", not "a window
    appeared".
    """
    with connect(server.port) as sock:
        if handshake(sock, read_digest(build_dir)) != STATUS_OK:
            raise Failure('handshake failed')

        # hinstance, hwnd: both discarded server-side (see
        # handle_CreateWin32SurfaceKHR), but still sent so the payload has
        # the shape a real client would send.
        payload = struct.pack('<QQ', 0x1000, 0x2000)
        send_message(sock, read_opcode(build_dir, 'vkCreateWin32SurfaceKHR'), payload)
        opcode, response = recv_message(sock)
        if opcode is None:
            raise Failure('server did not reply to vkCreateWin32SurfaceKHR')
        if len(response) != 16:
            raise Failure('vkCreateWin32SurfaceKHR reply had {} bytes, expected 16'.format(
                len(response)))
        if decode_u32(response) != STATUS_OK:
            raise Failure('vkCreateWin32SurfaceKHR did not report Status::Ok')
        result = struct.unpack_from('<i', response, 4)[0]
        handle = struct.unpack_from('<Q', response, 8)[0]
        if (result == 0) != (handle != 0):
            raise Failure('vkCreateWin32SurfaceKHR result and handle disagree')


TESTS = [
    test_handshake_accepts_matching_digest,
    test_handshake_rejects_foreign_digest,
    test_unknown_opcode_is_reported,
    test_reserved_opcode_zero_is_rejected,
    test_truncated_payload_does_not_hang,
    test_oversized_payload_is_refused,
    test_short_payload_for_known_command,
    test_abrupt_disconnect_is_survivable,
    test_client_without_server_fails_cleanly,
    test_wsi_decode_errors_reply,
    test_own_window_surface,
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', default='build')
    parser.add_argument('--port', type=int, default=0,
                        help='base port; zero selects a fresh ephemeral port per test')
    args = parser.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    binary = os.path.join(build_dir, 'vulkan_remoting_server')
    if not os.path.exists(binary):
        print('server binary not found at {}'.format(binary))
        return 2

    failures = 0
    for index, test in enumerate(TESTS):
        name = test.__name__
        # A fresh server per test, so one test cannot mask another by leaving
        # the connection or the server in a strange state.
        try:
            port = args.port + index if args.port else free_port()
            with Server(binary, port) as server:
                note = test(server, build_dir)
            print('  ok   {}{}'.format(name, ' ({})'.format(note) if note else ''))
        except Failure as error:
            print('  FAIL {}: {}'.format(name, error))
            failures += 1
        except Exception as error:  # noqa: BLE001 - a test harness reports everything
            print('  FAIL {}: unexpected {}: {}'.format(name, type(error).__name__, error))
            failures += 1

    print('\n{} passed, {} failed'.format(len(TESTS) - failures, failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
