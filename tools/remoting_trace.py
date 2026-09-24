#!/usr/bin/env python3
"""Record, inspect, and replay one Vulkan remoting TCP connection.

The proxy understands the remoting message boundary (little-endian opcode and
payload length) but deliberately does not decode general Vulkan payloads. Trace
files are self-validating and are completely validated before replay opens a
socket. Replay defaults to deterministic output verification: response status,
handshake command identity, and exact downloaded mapped bytes. Use
``--verify exact`` when every response byte is expected to be reproducible.
"""

import argparse
import hashlib
import json
import os
import re
import socket
import struct
import sys
import tempfile
import threading
import zlib
from dataclasses import dataclass
from typing import BinaryIO, Iterable

WIRE_HEADER = struct.Struct("<II")
FILE_HEADER = struct.Struct("<8sII")
RECORD_HEADER = struct.Struct("<4sQB3xIII")
TRAILER = struct.Struct("<4sQ32s")

MAGIC = b"VRTRACE\0"
VERSION = 1
RECORD_TAG = b"RECD"
TRAILER_TAG = b"DONE"
CLIENT_TO_SERVER = 0
SERVER_TO_CLIENT = 1
DIRECTION_NAMES = ("client->server", "server->client")

DEFAULT_PORT = 24680
DEFAULT_MAX_METADATA = 64 * 1024
DEFAULT_MAX_PAYLOAD = 64 * 1024 * 1024
DEFAULT_MAX_RECORDS = 1_000_000
DEFAULT_MAX_TRACE_BYTES = 1024 * 1024 * 1024
DEFAULT_TIMEOUT = 30.0
PROTOCOL_METADATA = {
    "wire_schema": "kWireSchemaRevision",
    "registry_sha256": "kRegistrySha256",
    "wire_abi": "kWireAbi",
    "command_digest": "kCommandSetDigest",
}
TOOL_METADATA_KEYS = frozenset(PROTOCOL_METADATA) | {
    "format", "upstream_host", "upstream_port", "custom",
}


class TraceError(Exception):
    """A malformed trace or failed record/replay operation."""


@dataclass(frozen=True)
class Record:
    sequence: int
    direction: int
    opcode: int
    payload: bytes


def validate_metadata(metadata: dict) -> None:
    if not isinstance(metadata, dict):
        raise TraceError("metadata must be a JSON object")
    missing = [key for key in PROTOCOL_METADATA if key not in metadata]
    if missing:
        raise TraceError("metadata missing required keys: " + ", ".join(missing))
    for key in PROTOCOL_METADATA:
        if not isinstance(metadata[key], str) or not metadata[key]:
            raise TraceError(f"metadata {key} must be a non-empty string")
    if not re.fullmatch(r"[0-9a-f]{64}", metadata["registry_sha256"]):
        raise TraceError("metadata registry_sha256 must be 64 lowercase hex characters")
    if not re.fullmatch(r"[0-9a-f]{16}", metadata["command_digest"]):
        raise TraceError("metadata command_digest must be 16 lowercase hex characters")


def canonical_metadata(metadata: dict) -> bytes:
    validate_metadata(metadata)
    try:
        encoded = json.dumps(
            metadata, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
            allow_nan=False,
        ).encode("utf-8")
    except (TypeError, ValueError, UnicodeError) as error:
        raise TraceError(f"metadata is not canonical JSON: {error}") from error
    return encoded


def read_protocol_file(path: str) -> dict:
    """Extract protocol identity from generated remoting_commands.inl."""
    try:
        with open(path, encoding="utf-8") as handle:
            content = handle.read()
    except (OSError, UnicodeError) as error:
        raise TraceError(f"cannot read protocol file: {error}") from error
    metadata = {}
    for key, constant in PROTOCOL_METADATA.items():
        matches = re.findall(
            rf'^inline constexpr const char\* {re.escape(constant)} = "([^"]+)";$',
            content, re.MULTILINE)
        if len(matches) != 1:
            raise TraceError(
                f"protocol file must define {constant} exactly once (found {len(matches)})")
        metadata[key] = matches[0]
    validate_metadata(metadata)
    return metadata


