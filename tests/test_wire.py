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
import ctypes
import os
import re
import socket
import struct
import subprocess
import sys
import time

HEADER = struct.Struct('<IIQ')
NEXT_REQUEST_ID = 1

OPCODE_INVALID = 0
OPCODE_HANDSHAKE = 1

STATUS_OK = 0
STATUS_UNSUPPORTED = 1
STATUS_DECODE_ERROR = 2

VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x00000002
VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x00000004
VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD = 0x00000040
VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD = 0x00000080


class Failure(Exception):
    pass


def read_protocol_constants(build_dir):
    """Read generated compatibility values rather than duplicating them in tests."""
    path = os.path.join(build_dir, 'remoting_commands.inl')
    with open(path) as handle:
        generated = handle.read()
    values = {}
    for name in ('kWireSchemaRevision', 'kRegistrySha256', 'kWireAbi',
                 'kCommandSetDigest'):
        match = re.search(r'{} = "([^"]+)"'.format(name), generated)
        if not match:
            raise Failure('no {} in {}'.format(name, path))
        values[name] = match.group(1)
    return values


def read_digest(build_dir):
    return read_protocol_constants(build_dir)['kCommandSetDigest']


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


def pack_blob(data):
    return struct.pack('<I', len(data)) + data


def decode_u32(payload, offset=0):
    return struct.unpack_from('<I', payload, offset)[0]


def decode_string(payload, offset):
    size = decode_u32(payload, offset)
    offset += 4
    if offset + size > len(payload):
        raise Failure('truncated encoded string')
    return payload[offset:offset + size].decode('utf-8'), offset + size


def send_message(sock, opcode, payload=b'', request_id=None):
    global NEXT_REQUEST_ID
    if request_id is None:
        request_id = NEXT_REQUEST_ID
        NEXT_REQUEST_ID += 1
    sock.sendall(HEADER.pack(opcode, len(payload), request_id) + payload)


def recv_message_with_id(sock):
    header = b''
    while len(header) < HEADER.size:
        chunk = sock.recv(HEADER.size - len(header))
        if not chunk:
            return None, None, None
        header += chunk
    opcode, size, request_id = HEADER.unpack(header)

    payload = b''
    while len(payload) < size:
        chunk = sock.recv(size - len(payload))
        if not chunk:
            return None, None, None
        payload += chunk
    return opcode, request_id, payload


def recv_message(sock):
    opcode, _, payload = recv_message_with_id(sock)
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


def handshake(sock, digest, build_dir=None, overrides=None,
              supported_capabilities=0, required_capabilities=0):
    values = read_protocol_constants(build_dir) if build_dir else {
        'kWireSchemaRevision': 'wire-v15-vulkan12-internal',
        'kRegistrySha256': '0' * 64,
        'kWireAbi': 'x86_64-little-endian-v1',
    }
    if overrides:
        values.update(overrides)
    payload = (encode_string(values['kWireSchemaRevision']) +
               encode_string(values['kRegistrySha256']) +
               encode_string(values['kWireAbi']) +
               encode_string(digest) +
               struct.pack('<II', supported_capabilities, required_capabilities))
    send_message(sock, OPCODE_HANDSHAKE, payload)
    opcode, payload = recv_message(sock)
    if opcode != OPCODE_HANDSHAKE:
        raise Failure('handshake reply had opcode {}'.format(opcode))
    return decode_u32(payload)


def test_handshake_accepts_matching_digest(server, build_dir):
    with connect(server.port) as sock:
        status = handshake(sock, read_digest(build_dir), build_dir)
        if status != STATUS_OK:
            raise Failure('server rejected its own digest (status {})'.format(status))


def test_ping_echoes_request_id(server, build_dir):
    with connect(server.port) as sock:
        if handshake(sock, read_digest(build_dir), build_dir) != STATUS_OK:
            raise Failure('handshake failed before Ping')
        ping = read_opcode(build_dir, 'Ping')
        request_id = 0x1020304050607080
        send_message(sock, ping, b'', request_id)
        opcode, reply_id, response = recv_message_with_id(sock)
        if (opcode, reply_id) != (ping, request_id):
            raise Failure('Ping correlation was {!r}, expected {!r}'.format(
                (opcode, reply_id), (ping, request_id)))
        if decode_u32(response) != STATUS_OK:
            raise Failure('valid Ping was rejected')
        send_message(sock, ping, b'x', request_id + 1)
        opcode, reply_id, response = recv_message_with_id(sock)
        if (opcode, reply_id, decode_u32(response)) != (
                ping, request_id + 1, STATUS_DECODE_ERROR):
            raise Failure('malformed Ping did not return correlated DecodeError')


