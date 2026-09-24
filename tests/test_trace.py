#!/usr/bin/env python3
"""Focused format and loopback tests for tools/remoting_trace.py."""

import contextlib
import importlib.util
import io
import json
import os
import socket
import struct
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "remoting_trace", ROOT / "tools" / "remoting_trace.py")
trace = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(trace)


class ThreadResult:
    def __init__(self, target):
        self.error = None

        def run():
            try:
                target()
            except BaseException as error:  # Report worker failures in the test thread.
                self.error = error

        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()

    def join(self, timeout=5):
        self.thread.join(timeout)
        if self.thread.is_alive():
            raise AssertionError("worker thread did not finish")
        if self.error:
            raise self.error


PROTOCOL = {
    "wire_schema": "wire-v6-mvp-abi-handshake",
    "registry_sha256": "a" * 64,
    "wire_abi": "x86_64-little-endian-v1",
    "command_digest": "0123456789abcdef",
}


def write_protocol_file(directory, values=PROTOCOL):
    path = os.path.join(directory, "remoting_commands.inl")
    constants = {
        "wire_schema": "kWireSchemaRevision",
        "registry_sha256": "kRegistrySha256",
        "wire_abi": "kWireAbi",
        "command_digest": "kCommandSetDigest",
    }
    Path(path).write_text("".join(
        f'inline constexpr const char* {constant} = "{values[key]}";\n'
        for key, constant in constants.items()), encoding="utf-8")
    return path


def listening_socket():
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(5)
    return listener


class FormatTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="remoting-trace-test-")
        self.path = os.path.join(self.directory.name, "sample.vrt")
        self.metadata = {
            **PROTOCOL, "z": "last", "format": "vulkan-remoting-wire", "unicode": "水"
        }
        self.protocol_file = write_protocol_file(self.directory.name)
        self.records = [
            trace.Record(0, trace.CLIENT_TO_SERVER, 1, b"request"),
            trace.Record(1, trace.SERVER_TO_CLIENT, 1, b"response"),
            trace.Record(2, trace.CLIENT_TO_SERVER, 73, b""),
        ]

    def tearDown(self):
        self.directory.cleanup()

    def test_round_trip_is_canonical_and_deterministic(self):
        second = os.path.join(self.directory.name, "second.vrt")
        trace.write_trace(self.path, self.metadata, self.records)
        trace.write_trace(second, dict(reversed(list(self.metadata.items()))), self.records)

        loaded_metadata, loaded_records = trace.read_trace(self.path)
        self.assertEqual(loaded_metadata, self.metadata)
        self.assertEqual(loaded_records, self.records)
        self.assertEqual(Path(self.path).read_bytes(), Path(second).read_bytes())
        raw = Path(self.path).read_bytes()
        self.assertEqual(raw[:8], trace.MAGIC)
        self.assertEqual(struct.unpack_from("<I", raw, 8)[0], trace.VERSION)

    def test_rejects_payload_corruption(self):
        trace.write_trace(self.path, self.metadata, self.records)
        raw = bytearray(Path(self.path).read_bytes())
        metadata_size = trace.FILE_HEADER.unpack_from(raw)[2]
        first_payload = trace.FILE_HEADER.size + metadata_size + trace.RECORD_HEADER.size
        raw[first_payload] ^= 0x40
        Path(self.path).write_bytes(raw)
        with self.assertRaisesRegex(trace.TraceError, "CRC mismatch"):
            trace.read_trace(self.path)

    def test_rejects_trailer_digest_corruption(self):
        trace.write_trace(self.path, self.metadata, self.records)
        raw = bytearray(Path(self.path).read_bytes())
        raw[-1] ^= 1
        Path(self.path).write_bytes(raw)
        with self.assertRaisesRegex(trace.TraceError, "SHA-256 mismatch"):
            trace.read_trace(self.path)

    def test_rejects_truncation_trailing_data_and_bad_sequence(self):
        trace.write_trace(self.path, self.metadata, self.records)
        good = Path(self.path).read_bytes()
        cases = [
            (good[:-3], "truncated trailer"),
            (good + b"x", "trailing data"),
        ]
        for index, (raw, message) in enumerate(cases):
            path = os.path.join(self.directory.name, f"bad-{index}.vrt")
            Path(path).write_bytes(raw)
            with self.subTest(message=message), self.assertRaisesRegex(trace.TraceError, message):
                trace.read_trace(path)

        raw = bytearray(good)
        metadata_size = trace.FILE_HEADER.unpack_from(raw)[2]
        record_offset = trace.FILE_HEADER.size + metadata_size
        struct.pack_into("<Q", raw, record_offset + 4, 9)
        Path(self.path).write_bytes(raw)
        with self.assertRaisesRegex(trace.TraceError, "sequence 9"):
            trace.read_trace(self.path)

    def test_limits_and_existing_file_are_enforced(self):
        with self.assertRaisesRegex(trace.TraceError, "payload exceeds"):
            trace.write_trace(self.path, PROTOCOL, self.records, max_payload=3)
        self.assertFalse(os.path.exists(self.path))

        trace.write_trace(self.path, PROTOCOL, [])
        with self.assertRaisesRegex(trace.TraceError, "already exists"):
            trace.write_trace(self.path, PROTOCOL, [], overwrite=False)
        with self.assertRaisesRegex(trace.TraceError, "too small"):
            trace.read_trace(self.path, max_trace_bytes=1)

    def test_inspect_json_reports_headers_without_payloads(self):
        trace.write_trace(self.path, self.metadata, self.records)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = trace.main(["inspect", self.path, "--json"])
        self.assertEqual(result, 0)
        decoded = json.loads(output.getvalue())
        self.assertEqual(decoded["record_count"], 3)
        self.assertEqual(decoded["records"][0]["direction"], "client->server")
        self.assertNotIn("payload", decoded["records"][0])
        self.assertEqual(
            decoded["summary"]["directions"]["client->server"],
            {"records": 2, "payload_bytes": 7})
        self.assertEqual(
            decoded["summary"]["opcodes"]["1"],
            {"records": 2, "payload_bytes": 15})
        self.assertEqual(decoded["summary"]["mapped_bytes"]["flush_uploaded"], 0)

    def test_mapped_byte_summary(self):
        flush = struct.pack("<QIQQQI", 9, 1, 10, 20, 3, 3) + b"abc"
        download = struct.pack("<QIQQQ", 9, 1, 10, 20, 5)
        reply = struct.pack("<II", 0, 5) + b"12345"
        records = [
            trace.Record(0, trace.CLIENT_TO_SERVER, 2, flush),
            trace.Record(1, trace.CLIENT_TO_SERVER, 3, download),
            trace.Record(2, trace.SERVER_TO_CLIENT, 3, reply),
        ]
        summary = trace.trace_summary(records)
        self.assertEqual(summary["mapped_bytes"], {
            "flush_uploaded": 3,
            "download_requested": 5,
            "download_returned": 5,
        })

    def test_replay_validates_before_connecting(self):
        Path(self.path).write_bytes(b"not a trace")
        error = io.StringIO()
        with mock.patch.object(trace.socket, "create_connection") as connect, \
                contextlib.redirect_stderr(error):
            result = trace.main([
                "replay", self.path, "--port", "1",
                "--protocol-file", self.protocol_file,
            ])
        self.assertEqual(result, 1)
        self.assertIn("trace is too short", error.getvalue())
        connect.assert_not_called()

    def test_protocol_mismatch_is_rejected_before_connecting(self):
        trace.write_trace(self.path, self.metadata, self.records)
        mismatch = dict(PROTOCOL)
        mismatch["command_digest"] = "fedcba9876543210"
        protocol_file = write_protocol_file(self.directory.name, mismatch)
        error = io.StringIO()
        with mock.patch.object(trace.socket, "create_connection") as connect, \
                contextlib.redirect_stderr(error):
            result = trace.main([
                "replay", self.path, "--port", "1",
                "--protocol-file", protocol_file,
            ])
        self.assertEqual(result, 1)
        self.assertIn("protocol mismatch for command_digest", error.getvalue())
        connect.assert_not_called()

    def test_protocol_file_parsing_and_required_metadata(self):
        self.assertEqual(trace.read_protocol_file(self.protocol_file), PROTOCOL)
        with self.assertRaisesRegex(trace.TraceError, "missing required keys"):
            trace.write_trace(self.path, {}, [])

    def test_custom_metadata_is_nested_and_owned_keys_are_rejected(self):
        metadata = trace.make_record_metadata(
            PROTOCOL, "gpu.example", 24680, ["case=compute", "note=nightly"])
        self.assertEqual(metadata["custom"], {"case": "compute", "note": "nightly"})
        self.assertEqual(metadata["upstream_host"], "gpu.example")
        for key in sorted(trace.TOOL_METADATA_KEYS):
            with self.subTest(key=key), self.assertRaisesRegex(
                    trace.TraceError, "reserved by the trace tool"):
                trace.make_record_metadata(PROTOCOL, "host", 1, [f"{key}=override"])
        with self.assertRaisesRegex(trace.TraceError, "more than once"):
            trace.make_record_metadata(PROTOCOL, "host", 1, ["case=a", "case=b"])