def require_protocol_match(metadata: dict, protocol: dict) -> None:
    validate_metadata(metadata)
    for key in PROTOCOL_METADATA:
        if metadata[key] != protocol[key]:
            raise TraceError(
                f"protocol mismatch for {key}: trace has {metadata[key]!r}, "
                f"local file has {protocol[key]!r}")


def _read_exact(handle: BinaryIO, size: int, what: str) -> bytes:
    data = handle.read(size)
    if len(data) != size:
        raise TraceError(f"truncated {what}: expected {size} bytes, got {len(data)}")
    return data


def _checked_limits(max_metadata: int, max_payload: int, max_records: int,
                    max_trace_bytes: int) -> None:
    if min(max_metadata, max_payload, max_records, max_trace_bytes) < 0:
        raise TraceError("limits must be non-negative")
    if max_trace_bytes < FILE_HEADER.size + TRAILER.size:
        raise TraceError("maximum trace size is too small for an empty trace")


def write_trace(path: str, metadata: dict, records: Iterable[Record], *,
                max_metadata: int = DEFAULT_MAX_METADATA,
                max_payload: int = DEFAULT_MAX_PAYLOAD,
                max_records: int = DEFAULT_MAX_RECORDS,
                max_trace_bytes: int = DEFAULT_MAX_TRACE_BYTES,
                overwrite: bool = False) -> None:
    """Atomically write a canonical trace, refusing partial output."""
    _checked_limits(max_metadata, max_payload, max_records, max_trace_bytes)
    metadata_bytes = canonical_metadata(metadata)
    if len(metadata_bytes) > max_metadata:
        raise TraceError(f"metadata exceeds {max_metadata} bytes")
    if os.path.exists(path) and not overwrite:
        raise TraceError(f"trace already exists: {path} (use --force to replace it)")

    destination = os.path.abspath(path)
    directory = os.path.dirname(destination) or "."
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".remoting-trace-", dir=directory)
    count = 0
    total = 0
    digest = hashlib.sha256()

    def emit(handle: BinaryIO, data: bytes) -> None:
        nonlocal total
        total += len(data)
        if total + TRAILER.size > max_trace_bytes:
            raise TraceError(f"trace exceeds {max_trace_bytes} bytes")
        handle.write(data)
        digest.update(data)

    try:
        with os.fdopen(fd, "wb") as handle:
            emit(handle, FILE_HEADER.pack(MAGIC, VERSION, len(metadata_bytes)))
            emit(handle, metadata_bytes)
            for record in records:
                if count >= max_records:
                    raise TraceError(f"trace exceeds {max_records} records")
                if record.sequence != count:
                    raise TraceError(
                        f"record sequence {record.sequence} is not expected {count}")
                if record.direction not in (CLIENT_TO_SERVER, SERVER_TO_CLIENT):
                    raise TraceError(f"record {count} has invalid direction {record.direction}")
                if not 0 <= record.opcode <= 0xFFFFFFFF:
                    raise TraceError(f"record {count} has invalid opcode {record.opcode}")
                payload = bytes(record.payload)
                if len(payload) > max_payload:
                    raise TraceError(
                        f"record {count} payload exceeds {max_payload} bytes")
                header = RECORD_HEADER.pack(
                    RECORD_TAG, count, record.direction, record.opcode, len(payload),
                    zlib.crc32(payload) & 0xFFFFFFFF,
                )
                emit(handle, header)
                emit(handle, payload)
                count += 1
            handle.write(TRAILER.pack(TRAILER_TAG, count, digest.digest()))
            total += TRAILER.size
            handle.flush()
            os.fsync(handle.fileno())
        if total > max_trace_bytes:
            raise TraceError(f"trace exceeds {max_trace_bytes} bytes")
        os.replace(temporary, destination)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def read_trace(path: str, *, max_metadata: int = DEFAULT_MAX_METADATA,
               max_payload: int = DEFAULT_MAX_PAYLOAD,
               max_records: int = DEFAULT_MAX_RECORDS,
               max_trace_bytes: int = DEFAULT_MAX_TRACE_BYTES) -> tuple[dict, list[Record]]:
    """Load and strictly validate a trace, including its final digest."""
    _checked_limits(max_metadata, max_payload, max_records, max_trace_bytes)
    try:
        size = os.path.getsize(path)
    except OSError as error:
        raise TraceError(f"cannot stat trace: {error}") from error
    if size > max_trace_bytes:
        raise TraceError(f"trace is {size} bytes; limit is {max_trace_bytes}")
    if size < FILE_HEADER.size + TRAILER.size:
        raise TraceError("trace is too short")

    digest = hashlib.sha256()
    records = []
    try:
        with open(path, "rb") as handle:
            header = _read_exact(handle, FILE_HEADER.size, "file header")
            digest.update(header)
            magic, version, metadata_size = FILE_HEADER.unpack(header)
            if magic != MAGIC:
                raise TraceError("bad trace magic")
            if version != VERSION:
                raise TraceError(f"unsupported trace version {version}")
            if metadata_size > max_metadata:
                raise TraceError(f"metadata exceeds {max_metadata} bytes")
            metadata_bytes = _read_exact(handle, metadata_size, "metadata")
            digest.update(metadata_bytes)
            try:
                metadata = json.loads(metadata_bytes.decode("utf-8"))
            except (UnicodeError, json.JSONDecodeError) as error:
                raise TraceError(f"invalid metadata JSON: {error}") from error
            if canonical_metadata(metadata) != metadata_bytes:
                raise TraceError("metadata JSON is not in canonical form")

            expected_sequence = 0
            while True:
                tag = _read_exact(handle, 4, "record or trailer tag")
                if tag == TRAILER_TAG:
                    remainder = _read_exact(handle, TRAILER.size - 4, "trailer")
                    trailer_count, expected_digest = struct.unpack("<Q32s", remainder)
                    if trailer_count != expected_sequence:
                        raise TraceError(
                            f"trailer count {trailer_count} does not match "
                            f"{expected_sequence} records")
                    if expected_digest != digest.digest():
                        raise TraceError("trace SHA-256 mismatch")
                    if handle.read(1):
                        raise TraceError("trailing data after trace trailer")
                    return metadata, records
                if tag != RECORD_TAG:
                    raise TraceError(f"unknown chunk tag {tag!r}")
                if expected_sequence >= max_records:
                    raise TraceError(f"trace exceeds {max_records} records")
                remainder = _read_exact(
                    handle, RECORD_HEADER.size - 4, f"record {expected_sequence} header")
                raw_header = tag + remainder
                digest.update(raw_header)
                if raw_header[13:16] != b"\0\0\0":
                    raise TraceError(
                        f"record {expected_sequence} has non-zero reserved bytes")
                _, sequence, direction, opcode, payload_size, expected_crc = (
                    RECORD_HEADER.unpack(raw_header))
                if sequence != expected_sequence:
                    raise TraceError(
                        f"record sequence {sequence} is not expected {expected_sequence}")
                if direction not in (CLIENT_TO_SERVER, SERVER_TO_CLIENT):
                    raise TraceError(f"record {sequence} has invalid direction {direction}")
                if payload_size > max_payload:
                    raise TraceError(
                        f"record {sequence} payload exceeds {max_payload} bytes")
                payload = _read_exact(handle, payload_size, f"record {sequence} payload")
                digest.update(payload)
                actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
                if actual_crc != expected_crc:
                    raise TraceError(
                        f"record {sequence} CRC mismatch: expected {expected_crc:08x}, "
                        f"got {actual_crc:08x}")
                records.append(Record(sequence, direction, opcode, payload))
                expected_sequence += 1
    except OSError as error:
        raise TraceError(f"cannot read trace: {error}") from error