def test_replies_echo_request_ids(server, build_dir):
    with connect(server.port) as sock:
        values = read_protocol_constants(build_dir)
        payload = (encode_string(values['kWireSchemaRevision']) +
                   encode_string(values['kRegistrySha256']) +
                   encode_string(values['kWireAbi']) +
                   encode_string(values['kCommandSetDigest']) +
                   struct.pack('<II', 0, 0))
        request_id = 0x123456789abcdef0
        send_message(sock, OPCODE_HANDSHAKE, payload, request_id)
        opcode, reply_id, response = recv_message_with_id(sock)
        if (opcode, reply_id) != (OPCODE_HANDSHAKE, request_id):
            raise Failure('reply correlation was opcode/id {!r}, expected {!r}'.format(
                (opcode, reply_id), (OPCODE_HANDSHAKE, request_id)))
        if decode_u32(response) != STATUS_OK:
            raise Failure('correlated handshake was rejected')


def assert_handshake_rejected_and_closed(sock, status, build_dir, description):
    if status == STATUS_OK:
        raise Failure('server accepted ' + description)
    send_message(sock, read_opcode(build_dir, 'vkEnumeratePhysicalDevices'))
    try:
        opcode, _ = recv_message(sock)
    except (BrokenPipeError, ConnectionResetError, TimeoutError):
        opcode = None
    if opcode is not None:
        raise Failure('server kept serving ' + description)


def test_handshake_rejects_unknown_required_capability(server, build_dir):
    with connect(server.port) as sock:
        status = handshake(sock, read_digest(build_dir), build_dir,
                           supported_capabilities=0x80000000,
                           required_capabilities=0x80000000)
        assert_handshake_rejected_and_closed(
            sock, status, build_dir, 'an unknown required capability')


def test_handshake_rejects_foreign_digest(server, build_dir):
    """A peer built from a different command set must be refused, not tolerated."""
    with connect(server.port) as sock:
        status = handshake(sock, 'deadbeefdeadbeef', build_dir)
        assert_handshake_rejected_and_closed(
            sock, status, build_dir, 'a foreign command set digest')


def test_handshake_rejects_schema_mismatch(server, build_dir):
    """Schema changes without opcode changes must still reject the peer."""
    with connect(server.port) as sock:
        status = handshake(sock, read_digest(build_dir), build_dir,
                           {'kWireSchemaRevision': 'wire-incompatible-test-schema'})
        assert_handshake_rejected_and_closed(
            sock, status, build_dir, 'a foreign wire schema')


def test_unknown_opcode_is_reported(server, build_dir):
    with connect(server.port) as sock:
        if handshake(sock, read_digest(build_dir), build_dir) != STATUS_OK:
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
        if handshake(sock, read_digest(build_dir), build_dir) != STATUS_OK:
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
    sock.sendall(HEADER.pack(read_opcode(build_dir, 'vkGetPhysicalDeviceProperties'), 64, 1))
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
    sock.sendall(HEADER.pack(read_opcode(build_dir, 'vkGetPhysicalDeviceProperties'), 0xFFFFFFFF, 1))
    sock.close()

    time.sleep(0.5)
    if not server.alive():
        raise Failure('server died on an oversized length field')


def test_short_payload_for_known_command(server, build_dir):
    """A command whose payload is too short to decode must report, not guess."""
    with connect(server.port) as sock:
        if handshake(sock, read_digest(build_dir), build_dir) != STATUS_OK:
            raise Failure('handshake failed')

        send_message(sock, read_opcode(build_dir, 'vkGetPhysicalDeviceProperties'), b'\x01\x00\x00')
        opcode, payload = recv_message(sock)
        if opcode is None:
            raise Failure('server closed the connection on a short payload')
        if decode_u32(payload) != STATUS_DECODE_ERROR:
            raise Failure('short payload was not reported as a decode error')


def test_pipeline_counts_are_bounded(server, build_dir):
    """Hostile outer createInfoCount values get synchronous DecodeError replies."""
    digest = read_digest(build_dir)
    for name in ('vkCreateGraphicsPipelines', 'vkCreateComputePipelines'):
        with connect(server.port) as sock:
            if handshake(sock, digest, build_dir) != STATUS_OK:
                raise Failure('handshake failed')

            expected_opcode = read_opcode(build_dir, name)
            count = (1 << 20) + 1
            # Supply enough bytes for the handler's minimum per-create-info
            # check, proving the explicit element cap rejects this count.
            payload = struct.pack('<QQI', 0, 0, count) + b'\0' * (count * 4)
            send_message(sock, expected_opcode, payload)
            opcode, response = recv_message(sock)
            if opcode != expected_opcode:
                raise Failure('{} reply had opcode {}'.format(name, opcode))
            if len(response) < 4 or decode_u32(response) != STATUS_DECODE_ERROR:
                raise Failure('{} hostile pipeline count was not rejected'.format(name))