class LoopbackTests(unittest.TestCase):
    def test_record_connection_enforces_cumulative_trace_size(self):
        client_peer, client_proxy = socket.socketpair()
        upstream_proxy, upstream_peer = socket.socketpair()
        overhead = trace.FILE_HEADER.size + trace.TRAILER.size
        limit = overhead + trace.RECORD_HEADER.size + 3

        worker = ThreadResult(lambda: trace.record_connection(
            client_proxy, upstream_proxy, timeout=2, max_payload=1024,
            max_records=10, max_trace_bytes=limit, initial_trace_bytes=overhead))
        try:
            trace.send_wire_message(client_peer, 7, b"abc")
            self.assertEqual(trace.recv_wire_message(upstream_peer, 1024), (7, b"abc"))
            trace.send_wire_message(client_peer, 8, b"x")
            client_peer.shutdown(socket.SHUT_WR)
            with self.assertRaisesRegex(trace.TraceError, "trace exceeds"):
                worker.join()
        finally:
            for endpoint in (client_peer, client_proxy, upstream_proxy, upstream_peer):
                endpoint.close()

    def test_record_proxy_and_exact_replay(self):
        requests = [(11, b"alpha"), (22, b"beta\x00gamma")]
        responses = [(11, b"ALPHA"), (22, b"BETA\x00GAMMA")]
        upstream_listener = listening_socket()
        proxy_listener = listening_socket()
        upstream_port = upstream_listener.getsockname()[1]
        proxy_port = proxy_listener.getsockname()[1]
        recorded = []

        def upstream_server():
            with upstream_listener:
                connection, _ = upstream_listener.accept()
                with connection:
                    connection.settimeout(2)
                    for expected, response in zip(requests, responses):
                        self.assertEqual(trace.recv_wire_message(connection, 1024), expected)
                        trace.send_wire_message(connection, *response)
                    connection.shutdown(socket.SHUT_WR)

        def proxy_server():
            with proxy_listener:
                client, _ = proxy_listener.accept()
                with client, socket.create_connection(
                        ("127.0.0.1", upstream_port), timeout=2) as upstream:
                    recorded.extend(trace.record_connection(
                        client, upstream, timeout=2, max_payload=1024, max_records=20))

        upstream_worker = ThreadResult(upstream_server)
        proxy_worker = ThreadResult(proxy_server)
        with socket.create_connection(("127.0.0.1", proxy_port), timeout=2) as client:
            client.settimeout(2)
            for request, expected in zip(requests, responses):
                trace.send_wire_message(client, *request)
                self.assertEqual(trace.recv_wire_message(client, 1024), expected)
            client.shutdown(socket.SHUT_WR)
            self.assertIsNone(trace.recv_wire_message(client, 1024))
        proxy_worker.join()
        upstream_worker.join()

        self.assertEqual(
            [(record.direction, record.opcode, record.payload) for record in recorded],
            [
                (trace.CLIENT_TO_SERVER, 11, b"alpha"),
                (trace.SERVER_TO_CLIENT, 11, b"ALPHA"),
                (trace.CLIENT_TO_SERVER, 22, b"beta\x00gamma"),
                (trace.SERVER_TO_CLIENT, 22, b"BETA\x00GAMMA"),
            ],
        )

        with tempfile.TemporaryDirectory(prefix="remoting-replay-test-") as directory:
            path = os.path.join(directory, "flow.vrt")
            trace.write_trace(path, {**PROTOCOL, "format": "vulkan-remoting-wire"}, recorded)
            protocol_file = write_protocol_file(directory)
            replay_listener = listening_socket()
            replay_port = replay_listener.getsockname()[1]

            def replay_target():
                with replay_listener:
                    connection, _ = replay_listener.accept()
                    with connection:
                        connection.settimeout(2)
                        for expected, response in zip(requests, responses):
                            self.assertEqual(trace.recv_wire_message(connection, 1024), expected)
                            trace.send_wire_message(connection, *response)
                        connection.shutdown(socket.SHUT_WR)

            replay_worker = ThreadResult(replay_target)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = trace.main([
                    "replay", path, "--host", "127.0.0.1", "--port", str(replay_port),
                    "--timeout", "2", "--max-payload", "1024",
                    "--protocol-file", protocol_file,
                ])
            replay_worker.join()
            self.assertEqual(result, 0)
            self.assertIn("verified 2 responses", output.getvalue())
            self.assertIn("outputs mode", output.getvalue())

    def test_outputs_verification_is_deterministic(self):
        generic = trace.Record(1, trace.SERVER_TO_CLIENT, 77, struct.pack("<I", 0) + b"old")
        trace.verify_response(generic, 77, struct.pack("<I", 0) + b"new", "outputs",
                              PROTOCOL["command_digest"])
        with self.assertRaisesRegex(trace.TraceError, "status mismatch"):
            trace.verify_response(generic, 77, struct.pack("<I", 2) + b"new", "outputs",
                                  PROTOCOL["command_digest"])
        with self.assertRaisesRegex(trace.TraceError, "payload mismatch"):
            trace.verify_response(generic, 77, struct.pack("<I", 0) + b"new", "exact",
                                  PROTOCOL["command_digest"])

        def handshake_payload(digest, device_count):
            raw = digest.encode("utf-8")
            return struct.pack("<II", 0, len(raw)) + raw + struct.pack("<I", device_count)

        handshake = trace.Record(
            1, trace.SERVER_TO_CLIENT, 1,
            handshake_payload(PROTOCOL["command_digest"], 1))
        trace.verify_response(
            handshake, 1, handshake_payload(PROTOCOL["command_digest"], 99),
            "outputs", PROTOCOL["command_digest"])
        with self.assertRaisesRegex(trace.TraceError, "handshake identity mismatch"):
            trace.verify_response(
                handshake, 1, handshake_payload("fedcba9876543210", 1),
                "outputs", PROTOCOL["command_digest"])

        download = trace.Record(
            3, trace.SERVER_TO_CLIENT, 3, struct.pack("<II", 0, 3) + b"abc")
        with self.assertRaisesRegex(trace.TraceError, "payload mismatch"):
            trace.verify_response(
                download, 3, struct.pack("<II", 0, 3) + b"abd",
                "outputs", PROTOCOL["command_digest"])

    def test_replay_preserves_oneway_request_order(self):
        records = [
            trace.Record(0, trace.CLIENT_TO_SERVER, 40, b"oneway"),
            trace.Record(1, trace.CLIENT_TO_SERVER, 41, b"sync"),
            trace.Record(2, trace.SERVER_TO_CLIENT, 41, struct.pack("<I", 0) + b"reply"),
        ]
        with tempfile.TemporaryDirectory(prefix="remoting-oneway-test-") as directory:
            path = os.path.join(directory, "flow.vrt")
            protocol_file = write_protocol_file(directory)
            trace.write_trace(path, PROTOCOL, records)
            listener = listening_socket()
            target_port = listener.getsockname()[1]
            received = []

            def target():
                with listener:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(2)
                        received.append(trace.recv_wire_message(connection, 1024))
                        received.append(trace.recv_wire_message(connection, 1024))
                        trace.send_wire_message(
                            connection, 41, struct.pack("<I", 0) + b"different-body")
                        connection.shutdown(socket.SHUT_WR)

            worker = ThreadResult(target)
            result = trace.main([
                "replay", path, "--port", str(target_port), "--timeout", "2",
                "--protocol-file", protocol_file,
            ])
            worker.join()
            self.assertEqual(result, 0)
            self.assertEqual(received, [(40, b"oneway"), (41, b"sync")])

    def test_replay_detects_response_mismatch(self):
        records = [
            trace.Record(0, trace.CLIENT_TO_SERVER, 7, b"request"),
            trace.Record(1, trace.SERVER_TO_CLIENT, 7, b"expected"),
        ]
        with tempfile.TemporaryDirectory(prefix="remoting-mismatch-test-") as directory:
            path = os.path.join(directory, "flow.vrt")
            trace.write_trace(path, PROTOCOL, records)
            protocol_file = write_protocol_file(directory)
            listener = listening_socket()
            target_port = listener.getsockname()[1]

            def target():
                with listener:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(2)
                        trace.recv_wire_message(connection, 1024)
                        trace.send_wire_message(connection, 7, b"different")

            worker = ThreadResult(target)
            error = io.StringIO()
            with contextlib.redirect_stderr(error):
                result = trace.main([
                    "replay", path, "--port", str(target_port), "--timeout", "2",
                    "--protocol-file", protocol_file, "--verify", "exact",
                ])
            worker.join()
            self.assertEqual(result, 1)
            self.assertIn("payload mismatch", error.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