def recv_exact(sock: socket.socket, size: int, what: str, *, allow_eof: bool = False) -> bytes | None:
    chunks = []
    received = 0
    while received < size:
        try:
            chunk = sock.recv(size - received)
        except socket.timeout as error:
            raise TraceError(f"timed out reading {what}") from error
        except OSError as error:
            raise TraceError(f"socket error reading {what}: {error}") from error
        if not chunk:
            if allow_eof and received == 0:
                return None
            raise TraceError(f"connection closed during {what}")
        chunks.append(chunk)
        received += len(chunk)
    return b"".join(chunks)


def recv_wire_message(sock: socket.socket, max_payload: int) -> tuple[int, bytes] | None:
    header = recv_exact(sock, WIRE_HEADER.size, "wire header", allow_eof=True)
    if header is None:
        return None
    opcode, payload_size = WIRE_HEADER.unpack(header)
    if payload_size > max_payload:
        raise TraceError(f"wire payload {payload_size} exceeds {max_payload} bytes")
    payload = recv_exact(sock, payload_size, "wire payload")
    assert payload is not None
    return opcode, payload


def send_wire_message(sock: socket.socket, opcode: int, payload: bytes) -> None:
    try:
        sock.sendall(WIRE_HEADER.pack(opcode, len(payload)) + payload)
    except (socket.timeout, OSError) as error:
        raise TraceError(f"socket error sending opcode {opcode}: {error}") from error