def test_graphics_pipeline_nested_counts_are_bounded(server, build_dir):
    """Nested vertex-input counts must agree with their bounded wire blobs."""
    class GraphicsPipelineCreateInfo(ctypes.Structure):
        _fields_ = [
            ('sType', ctypes.c_int32), ('pNext', ctypes.c_void_p),
            ('flags', ctypes.c_uint32), ('stageCount', ctypes.c_uint32),
            ('pStages', ctypes.c_void_p), ('pVertexInputState', ctypes.c_void_p),
            ('pInputAssemblyState', ctypes.c_void_p),
            ('pTessellationState', ctypes.c_void_p),
            ('pViewportState', ctypes.c_void_p),
            ('pRasterizationState', ctypes.c_void_p),
            ('pMultisampleState', ctypes.c_void_p),
            ('pDepthStencilState', ctypes.c_void_p),
            ('pColorBlendState', ctypes.c_void_p),
            ('pDynamicState', ctypes.c_void_p), ('layout', ctypes.c_uint64),
            ('renderPass', ctypes.c_uint64), ('subpass', ctypes.c_uint32),
            ('basePipelineHandle', ctypes.c_uint64),
            ('basePipelineIndex', ctypes.c_int32),
        ]

    class VertexInputState(ctypes.Structure):
        _fields_ = [
            ('sType', ctypes.c_int32), ('pNext', ctypes.c_void_p),
            ('flags', ctypes.c_uint32),
            ('vertexBindingDescriptionCount', ctypes.c_uint32),
            ('pVertexBindingDescriptions', ctypes.c_void_p),
            ('vertexAttributeDescriptionCount', ctypes.c_uint32),
            ('pVertexAttributeDescriptions', ctypes.c_void_p),
        ]

    if (ctypes.sizeof(ctypes.c_void_p) != 8 or
            ctypes.sizeof(GraphicsPipelineCreateInfo) != 144 or
            ctypes.sizeof(VertexInputState) != 48):
        raise Failure('wire test Vulkan structure layout is not the required x86-64 ABI')

    digest = read_digest(build_dir)
    opcode = read_opcode(build_dir, 'vkCreateGraphicsPipelines')
    outer = GraphicsPipelineCreateInfo()
    outer.pVertexInputState = 1
    for description, binding_count, attribute_count in (
            ('binding', 1, 0), ('attribute', 0, 1)):
        vertex = VertexInputState()
        vertex.vertexBindingDescriptionCount = binding_count
        vertex.vertexAttributeDescriptionCount = attribute_count
        # After the two deliberately inconsistent blobs, append the complete
        # otherwise-valid tail: absent input/tessellation/viewport/raster/
        # multisample/depth/color/dynamic states and empty graphics pNext.
        tail = struct.pack('<IIIIIIIII', 0, 0, 0, 0, 0, 0, 0, 0, 0)
        payload = (struct.pack('<QQI', 0, 0, 1) + pack_blob(bytes(outer)) +
                   struct.pack('<I', 1) + pack_blob(bytes(vertex)) +
                   pack_blob(b'') + pack_blob(b'') + tail)
        with connect(server.port) as sock:
            if handshake(sock, digest, build_dir) != STATUS_OK:
                raise Failure('handshake failed')
            send_message(sock, opcode, payload)
            reply_opcode, response = recv_message(sock)
            if (reply_opcode != opcode or len(response) < 4 or
                    decode_u32(response) != STATUS_DECODE_ERROR):
                raise Failure('nested vertex-input {} count was not rejected'.format(
                    description))


