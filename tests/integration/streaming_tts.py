"""Offline TLS test for streaming TTS and the real PCM worker."""
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile

from streaming_asr import handshake, recv_frame, send_event, send_frame


def exercise(wire, proc, scenario):
    with wire.makefile("rb") as reader:
        handshake(wire, reader)
        def command(action, task=None):
            opcode, payload = recv_frame(reader)
            assert opcode == 1
            request = json.loads(payload)
            assert request["header"]["action"] == action
            if task:
                assert request["header"]["task_id"] == task
            return request
        start = command("run-task")
        task = start["header"]["task_id"]
        payload = start["payload"]
        assert payload["function"] == "SpeechSynthesizer"
        assert payload["parameters"]["format"] == "pcm"
        assert payload["parameters"]["sample_rate"] == 16000
        send_event(wire, task, "task-started", fragmented=True)
        assert command("continue-task", task)["payload"]["input"]["text"]
        command("finish-task", task)
        pcm = bytes((0, 224)) * 1600
        # Deliberately split a PCM sample across binary messages. The fixture
        # withholds task-finished until the actual playback queue has a frame.
        send_frame(wire, 2, pcm if scenario == "cancel" else pcm[:1281])
        assert proc.stdout.readline().startswith("FIRST_PCM ")
        if scenario == "cancel":
            try:
                assert recv_frame(reader)[0] == 8
            except (ConnectionResetError, ssl.SSLEOFError):
                pass
            return
        send_frame(wire, 2, pcm[1281:] + (bytes(1) if scenario == "invalid" else b""))
        send_event(wire, task, "task-finished")


def main():
    with tempfile.TemporaryDirectory() as folder:
        cert, key = Path(folder) / "localhost.crt", Path(folder) / "localhost.key"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=127.0.0.1",
                        "-addext", "subjectAltName=IP:127.0.0.1,DNS:127.0.0.1"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        for scenario in ("speech", "cancel", "invalid"):
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen()
                listener.settimeout(5)
                endpoint = f"wss://127.0.0.1:{listener.getsockname()[1]}/api-ws/v1/inference"
                env = {**os.environ, "STREAM_ASR_TEST_KEY": "offline-fixture-key"}
                with subprocess.Popen([sys.argv[1], endpoint, str(cert), scenario], env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as proc:
                    try:
                        conn, _ = listener.accept()
                        with conn:
                            conn.settimeout(5)
                            with tls.wrap_socket(conn, server_side=True) as wire:
                                exercise(wire, proc, scenario)
                    except Exception as exc:
                        proc.kill()
                        _, err = proc.communicate()
                        raise AssertionError(f"{scenario}: {err.strip()}") from exc
                    _, err = proc.communicate(timeout=5)
                    assert proc.returncode == 0, f"{scenario}: {err.strip()}"


if __name__ == "__main__":
    main()