def record_connection(client: socket.socket, upstream: socket.socket, *,
                      timeout: float, max_payload: int, max_records: int,
                      max_trace_bytes: int = DEFAULT_MAX_TRACE_BYTES,
                      initial_trace_bytes: int = FILE_HEADER.size + TRAILER.size) -> list[Record]:
    """Proxy one connection while bounding the eventual encoded trace size."""
    if initial_trace_bytes < FILE_HEADER.size + TRAILER.size:
        raise TraceError("initial trace size is smaller than fixed trace overhead")
    if initial_trace_bytes > max_trace_bytes:
        raise TraceError(f"trace exceeds {max_trace_bytes} bytes before recording")
    client.settimeout(timeout)
    upstream.settimeout(timeout)
    records = []
    encoded_size = initial_trace_bytes
    lock = threading.Lock()
    stop = threading.Event()
    errors = []

    def relay(source: socket.socket, destination: socket.socket, direction: int) -> None:
        nonlocal encoded_size
        try:
            while not stop.is_set():
                message = recv_wire_message(source, max_payload)
                if message is None:
                    try:
                        destination.shutdown(socket.SHUT_WR)
                    except OSError:
                        pass
                    return
                opcode, payload = message
                with lock:
                    if len(records) >= max_records:
                        raise TraceError(f"connection exceeds {max_records} records")
                    record_size = RECORD_HEADER.size + len(payload)
                    if encoded_size + record_size > max_trace_bytes:
                        raise TraceError(
                            f"connection trace exceeds {max_trace_bytes} bytes")
                    record = Record(len(records), direction, opcode, payload)
                    records.append(record)
                    encoded_size += record_size
                # Never hold the ordering lock across network I/O: a full socket
                # buffer must not prevent the reverse relay from draining data.
                send_wire_message(destination, opcode, payload)
        except BaseException as error:  # Relay failures must wake and join the peer.
            with lock:
                errors.append(error)
            stop.set()
            for endpoint in (client, upstream):
                try:
                    endpoint.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    threads = [
        threading.Thread(target=relay, args=(client, upstream, CLIENT_TO_SERVER), daemon=True),
        threading.Thread(target=relay, args=(upstream, client, SERVER_TO_CLIENT), daemon=True),
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    if errors:
        error = errors[0]
        if isinstance(error, TraceError):
            raise error
        raise TraceError(f"proxy failed: {error}") from error
    return records


def make_record_metadata(protocol: dict, upstream_host: str, upstream_port: int,
                         items: Iterable[str]) -> dict:
    metadata = {
        "format": "vulkan-remoting-wire",
        "upstream_host": upstream_host,
        "upstream_port": upstream_port,
        **protocol,
    }
    custom = {}
    for item in items:
        if "=" not in item:
            raise TraceError(f"metadata must be KEY=VALUE: {item!r}")
        key, value = item.split("=", 1)
        if not key:
            raise TraceError("metadata key must not be empty")
        if key in TOOL_METADATA_KEYS:
            raise TraceError(f"metadata key {key!r} is reserved by the trace tool")
        if key in custom:
            raise TraceError(f"metadata key {key!r} was supplied more than once")
        custom[key] = value
    if custom:
        metadata["custom"] = custom
    validate_metadata(metadata)
    return metadata


def run_record(args: argparse.Namespace) -> None:
    if os.path.exists(args.trace) and not args.force:
        raise TraceError(f"trace already exists: {args.trace} (use --force to replace it)")
    metadata = make_record_metadata(
        read_protocol_file(args.protocol_file), args.upstream_host,
        args.upstream_port, args.metadata)
    metadata_bytes = canonical_metadata(metadata)
    if len(metadata_bytes) > args.max_metadata:
        raise TraceError(f"metadata exceeds {args.max_metadata} bytes")
    initial_trace_bytes = FILE_HEADER.size + len(metadata_bytes) + TRAILER.size
    if initial_trace_bytes > args.max_trace_bytes:
        raise TraceError(f"trace exceeds {args.max_trace_bytes} bytes before recording")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.listen_host, args.listen_port))
        listener.listen(1)
        listener.settimeout(args.accept_timeout)
        bound_host, bound_port = listener.getsockname()[:2]
        print(f"record: listening on {bound_host}:{bound_port}", flush=True)
        try:
            client, peer = listener.accept()
        except socket.timeout as error:
            raise TraceError("timed out waiting for the single client") from error
        listener.close()
        print(f"record: accepted {peer[0]}:{peer[1]}; listener closed", flush=True)
        try:
            upstream = socket.create_connection(
                (args.upstream_host, args.upstream_port), timeout=args.timeout)
        except OSError as error:
            client.close()
            raise TraceError(f"cannot connect upstream: {error}") from error
        with client, upstream:
            records = record_connection(
                client, upstream, timeout=args.timeout,
                max_payload=args.max_payload, max_records=args.max_records,
                max_trace_bytes=args.max_trace_bytes,
                initial_trace_bytes=initial_trace_bytes)

    write_trace(
        args.trace, metadata, records, max_metadata=args.max_metadata,
        max_payload=args.max_payload, max_records=args.max_records,
        max_trace_bytes=args.max_trace_bytes, overwrite=args.force)
    print(f"record: wrote {len(records)} records to {args.trace}")