def test_indexed_binding_payloads_are_rejected(server, build_dir):
    """Malformed and invalid-handle indexed binding requests remain safe one-way calls."""
    digest = read_digest(build_dir)
    cases = (
        # A small, plausible count whose single buffer/offset entry is truncated.
        ('truncated vertex binding', 'vkCmdBindVertexBuffers',
         struct.pack('<QII', 0, 0, 1)),
        # Complete vertex-binding shape with a nonzero unresolved buffer id.
        ('invalid vertex buffer', 'vkCmdBindVertexBuffers',
         struct.pack('<QIIQQ', 1, 0, 1, 0xBAD, 0)),
        # Complete index-binding shape with a nonzero unresolved buffer id.
        ('invalid index buffer', 'vkCmdBindIndexBuffer',
         struct.pack('<QQQi', 1, 0xBAD, 0, 0)),
    )
    for description, name, payload in cases:
        with connect(server.port) as sock:
            if handshake(sock, digest, build_dir) != STATUS_OK:
                raise Failure('handshake failed for {}'.format(description))
            send_message(sock, read_opcode(build_dir, name), payload)
        if not server.alive():
            raise Failure('server died on {}'.format(description))

    with connect(server.port) as good:
        if handshake(good, digest, build_dir) != STATUS_OK:
            raise Failure('server stopped serving after malformed indexed bindings')


def test_push_constant_and_descriptor_payloads_are_rejected(server, build_dir):
    """Current push/descriptor hardening rejects hostile one-way payloads safely."""
    digest = read_digest(build_dir)
    max_elements = (1 << 20)
    push = read_opcode(build_dir, 'vkCmdPushConstants')
    bind = read_opcode(build_dir, 'vkCmdBindDescriptorSets')
    update = read_opcode(build_dir, 'vkUpdateDescriptorSets')

    class WriteDescriptorSet(ctypes.Structure):
        _fields_ = [
            ('sType', ctypes.c_int32), ('pNext', ctypes.c_void_p),
            ('dstSet', ctypes.c_uint64), ('dstBinding', ctypes.c_uint32),
            ('dstArrayElement', ctypes.c_uint32), ('descriptorCount', ctypes.c_uint32),
            ('descriptorType', ctypes.c_int32), ('pImageInfo', ctypes.c_void_p),
            ('pBufferInfo', ctypes.c_void_p), ('pTexelBufferView', ctypes.c_void_p),
        ]

    if ctypes.sizeof(WriteDescriptorSet) != 64:
        raise Failure('wire test VkWriteDescriptorSet layout is not x86-64 ABI')

    write = WriteDescriptorSet()
    write.sType = 35  # VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
    write.dstSet = 1
    write.descriptorCount = 1
    write.descriptorType = 6  # VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    raw_write = pack_blob(bytes(write))
    update_prefix = struct.pack('<QI', 0, 1) + raw_write

    cases = (
        ('push oversized', push, struct.pack('<QQIII', 1, 1, 1, 0, 65537)),
        ('push truncated', push, struct.pack('<QQIII', 1, 1, 1, 0, 4) + b'abc'),
        ('push misaligned', push, struct.pack('<QQIII', 1, 1, 1, 0, 3) + b'abc'),
        ('push trailing', push, struct.pack('<QQIII', 1, 1, 1, 0, 4) + b'abcd!'),
        ('push invalid layout', push, struct.pack('<QQIII', 1, 0xBAD, 1, 0, 4) + b'abcd'),
        ('bind descriptor count cap', bind,
         struct.pack('<QiQII', 1, 0, 1, 0, max_elements + 1)),
        ('bind descriptor truncation', bind,
         struct.pack('<QiQII', 1, 0, 1, 0, 1)),
        ('bind descriptor invalid handles', bind,
         struct.pack('<QiQIIQI', 1, 0, 0xBAD, 0, 1, 0xBAD, 0)),
        ('update outer count cap', update, struct.pack('<QI', 0, max_elements + 1)),
        ('update discriminator mismatch', update, update_prefix + struct.pack('<I', 1)),
        ('update descriptorCount cap', update,
         struct.pack('<QI', 0, 1) + pack_blob(bytes(WriteDescriptorSet(
             sType=35, dstSet=1, descriptorCount=max_elements + 1,
             descriptorType=6))) + struct.pack('<I', 2)),
        ('update truncated buffer array', update, update_prefix + struct.pack('<I', 2)),
        ('update invalid required handles', update,
         update_prefix + struct.pack('<IQQQ', 2, 0, 0, 64) + struct.pack('<I', 0)),
        ('update trailing', update,
         update_prefix + struct.pack('<IQQQ', 2, 1, 0, 64) + struct.pack('<I', 0) + b'!'),
    )
    for description, opcode, payload in cases:
        with connect(server.port) as sock:
            if handshake(sock, digest, build_dir) != STATUS_OK:
                raise Failure('handshake failed for {}'.format(description))
            send_message(sock, opcode, payload)

    time.sleep(0.2)
    if not server.alive():
        raise Failure('server died on hardened push/descriptor payloads')
    with connect(server.port) as good:
        if handshake(good, digest, build_dir) != STATUS_OK:
            raise Failure('server stopped serving after push/descriptor payloads')


