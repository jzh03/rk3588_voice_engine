"""Offline TLS/WebSocket fixture for the Bailian ASR adapter."""
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def handshake(wire, reader):
    assert reader.readline().startswith(b"GET /api-ws/v1/inference HTTP/")
    headers = {}
    while (line := reader.readline()) != b"\r\n":
        name, value = line.split(b":", 1)
        headers[name.lower()] = value.strip()
    assert headers[b"authorization"] == b"Bearer offline-fixture-key"
    accept = base64.b64encode(hashlib.sha1(headers[b"sec-websocket-key"] + GUID).digest())
    wire.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                 b"Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")


def recv_frame(reader):
    head = reader.read(2)
    if not head:
        return 8, b""
    flags, length = head
    assert flags & 0x80 and length & 0x80
    length &= 127
    if length == 126:
        length = int.from_bytes(reader.read(2), "big")
    elif length == 127:
        length = int.from_bytes(reader.read(8), "big")
    assert length <= 8192
    mask = reader.read(4)
    data = reader.read(length)
    return flags & 15, bytes(x ^ mask[n % 4] for n, x in enumerate(data))


def send_frame(wire, opcode, data, final=True):
    header = bytes([(0x80 if final else 0) | opcode])
    size = bytes([len(data)]) if len(data) < 126 else b"\x7e" + len(data).to_bytes(2, "big")
    wire.sendall(header + size + data)


def send_event(wire, task, name, sentence=None, fragmented=False):
    message = {"header": {"event": name, "task_id": task}, "payload": {}}
    if sentence is not None:
        message["payload"] = {"output": {"sentence": sentence}}
    data = json.dumps(message).encode()
    if fragmented:
        send_frame(wire, 1, data[:23], final=False)
        send_frame(wire, 0, data[23:])
    else:
        send_frame(wire, 1, data)


def exercise(wire, scenario):
    with wire.makefile("rb") as reader:
        handshake(wire, reader)
        opcode, payload = recv_frame(reader)
        assert opcode == 1
        start = json.loads(payload)
        assert start["header"]["action"] == "run-task"
        assert start["payload"]["parameters"]["format"] == "pcm"
        assert start["payload"]["parameters"]["sample_rate"] == 16000
        task = start["header"]["task_id"]
        if scenario not in ("timeout", "overflow"):
            send_event(wire, task, "task-started")
        if scenario != "speech":
            if scenario == "cancel":
                opcode, payload = recv_frame(reader)
                assert opcode == 2 and payload
            while (frame := recv_frame(reader))[0] != 8:
                assert scenario == "cancel" and frame[0] == 2
            return

        pcm = bytearray()
        while (frame := recv_frame(reader))[0] == 2:
            pcm.extend(frame[1])
        assert frame[0] == 1
        finish = json.loads(frame[1])
        assert finish["header"]["action"] == "finish-task"
        assert finish["header"]["task_id"] == task
        expected = (struct.pack("<h", -16384) * (4800 - 512)
                    + struct.pack("<h", 8192) * 512 + struct.pack("<h", 16384) * 1024)
        assert pcm == expected, f"unexpected PCM length/order ({len(pcm)} vs {len(expected)})"
        send_event(wire, "old-task", "task-finished")
        send_event(wire, task, "result-generated", {"sentence_id": 1, "sentence_end": False, "text": "draft"})
        send_event(wire, task, "result-generated", {"sentence_id": 2, "sentence_end": True, "text": "second"})
        send_event(wire, task, "result-generated", {"sentence_id": 1, "sentence_end": True, "text": "first "},
                   fragmented=True)
        send_event(wire, task, "task-finished")


def run_case(executable, tls, cert, scenario):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(4)
        port = listener.getsockname()[1]
        args = [executable, f"wss://127.0.0.1:{port}/api-ws/v1/inference", str(cert), scenario]
        env = {**os.environ, "STREAM_ASR_TEST_KEY": "offline-fixture-key"}
        with subprocess.Popen(args, env=env, stderr=subprocess.PIPE, text=True) as proc:
            try:
                conn, _ = listener.accept()
                with conn:
                    conn.settimeout(4)
                    with tls.wrap_socket(conn, server_side=True) as wire:
                        exercise(wire, scenario)
            except (TimeoutError, ssl.SSLError, ConnectionError) as exc:
                # Queue overflow can terminate while the connection is still opening.
                if scenario != "overflow":
                    proc.kill()
                    _, err = proc.communicate()
                    raise AssertionError(f"{scenario}: {exc}; {err.strip()}") from exc
            except Exception:
                proc.kill()
                raise
            _, err = proc.communicate(timeout=5)
            assert proc.returncode == 0, f"{scenario}: {err.strip()}"


def main():
    with tempfile.TemporaryDirectory() as folder:
        cert, key = Path(folder) / "localhost.crt", Path(folder) / "localhost.key"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=127.0.0.1",
                        "-addext", "subjectAltName=IP:127.0.0.1,DNS:127.0.0.1"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        for scenario in ("speech", "silence", "cancel", "timeout", "overflow"):
            run_case(sys.argv[1], tls, cert, scenario)


if __name__ == "__main__":
    main()