def _take_u32(payload: bytes, offset: int) -> tuple[int, int]:
    if offset + 4 > len(payload):
        raise ValueError("short u32")
    return struct.unpack_from("<I", payload, offset)[0], offset + 4


def _take_u64(payload: bytes, offset: int) -> tuple[int, int]:
    if offset + 8 > len(payload):
        raise ValueError("short u64")
    return struct.unpack_from("<Q", payload, offset)[0], offset + 8


def mapped_byte_totals(records: Iterable[Record]) -> dict:
    """Best-effort totals for structurally valid mapped-memory messages."""
    totals = {"flush_uploaded": 0, "download_requested": 0, "download_returned": 0}
    pending_download_counts = []
    for record in records:
        try:
            if record.direction == CLIENT_TO_SERVER and record.opcode == 2:
                _, offset = _take_u64(record.payload, 0)
                count, offset = _take_u32(record.payload, offset)
                if count > (len(record.payload) - offset) // 28:
                    raise ValueError("impossible flush range count")
                transferred = 0
                for _ in range(count):
                    _, offset = _take_u64(record.payload, offset)  # memory
                    _, offset = _take_u64(record.payload, offset)  # offset
                    _, offset = _take_u64(record.payload, offset)  # size
                    byte_count, offset = _take_u32(record.payload, offset)
                    if offset + byte_count > len(record.payload):
                        raise ValueError("short flush bytes")
                    offset += byte_count
                    transferred += byte_count
                if offset == len(record.payload):
                    totals["flush_uploaded"] += transferred
            elif record.direction == CLIENT_TO_SERVER and record.opcode == 3:
                _, offset = _take_u64(record.payload, 0)
                count, offset = _take_u32(record.payload, offset)
                if count > (len(record.payload) - offset) // 24:
                    raise ValueError("impossible download range count")
                requested = 0
                for _ in range(count):
                    _, offset = _take_u64(record.payload, offset)  # memory
                    _, offset = _take_u64(record.payload, offset)  # offset
                    size, offset = _take_u64(record.payload, offset)
                    requested += size
                if offset == len(record.payload):
                    totals["download_requested"] += requested
                    pending_download_counts.append(count)
            elif record.direction == SERVER_TO_CLIENT and record.opcode == 3:
                count = pending_download_counts.pop(0) if pending_download_counts else 0
                status, offset = _take_u32(record.payload, 0)
                transferred = 0
                if status == 0:
                    for _ in range(count):
                        byte_count, offset = _take_u32(record.payload, offset)
                        if offset + byte_count > len(record.payload):
                            raise ValueError("short download bytes")
                        offset += byte_count
                        transferred += byte_count
                    if offset == len(record.payload):
                        totals["download_returned"] += transferred
        except (ValueError, struct.error):
            continue
    return totals