def test_shader_module_malformed_payloads_are_rejected(server, build_dir):
    """Field-wise shader decoding rejects inconsistent data before Vulkan."""
    digest = read_digest(build_dir)
    shader_opcode = read_opcode(build_dir, 'vkCreateShaderModule')
    shader_stype = 16  # VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO

    def shader_payload(code_size, code=b'', pnext=b'', flags=0):
        return (struct.pack('<QiII', 0, shader_stype, flags,
                            1 if pnext else 0) + pnext +
                struct.pack('<Q', code_size) + code)

    cases = {
        'zero code size': shader_payload(0),
        'non-word-aligned code size': shader_payload(5, b'12345'),
        'oversized code size': shader_payload(16 * 1024 * 1024 + 4),
        'declared/payload mismatch': shader_payload(8, struct.pack('<I', 0x07230203)),
        'truncated word': shader_payload(4, struct.pack('<I', 0x07230203)[:3]),
        'trailing data': shader_payload(4, struct.pack('<II', 0x07230203, 0xDEADBEEF)),
        'unsupported pNext': shader_payload(
            4, struct.pack('<I', 0x07230203),
            struct.pack('<i', 0)),  # VK_STRUCTURE_TYPE_APPLICATION_INFO
    }

    for description, payload in cases.items():
        with connect(server.port) as sock:
            if handshake(sock, digest, build_dir) != STATUS_OK:
                raise Failure('handshake failed')
            send_message(sock, shader_opcode, payload)
            opcode, response = recv_message(sock)
            if opcode != shader_opcode or len(response) < 4:
                raise Failure('{} produced a malformed reply'.format(description))
            if decode_u32(response) != STATUS_DECODE_ERROR:
                raise Failure('{} was not rejected'.format(description))


def test_submit_and_fence_payloads_are_rejected(server, build_dir):
    """Submit and fence RPCs reject hostile counts, ids, and trailing bytes."""
    digest = read_digest(build_dir)
    max_elements = 1 << 20
    cases = (
        ('queue submit outer count cap', 'vkQueueSubmit',
         struct.pack('<QQI', 0, 0, max_elements + 1)),
        ('queue submit nested wait count cap', 'vkQueueSubmit',
         struct.pack('<QQIII', 0, 0, 1, 0, max_elements + 1)),
        ('queue submit unresolved queue', 'vkQueueSubmit',
         struct.pack('<QQIIIII', 0xBAD, 0, 1, 0, 0, 0, 0)),
        ('queue submit unresolved fence', 'vkQueueSubmit',
         struct.pack('<QQI', 0, 0xBAD, 0)),
        ('queue submit unresolved wait semaphore', 'vkQueueSubmit',
         struct.pack('<QQIIIQIII', 0, 0, 1, 0, 1, 0xBAD, 0, 0, 0)),
        ('queue submit unresolved command buffer', 'vkQueueSubmit',
         struct.pack('<QQIIIIQ', 0, 0, 1, 0, 0, 1, 0xBAD) + struct.pack('<I', 0)),
        ('queue submit unresolved signal semaphore', 'vkQueueSubmit',
         struct.pack('<QQIIIIIQ', 0, 0, 1, 0, 0, 0, 1, 0xBAD)),
        ('queue submit trailing data', 'vkQueueSubmit',
         struct.pack('<QQI', 0, 0, 0) + b'!'),
        ('reset fences count cap', 'vkResetFences',
         struct.pack('<QI', 0, max_elements + 1)),
        ('reset fences unresolved id', 'vkResetFences',
         struct.pack('<QIQ', 0, 1, 0xBAD)),
        ('reset fences trailing data', 'vkResetFences',
         struct.pack('<QI', 0, 0) + b'!'),
        ('wait fences count cap', 'vkWaitForFences',
         struct.pack('<QII', 0, 0, max_elements + 1)),
        ('wait fences unresolved id', 'vkWaitForFences',
         struct.pack('<QIIQQ', 0, 0, 1, 0xBAD, 0)),
        ('wait fences trailing data', 'vkWaitForFences',
         struct.pack('<QIIQQ', 0, 0, 1, 0xBAD, 0) + b'!'),
        ('fence status unresolved id', 'vkGetFenceStatus',
         struct.pack('<QQ', 0, 0xBAD)),
        ('fence status trailing data', 'vkGetFenceStatus',
         struct.pack('<QQ', 0, 0xBAD) + b'!'),
    )
    for description, name, payload in cases:
        with connect(server.port) as sock:
            if handshake(sock, digest, build_dir) != STATUS_OK:
                raise Failure('handshake failed for {}'.format(description))
            expected = read_opcode(build_dir, name)
            send_message(sock, expected, payload)
            opcode, response = recv_message(sock)
            if opcode != expected or len(response) < 4:
                raise Failure('{} produced no synchronous error reply'.format(description))
            if decode_u32(response) != STATUS_DECODE_ERROR:
                raise Failure('{} was not rejected'.format(description))