def trace_summary(records: list[Record]) -> dict:
    directions = {
        name: {"records": 0, "payload_bytes": 0} for name in DIRECTION_NAMES
    }
    opcodes = {}
    direction_opcodes = {}
    for record in records:
        direction = DIRECTION_NAMES[record.direction]
        directions[direction]["records"] += 1
        directions[direction]["payload_bytes"] += len(record.payload)
        key = str(record.opcode)
        entry = opcodes.setdefault(key, {"records": 0, "payload_bytes": 0})
        entry["records"] += 1
        entry["payload_bytes"] += len(record.payload)
        combined = f"{direction}/opcode-{record.opcode}"
        entry = direction_opcodes.setdefault(
            combined, {"records": 0, "payload_bytes": 0})
        entry["records"] += 1
        entry["payload_bytes"] += len(record.payload)
    return {
        "directions": directions,
        "opcodes": opcodes,
        "direction_opcodes": direction_opcodes,
        "mapped_bytes": mapped_byte_totals(records),
    }


def run_inspect(args: argparse.Namespace) -> None:
    metadata, records = read_trace(
        args.trace, max_metadata=args.max_metadata, max_payload=args.max_payload,
        max_records=args.max_records, max_trace_bytes=args.max_trace_bytes)
    summary = trace_summary(records)
    if args.json:
        output = {
            "metadata": metadata,
            "record_count": len(records),
            "summary": summary,
            "records": [
                {
                    "sequence": record.sequence,
                    "direction": DIRECTION_NAMES[record.direction],
                    "opcode": record.opcode,
                    "length": len(record.payload),
                    "crc32": f"{zlib.crc32(record.payload) & 0xFFFFFFFF:08x}",
                }
                for record in records
            ],
        }
        print(json.dumps(output, sort_keys=True, indent=2, ensure_ascii=False))
        return
    print("metadata: " + canonical_metadata(metadata).decode("utf-8"))
    for record in records:
        print(
            f"{record.sequence:08d} {DIRECTION_NAMES[record.direction]:14s} "
            f"opcode={record.opcode} length={len(record.payload)} "
            f"crc32={zlib.crc32(record.payload) & 0xFFFFFFFF:08x}")
    print(f"records: {len(records)}")
    for direction in DIRECTION_NAMES:
        values = summary["directions"][direction]
        print(
            f"direction {direction}: records={values['records']} "
            f"payload_bytes={values['payload_bytes']}")
    for opcode, values in sorted(
            summary["opcodes"].items(), key=lambda item: int(item[0])):
        print(
            f"opcode {opcode}: records={values['records']} "
            f"payload_bytes={values['payload_bytes']}")
    for key, values in sorted(summary["direction_opcodes"].items()):
        print(
            f"direction_opcode {key}: records={values['records']} "
            f"payload_bytes={values['payload_bytes']}")
    mapped = summary["mapped_bytes"]
    print(
        "mapped_bytes: "
        f"flush_uploaded={mapped['flush_uploaded']} "
        f"download_requested={mapped['download_requested']} "
        f"download_returned={mapped['download_returned']}")