def test_vulkan10_family_payloads_are_bounded(server, build_dir):
    """New object/query/sparse families reject hostile counts and bad handles."""
    digest = read_digest(build_dir)
    cases = {
        'vkMergePipelineCaches': struct.pack('<QQI', 0, 0, 0xFFFFFFFF),
        'vkFreeDescriptorSets': struct.pack('<QQI', 0, 0, 0xFFFFFFFF),
        'vkGetQueryPoolResults': struct.pack('<QQIIQQI', 0, 0, 0, 1,
                                                    64 * 1024 * 1024 + 1, 8, 0),
        'vkQueueBindSparse': struct.pack('<QQI', 0, 0, 0xFFFFFFFF),
    }
    for name, payload in cases.items():
        with connect(server.port) as sock:
            if handshake(sock, digest, build_dir) != STATUS_OK:
                raise Failure('handshake failed')
            expected = read_opcode(build_dir, name)
            send_message(sock, expected, payload)
            opcode, response = recv_message(sock)
            if opcode != expected or len(response) < 4:
                raise Failure('{} produced no bounded error reply'.format(name))
            if decode_u32(response) != STATUS_DECODE_ERROR:
                raise Failure('{} did not reject malformed payload'.format(name))


def test_transfer_command_payloads_are_bounded(server, build_dir):
    """Hostile transfer counts and byte lengths must not allocate or reach Vulkan."""
    digest = read_digest(build_dir)
    with connect(server.port) as sock:
        if handshake(sock, digest, build_dir) != STATUS_OK:
            raise Failure('handshake failed')

        # command buffer, source image, source layout, destination image,
        # destination layout, and an impossible region count with no regions.
        hostile_regions = struct.pack('<QQiQiI', 0, 0, 0, 0, 0, 0xFFFFFFFF)
        send_message(sock, read_opcode(build_dir, 'vkCmdCopyImage'), hostile_regions)
        send_message(sock, read_opcode(build_dir, 'vkCmdBlitImage'), hostile_regions)

        # command buffer, destination buffer, offset, declared Vulkan dataSize,
        # and a blob length larger than the handler's 64 KiB allocation cap.
        hostile_update = struct.pack('<QQQQI', 0, 0, 0, 65536, 0xFFFFFFFF)
        send_message(sock, read_opcode(build_dir, 'vkCmdUpdateBuffer'), hostile_update)

    time.sleep(0.2)
    if not server.alive():
        raise Failure('server died on malformed transfer command payloads')
    with connect(server.port) as good:
        if handshake(good, digest, build_dir) != STATUS_OK:
            raise Failure('server stopped serving after malformed transfer commands')


def decode_memory_properties(response):
    if len(response) < 8 or decode_u32(response) != STATUS_OK:
        raise Failure('memory-properties reply was malformed')
    type_count = decode_u32(response, 4)
    if type_count > 32:
        raise Failure('memory-properties type count exceeded Vulkan limit')
    offset = 8
    types = []
    for _ in range(type_count):
        if offset + 8 > len(response):
            raise Failure('truncated memory type')
        types.append(struct.unpack_from('<II', response, offset))
        offset += 8
    if offset + 4 > len(response):
        raise Failure('truncated memory heap count')
    heap_count = decode_u32(response, offset)
    offset += 4
    if heap_count > 16 or offset + heap_count * 12 != len(response):
        raise Failure('malformed memory heap array')
    heaps = []
    for _ in range(heap_count):
        heaps.append(struct.unpack_from('<QI', response, offset))
        offset += 12
    return types, heaps