def _status(payload: bytes, sequence: int) -> int:
    if len(payload) < 4:
        raise TraceError(
            f"response {sequence}: payload is too short for protocol status")
    return struct.unpack_from("<I", payload)[0]


def _handshake_digest(payload: bytes, sequence: int) -> str:
    _status(payload, sequence)
    if len(payload) < 8:
        raise TraceError(f"response {sequence}: handshake has no identity string")
    size = struct.unpack_from("<I", payload, 4)[0]
    end = 8 + size
    if end > len(payload):
        raise TraceError(f"response {sequence}: truncated handshake identity string")
    try:
        return payload[8:end].decode("utf-8")
    except UnicodeError as error:
        raise TraceError(
            f"response {sequence}: handshake identity is not UTF-8") from error


def verify_response(record: Record, actual_opcode: int, actual_payload: bytes,
                    mode: str, command_digest: str) -> None:
    if actual_opcode != record.opcode:
        raise TraceError(
            f"response {record.sequence}: opcode mismatch: expected "
            f"{record.opcode}, got {actual_opcode}")
    if mode == "exact" or record.opcode == 3:
        if actual_payload != record.payload:
            mismatch = next(
                (index for index, pair in enumerate(zip(record.payload, actual_payload))
                 if pair[0] != pair[1]),
                min(len(record.payload), len(actual_payload)),
            )
            raise TraceError(
                f"response {record.sequence}: payload mismatch at byte {mismatch}; "
                f"expected {len(record.payload)} bytes, got {len(actual_payload)}")
        return

    expected_status = _status(record.payload, record.sequence)
    actual_status = _status(actual_payload, record.sequence)
    if actual_status != expected_status:
        raise TraceError(
            f"response {record.sequence}: status mismatch: expected "
            f"{expected_status}, got {actual_status}")
    if record.opcode == 1:
        expected_digest = _handshake_digest(record.payload, record.sequence)
        actual_digest = _handshake_digest(actual_payload, record.sequence)
        if expected_digest != command_digest:
            raise TraceError(
                f"response {record.sequence}: recorded handshake identity "
                f"{expected_digest!r} does not match trace metadata {command_digest!r}")
        if actual_digest != command_digest:
            raise TraceError(
                f"response {record.sequence}: handshake identity mismatch: expected "
                f"{command_digest!r}, got {actual_digest!r}")


def run_replay(args: argparse.Namespace) -> None:
    # Trace integrity and local protocol identity must both validate before the
    # first network operation, so an incompatible recording cannot reach a GPU.
    metadata, records = read_trace(
        args.trace, max_metadata=args.max_metadata, max_payload=args.max_payload,
        max_records=args.max_records, max_trace_bytes=args.max_trace_bytes)
    require_protocol_match(metadata, read_protocol_file(args.protocol_file))
    try:
        connection = socket.create_connection((args.host, args.port), timeout=args.timeout)
    except OSError as error:
        raise TraceError(f"cannot connect replay target: {error}") from error
    with connection:
        connection.settimeout(args.timeout)
        compared = 0
        for record in records:
            if record.direction == CLIENT_TO_SERVER:
                send_wire_message(connection, record.opcode, record.payload)
                continue
            actual = recv_wire_message(connection, args.max_payload)
            if actual is None:
                raise TraceError(
                    f"response {record.sequence}: target closed before opcode {record.opcode}")
            actual_opcode, actual_payload = actual
            verify_response(
                record, actual_opcode, actual_payload, args.verify,
                metadata["command_digest"])
            compared += 1
        try:
            connection.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        unexpected = recv_wire_message(connection, args.max_payload)
        if unexpected is not None:
            raise TraceError(
                f"unexpected response after trace: opcode {unexpected[0]}, "
                f"{len(unexpected[1])} bytes")
    print(
        f"replay: verified {compared} responses across {len(records)} records "
        f"using {args.verify} mode")


def positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def nonnegative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return parsed


def port(value: str) -> int:
    parsed = int(value)
    if not 0 <= parsed <= 65535:
        raise argparse.ArgumentTypeError("must be between 0 and 65535")
    return parsed


def add_limits(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--max-metadata", type=nonnegative_int, default=DEFAULT_MAX_METADATA)
    parser.add_argument("--max-payload", type=nonnegative_int, default=DEFAULT_MAX_PAYLOAD)
    parser.add_argument("--max-records", type=nonnegative_int, default=DEFAULT_MAX_RECORDS)
    parser.add_argument("--max-trace-bytes", type=nonnegative_int,
                        default=DEFAULT_MAX_TRACE_BYTES)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    record = subparsers.add_parser("record", help="proxy and record one connection")
    record.add_argument("trace")
    record.add_argument("--listen-host", default="127.0.0.1")
    record.add_argument("--listen-port", type=port, default=24681)
    record.add_argument("--upstream-host", default="127.0.0.1")
    record.add_argument("--upstream-port", type=port, default=DEFAULT_PORT)
    record.add_argument("--timeout", type=positive_float, default=DEFAULT_TIMEOUT)
    record.add_argument("--protocol-file", required=True,
                        help="generated remoting_commands.inl used by both peers")
    record.add_argument("--accept-timeout", type=positive_float, default=DEFAULT_TIMEOUT)
    record.add_argument(
        "--metadata", action="append", default=[], metavar="KEY=VALUE",
        help="add an entry under metadata.custom; tool-owned keys are reserved")
    record.add_argument("--force", action="store_true")
    add_limits(record)
    record.set_defaults(action=run_record)

    inspect = subparsers.add_parser("inspect", help="validate and summarize a trace")
    inspect.add_argument("trace")
    inspect.add_argument("--json", action="store_true")
    add_limits(inspect)
    inspect.set_defaults(action=run_inspect)

    replay = subparsers.add_parser("replay", help="replay requests and compare responses")
    replay.add_argument("trace")
    replay.add_argument("--host", default="127.0.0.1")
    replay.add_argument("--port", type=port, default=DEFAULT_PORT)
    replay.add_argument("--timeout", type=positive_float, default=DEFAULT_TIMEOUT)
    replay.add_argument("--protocol-file", required=True,
                        help="generated remoting_commands.inl required to match the trace")
    replay.add_argument(
        "--verify", choices=("outputs", "exact"), default="outputs",
        help="outputs compares status/handshake/downloads; exact compares all bytes "
             "(default: %(default)s)")
    add_limits(replay)
    replay.set_defaults(action=run_replay)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    try:
        args.action(args)
        return 0
    except (TraceError, OSError) as error:
        print(f"remoting_trace: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