def test_memory_properties_hide_host_coherent(server, build_dir):
    """Legacy and properties2 expose the same non-coherent public memory view."""
    digest = read_digest(build_dir)
    replies = []
    for name in ('vkGetPhysicalDeviceMemoryProperties',
                 'vkGetPhysicalDeviceMemoryProperties2'):
        with connect(server.port) as sock:
            if handshake(sock, digest, build_dir) != STATUS_OK:
                raise Failure('handshake failed')
            opcode = read_opcode(build_dir, name)
            send_message(sock, opcode, struct.pack('<Q', 1))
            reply_opcode, response = recv_message(sock)
            if reply_opcode != opcode:
                raise Failure('{} reply had opcode {}'.format(name, reply_opcode))
            replies.append(decode_memory_properties(response))
    if replies[0] != replies[1]:
        raise Failure('legacy and properties2 memory properties differ')
    types, _ = replies[0]
    hidden = (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
              VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
              VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)
    if any(flags & hidden for flags, _ in types):
        raise Failure('remote memory properties exposed coherence-dependent flags')
    if not any(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT for flags, _ in types):
        raise Failure('remote memory properties lost every HOST_VISIBLE type')


def test_device_extension_advertisement_is_real_device_intersection(server, build_dir):
    """The remote list is exactly swapchain plus the closed reviewed contract."""
    with connect(server.port) as sock:
        if handshake(sock, read_digest(build_dir), build_dir) != STATUS_OK:
            raise Failure('handshake failed')
        opcode = read_opcode(build_dir, 'vkEnumerateDeviceExtensionProperties')
        send_message(sock, opcode, struct.pack('<Q', 1))
        reply_opcode, response = recv_message(sock)
        if reply_opcode != opcode or len(response) < 12:
            raise Failure('device-extension enumeration reply was malformed')
        if decode_u32(response) != STATUS_OK:
            raise Failure('device-extension enumeration did not report Status::Ok')
        result = struct.unpack_from('<i', response, 4)[0]
        if result != 0:
            raise Failure('device-extension enumeration returned {}'.format(result))
        count = decode_u32(response, 8)
        offset = 12
        names = []
        for _ in range(count):
            name, offset = decode_string(response, offset)
            if offset + 4 > len(response):
                raise Failure('truncated extension version')
            offset += 4
            names.append(name)
        if offset != len(response):
            raise Failure('trailing bytes in device-extension enumeration')

        contract = {
            'VK_KHR_multiview',
            'VK_KHR_maintenance2',
            'VK_KHR_image_format_list',
            'VK_KHR_create_renderpass2',
            'VK_KHR_fragment_shading_rate',
            'VK_KHR_imageless_framebuffer',
        }
        advertised_contract = contract.intersection(names)
        if advertised_contract and advertised_contract != contract:
            raise Failure('server advertised an incomplete extension contract: {}'.format(
                sorted(advertised_contract)))
        unexpected = set(names) - contract - {
            'VK_KHR_swapchain', 'VK_KHR_synchronization2',
            'VK_KHR_buffer_device_address'}
        if unexpected:
            raise Failure('server exposed unreviewed device extensions: {}'.format(
                sorted(unexpected)))
        if 'VK_KHR_dynamic_rendering' in names:
            raise Failure('server exposed dynamic rendering')


def test_new_extension_opcode_decode_errors(server, build_dir):
    """New synchronous and one-way commands reject malformed wire safely."""
    digest = read_digest(build_dir)
    with connect(server.port) as sock:
        if handshake(sock, digest, build_dir) != STATUS_OK:
            raise Failure('handshake failed')

        synchronous = [
            'vkCreateRenderPass2',
            'vkGetPhysicalDeviceFragmentShadingRatesKHR',
        ]
        for name in synchronous:
            expected_opcode = read_opcode(build_dir, name)
            send_message(sock, expected_opcode, b'\x01')
            opcode, response = recv_message(sock)
            if opcode != expected_opcode:
                raise Failure('{} reply had opcode {}'.format(name, opcode))
            if len(response) < 4 or decode_u32(response) != STATUS_DECODE_ERROR:
                raise Failure('{} did not report DecodeError'.format(name))

        one_way = [
            'vkCmdBeginRenderPass2',
            'vkCmdNextSubpass2',
            'vkCmdEndRenderPass2',
            'vkCmdSetFragmentShadingRateKHR',
        ]
        for name in one_way:
            send_message(sock, read_opcode(build_dir, name), b'\x01')

    # One-way commands have no immediate reply by design. Their malformed
    # payloads must neither call Vulkan nor terminate the process; prove the
    # server remains available to a fresh, valid peer.
    if not server.alive():
        raise Failure('server died on malformed one-way extension commands')
    with connect(server.port) as good:
        if handshake(good, digest, build_dir) != STATUS_OK:
            raise Failure('server stopped serving after malformed extension commands')



def test_vulkan12_payloads_are_bounded(server, build_dir):
    """Vulkan 1.2 counted arrays and object ids reject malformed requests."""
    digest = read_digest(build_dir)
    cases = {
        'vkWaitSemaphores': struct.pack('<QII', 0, 0, 0xFFFFFFFF),
        'vkSignalSemaphore': struct.pack('<QQQ', 0, 0, 1),
        'vkResetQueryPool': struct.pack('<QQII', 0, 0, 0, 1),
    }
    for name, payload in cases.items():
        with connect(server.port) as sock:
            if handshake(sock, digest, build_dir) != STATUS_OK:
                raise Failure('handshake failed')
            opcode = read_opcode(build_dir, name)
            send_message(sock, opcode, payload)
            if name == 'vkResetQueryPool':
                continue
            reply_opcode, response = recv_message(sock)
            if reply_opcode != opcode or len(response) < 4 or decode_u32(response) != STATUS_DECODE_ERROR:
                raise Failure('{} did not reject malformed payload'.format(name))
    if not server.alive():
        raise Failure('server died on malformed Vulkan 1.2 payloads')

def test_mapped_transfer_chunks_are_bounded(server, build_dir):
    """Mapped-memory RPCs reject malformed and over-limit chunks synchronously."""
    digest = read_digest(build_dir)
    with connect(server.port) as sock:
        if handshake(sock, digest, build_dir) != STATUS_OK:
            raise Failure('handshake failed')

        flush_opcode = read_opcode(build_dir, 'FlushMappedMemory')
        # device, memory, offset, size, then a blob length larger than the wire cap.
        payload = struct.pack('<QQQQI', 0, 0, 0, 64 * 1024 * 1024, 0xFFFFFFFF)
        send_message(sock, flush_opcode, payload)
        opcode, response = recv_message(sock)
        if opcode != flush_opcode or decode_u32(response) != STATUS_DECODE_ERROR:
            raise Failure('oversized mapped flush was not rejected')

        download_opcode = read_opcode(build_dir, 'DownloadMappedMemory')
        payload = struct.pack('<QQQQ', 0, 0, 0, 64 * 1024 * 1024)
        send_message(sock, download_opcode, payload)
        opcode, response = recv_message(sock)
        if opcode != download_opcode or decode_u32(response) != STATUS_DECODE_ERROR:
            raise Failure('oversized mapped download was not rejected')


def test_abrupt_disconnect_is_survivable(server, build_dir):
    """Half a message, then vanish: the common case when a machine sleeps."""
    sock = connect(server.port)
    sock.sendall(HEADER.pack(OPCODE_HANDSHAKE, 32, 1)[:4])
    sock.close()

    time.sleep(0.5)
    if not server.alive():
        raise Failure('server died when a peer vanished mid-header')

    with connect(server.port) as good:
        if handshake(good, read_digest(build_dir), build_dir) != STATUS_OK:
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

    # Use the repository's minimal compute client: unlike vulkaninfo it requests
    # no optional instance extensions, so this test reaches the connection path
    # even when the ICD truthfully stops advertising properties2.
    client = os.path.join(build_dir, 'vulkan_remoting_compute')
    if not os.path.exists(client):
        return 'skipped: compute client not built'
    result = subprocess.run([client, '--no-validate'], env=env, capture_output=True,
                            text=True, timeout=60)

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
        if handshake(sock, digest, build_dir) != STATUS_OK:
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
        if handshake(sock, read_digest(build_dir), build_dir) != STATUS_OK:
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
    test_replies_echo_request_ids,
    test_ping_echoes_request_id,
    test_handshake_rejects_unknown_required_capability,
    test_handshake_rejects_foreign_digest,
    test_handshake_rejects_schema_mismatch,
    test_unknown_opcode_is_reported,
    test_reserved_opcode_zero_is_rejected,
    test_truncated_payload_does_not_hang,
    test_oversized_payload_is_refused,
    test_short_payload_for_known_command,
    test_pipeline_counts_are_bounded,
    test_graphics_pipeline_nested_counts_are_bounded,
    test_indexed_binding_payloads_are_rejected,
    test_push_constant_and_descriptor_payloads_are_rejected,
    test_shader_module_malformed_payloads_are_rejected,
    test_submit_and_fence_payloads_are_rejected,
    test_vulkan10_family_payloads_are_bounded,
    test_transfer_command_payloads_are_bounded,
    test_memory_properties_hide_host_coherent,
    test_device_extension_advertisement_is_real_device_intersection,
    test_new_extension_opcode_decode_errors,
    test_vulkan12_payloads_are_bounded,
    test_mapped_transfer_chunks_are_bounded,
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
    suffix = '.exe' if os.name == 'nt' else ''
    binary = os.path.join(build_dir, 'vulkan_remoting_server' + suffix)
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
