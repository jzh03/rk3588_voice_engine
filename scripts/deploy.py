#!/usr/bin/env python3
"""Local, versioned RK3588 deployment. No downloads, root changes or remote API."""
import argparse
from contextlib import contextmanager
import ctypes
from datetime import datetime, timezone
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import selectors
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time
import uuid

DEFAULT_ROOT = Path.home() / ".local/share/rk3588-voice-engine"
MODEL_LOCK = Path("/tmp/rk3588_voice_engine_model_validation.lock")
LOG_LIMIT = 500 * 1024 * 1024
SPACE_RESERVE = 3 * 1024**3
COMMANDS = {"start", "stop", "cancel", "new-session", "status", "help", "quit"}
PRIVATE_LIBRARIES = {"librknnrt.so", "librkllmrt.so", "libsherpa-onnx-c-api.so", "libonnxruntime.so"}

def save(path, value):
    path = Path(path)
    temp = path.with_name(path.name + ".tmp-" + uuid.uuid4().hex)
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    os.chmod(temp, 0o600)
    os.replace(temp, path)

def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()

def checked_id(value):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", value):
        raise ValueError("invalid release/run ID")
    return value

def inside(path, parent):
    return Path(path).resolve().is_relative_to(Path(parent).resolve())

def child(root, relative):
    relative = Path(relative)
    if relative.is_absolute() or ".." in relative.parts or not inside(root / relative, root):
        raise ValueError("path escapes managed directory: " + str(relative))
    return root / relative

def initialize(root):
    root = Path(root).expanduser().absolute()
    if root.exists() and root.is_symlink():
        raise ValueError("installation root must not be a symlink")
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    for name in ("assets", "releases", "runs"):
        p = root / name
        if p.is_symlink():
            raise ValueError("managed directory must not be a symlink: " + name)
        p.mkdir(exist_ok=True, mode=0o700)
    return root.resolve()

@contextmanager
def lock(path):
    path = Path(path)
    with path.open("a") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("busy: " + str(path)) from error
        yield
        fcntl.flock(stream, fcntl.LOCK_UN)

def read_config(path):
    text = Path(path).read_text()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        import yaml  # Packaging only; service/runtime use the frozen JSON YAML.
        return yaml.safe_load(text)

def copy_checked(source, target, expected=None):
    target.parent.mkdir(parents=True, exist_ok=True)
    h = hashlib.sha256()
    with Path(source).open("rb") as src, target.open("xb") as dst:
        for block in iter(lambda: src.read(4 * 1024 * 1024), b""):
            h.update(block)
            dst.write(block)
    value = h.hexdigest()
    if expected and value != expected:
        raise ValueError("asset hash mismatch: " + str(source))
    return value

def freeze_metadata(path):
    return {"sha256": digest(path), "size_bytes": path.stat().st_size}

def stage(root, project, binary, config, release_id):
    root = initialize(root)
    project, binary, config = Path(project).resolve(), Path(binary).resolve(), Path(config).resolve()
    release_id = checked_id(release_id)
    with lock(root / "deployment.lock"), lock(root / "running.lock"), lock(MODEL_LOCK):
        release = root / "releases" / release_id
        if release.exists():
            raise ValueError("release already exists")
        settings = read_config(config)
        manifests, assets = {}, {}
        for kind, key, collection in (("models", "models_manifest", "models"), ("deps", "deps_manifest", "dependencies")):
            source = (config.parent / settings["assets"][key]).resolve()
            raw = source.read_bytes()
            manifest = json.loads(raw)
            manifests[kind] = (source, raw)
            for entry in manifest[collection]:
                for item in entry.get("files", []):
                    rel = str(Path(kind) / item["path"])
                    src = child(source.parent, item["path"])
                    if not src.is_file() or src.stat().st_size != item["size_bytes"]:
                        raise ValueError("missing asset or size mismatch: " + str(src))
                    record = (src, item["sha256"], item["size_bytes"])
                    if rel in assets and assets[rel][1:] != record[1:]:
                        raise ValueError("conflicting asset declaration: " + rel)
                    assets[rel] = record
        pool_id = hashlib.sha256(manifests["models"][1] + b"\0" + manifests["deps"][1]).hexdigest()
        pool = root / "assets" / pool_id
        if pool.is_symlink():
            raise ValueError("asset pool must not be a symlink")
        required = (sum(v[2] for v in assets.values()) if not pool.exists() else 0) + binary.stat().st_size
        if shutil.disk_usage(root).free < required + SPACE_RESERVE:
            raise RuntimeError("insufficient space; preserve 3 GiB after staging")
        if not pool.exists():
            temp = root / "assets" / (".staging-" + uuid.uuid4().hex)
            temp.mkdir(mode=0o700)
            try:
                for rel, (src, sha, size) in assets.items():
                    dst = child(temp, rel)
                    copy_checked(src, dst, sha)
                    dst.chmod(0o444)
                for kind, (_, raw) in manifests.items():
                    p = temp / kind / "manifest.json"
                    p.parent.mkdir(parents=True, exist_ok=True)
                    p.write_bytes(raw)
                    p.chmod(0o444)
                save(temp / "pool.json", {"schema_version": 1, "id": pool_id,
                     "files": {rel: {"sha256": v[1], "size_bytes": v[2]} for rel, v in assets.items()}})
                (temp / "pool.json").chmod(0o444)
                temp.rename(pool)
            except BaseException:
                shutil.rmtree(temp)
                raise
        else:
            for kind, (_, raw) in manifests.items():
                if (pool / kind / "manifest.json").read_bytes() != raw:
                    raise ValueError("existing pool manifest mismatch")
            for rel, (_, _, size) in assets.items():
                dst = child(pool, rel)
                if not dst.is_file() or dst.stat().st_size != size:
                    raise ValueError("existing pool asset missing or wrong size: " + rel)
        temp = root / "releases" / (".staging-" + uuid.uuid4().hex)
        temp.mkdir(mode=0o700)
        try:
            (temp / "config").mkdir()
            (temp / "bin").mkdir()
            (temp / "lib").mkdir()
            copy_checked(binary, temp / "bin/voice_engine")
            (temp / "bin/voice_engine").chmod(0o555)
            copy_checked(Path(__file__), temp / "deploy.py")
            for kind in ("models", "deps"):
                (temp / kind).symlink_to("../../assets/" + pool_id + "/" + kind)
            libmap = {}
            for rel, (_, _, _) in assets.items():
                if rel.startswith("deps/") and Path(rel).name in PRIVATE_LIBRARIES:
                    libmap[Path(rel).name] = rel
            enabled = any(v.get("enabled") for v in settings.get("models", {}).values())
            if enabled and set(libmap) != PRIVATE_LIBRARIES:
                raise ValueError("incomplete private runtime library set")
            for name, rel in libmap.items():
                (temp / "lib" / name).symlink_to("../../../assets/" + pool_id + "/" + rel)
            original_settings = json.loads(json.dumps(settings))
            settings["assets"] = {"models_manifest": "../models/manifest.json", "deps_manifest": "../deps/manifest.json"}
            if settings.get("dialogue", {}).get("wake_enabled"):
                source_keywords = (config.parent / settings["dialogue"]["keywords_file"]).resolve()
                copy_checked(source_keywords, temp / "config/wake-keywords.txt")
                settings["dialogue"]["keywords_file"] = "wake-keywords.txt"
            (temp / "config/engine.yaml").write_text(json.dumps(settings, ensure_ascii=False, indent=2) + "\n")
            # Only deployment-relative asset/keyword paths change.
            before, after = json.loads(json.dumps(original_settings)), json.loads(json.dumps(settings))
            for value in (before, after):
                value.pop("assets", None)
                if "dialogue" in value:
                    value["dialogue"].pop("keywords_file", None)
            if before != after:
                raise ValueError("packaging changed operational parameters")
            inventory = {str(p.relative_to(temp)): freeze_metadata(p)
                         for p in temp.rglob("*") if p.is_file() and not p.is_symlink()}
            info = {"schema_version": 1, "id": release_id, "asset_pool": pool_id,
                    "files": inventory, "libraries": libmap,
                    "manifests": {k: hashlib.sha256(v[1]).hexdigest() for k, v in manifests.items()},
                    "source": {"project": str(project), "config_sha256": digest(config),
                               "binary_sha256": digest(binary)},
                    "parameters_unchanged": True, "parameters": after,
                    "log_limit_bytes": LOG_LIMIT, "host": {"machine": platform.machine(), "kernel": platform.release()},
                    "created_utc": datetime.now(timezone.utc).isoformat()}
            save(temp / "release.json", info)
            for p in temp.rglob("*"):
                if p.is_file() and not p.is_symlink():
                    p.chmod(0o555 if p.name == "voice_engine" else 0o444)
            temp.rename(release)
        except BaseException:
            shutil.rmtree(temp)
            raise
        return {"release": str(release), "asset_pool": pool_id, "new_bytes_required": required}

def resolve_release(root, release_id=None):
    root = Path(root).resolve()
    if release_id:
        path = root / "releases" / checked_id(release_id)
    else:
        pointer = root / "current"
        if not pointer.is_symlink():
            raise ValueError("no active release")
        path = pointer.resolve(strict=True)
    if not path.is_dir() or path.is_symlink() or path.parent != root / "releases":
        raise ValueError("release escapes version directory or is missing")
    return path

def release_info(root, release_id=None):
    release = resolve_release(root, release_id)
    info = json.loads((release / "release.json").read_text())
    if info["id"] != release.name or not re.fullmatch("[0-9a-f]{64}", info["asset_pool"]):
        raise ValueError("invalid release identity")
    pool = root / "assets" / info["asset_pool"]
    if not pool.is_dir() or pool.is_symlink():
        raise ValueError("missing asset pool")
    for rel, expected in info["files"].items():
        file = child(release, rel)
        if file.is_symlink() or not file.is_file() or freeze_metadata(file) != expected:
            raise ValueError("release file hash/size mismatch: " + rel)
    for name in ("models", "deps"):
        if (release / name).resolve() != pool / name:
            raise ValueError("asset link mismatch")
        if digest(pool / name / "manifest.json") != info["manifests"][name]:
            raise ValueError("asset manifest hash mismatch")
    for name, rel in info["libraries"].items():
        if Path(name).name != name or (release / "lib" / name).resolve() != child(pool, rel).resolve():
            raise ValueError("runtime link mismatch")
    return release, info

def engine_env(release):
    env = dict(os.environ)
    for name in ("LD_PRELOAD", "LD_AUDIT"):
        env.pop(name, None)
    env["LD_LIBRARY_PATH"] = str(release / "lib")
    return env

def verify_unlocked(root, release_id=None):
    release, info = release_info(root, release_id)
    env = engine_env(release)
    loaded = {}
    if info["libraries"]:
        result = subprocess.run(["ldd", str(release / "bin/voice_engine")], env=env,
                                text=True, capture_output=True, timeout=15, check=True)
        for name, rel in info["libraries"].items():
            match = re.search(r"^\s*" + re.escape(name) + r"\s+=>\s+(\S+)", result.stdout, re.M)
            expected = (root / "assets" / info["asset_pool"] / rel).resolve()
            if not match or Path(match.group(1)).resolve() != expected:
                raise ValueError("runtime loader would use a different library: " + name)
            loaded[name] = str(expected)
    result = subprocess.run([str(release / "bin/voice_engine"), "--config",
                            str(release / "config/engine.yaml"), "--check-config"],
                            env=env, cwd=release, capture_output=True, text=True, timeout=180)
    if result.returncode or "CONFIG_OK" not in result.stdout:
        raise ValueError("release preflight failed: " + (result.stderr or result.stdout)[-2000:])
    return {"release": info["id"], "status": "passed", "loaded_private_libraries": loaded,
            "config_asset_check": result.stdout.splitlines()[:2]}

def verify(root, release_id=None):
    root = initialize(root)
    with lock(root / "deployment.lock"), lock(MODEL_LOCK):
        return verify_unlocked(root, release_id)

def point(root, name, release_id):
    temp = root / ("." + name + "-" + uuid.uuid4().hex)
    temp.symlink_to("releases/" + checked_id(release_id))
    os.replace(temp, root / name)

def activate(root, release_id):
    root = initialize(root)
    with lock(root / "deployment.lock"), lock(root / "running.lock"), lock(MODEL_LOCK):
        result = verify_unlocked(root, release_id)
        old = resolve_release(root).name if (root / "current").is_symlink() else None
        if old and old != release_id:
            point(root, "previous", old)
        point(root, "current", release_id)
        result["previous"] = old
        return result

def rollback(root):
    root = initialize(root)
    pointer = root / "previous"
    if not pointer.is_symlink() or pointer.resolve().parent != root / "releases":
        raise ValueError("no valid previous release")
    return activate(root, pointer.resolve().name)

def apply_offline_guard():
    """Deny creation of non-UNIX sockets; inherited by engine and SDK children."""
    machine = platform.machine()
    specs = {"aarch64": (0xc00000b7, 198, 199), "x86_64": (0xc000003e, 41, 53)}
    if machine not in specs:
        raise RuntimeError("offline seccomp guard unsupported on " + machine)
    arch, socket_nr, socketpair_nr = specs[machine]
    class Filter(ctypes.Structure):
        _fields_ = [("code", ctypes.c_ushort), ("jt", ctypes.c_ubyte), ("jf", ctypes.c_ubyte), ("k", ctypes.c_uint32)]
    class Program(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ushort), ("filters", ctypes.POINTER(Filter))]
    # seccomp_data: arch at 4, syscall number at 0, first argument at 16.
    ops = [(0x20,0,0,4), (0x15,1,0,arch), (0x06,0,0,0x80000000),
           (0x20,0,0,0), (0x15,2,0,socket_nr), (0x15,1,0,socketpair_nr),
           (0x06,0,0,0x7fff0000), (0x20,0,0,16), (0x15,1,0,socket.AF_UNIX),
           (0x06,0,0,0x00050000 | errno.EAFNOSUPPORT), (0x06,0,0,0x7fff0000)]
    filters = (Filter * len(ops))(*(Filter(*op) for op in ops))
    program = Program(len(ops), filters)
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
    for option, arg in [(38, 1), (22, 2)]:
        ptr = ctypes.addressof(program) if option == 22 else 0
        if libc.prctl(option, arg, ptr, 0, 0):
            raise OSError(ctypes.get_errno(), "cannot install offline guard")
    results = {}
    for family in (socket.AF_INET, socket.AF_INET6):
        try:
            sock = socket.socket(family, socket.SOCK_STREAM)
        except OSError as error:
            if error.errno != errno.EAFNOSUPPORT:
                raise
            results[str(int(family))] = "blocked"
        else:
            sock.close()
            raise RuntimeError("offline guard did not block IP socket")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM):
        results["AF_UNIX"] = "allowed"
    return results

def offline_exec(args):
    # Acquire in the process that will exec the engine: its /proc/locks owner
    # remains the engine PID, satisfying the existing native lock contract.
    lock_path = args.lock_path.resolve()
    if not args.mock and lock_path != MODEL_LOCK.resolve():
        raise ValueError("real models must use the fixed project lock")
    lock_fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600)
    fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    os.set_inheritable(lock_fd, True)
    proof = apply_offline_guard()
    save(args.proof, {"status": "passed", "method": "inherited_seccomp_allow_AF_UNIX_only",
                     "socket_checks": proof, "pid": os.getpid(), "model_lock": str(lock_path)})
    command = [str(args.binary), "--config", str(args.config), "--control", "stdin",
               "--run-id", "engine", "--output-dir", str(args.output)]
    if args.mock:
        command.append("--mock")
    os.execve(str(args.binary), command, os.environ)

def run_bytes(path):
    total = 0
    for base, dirs, files in os.walk(path, followlinks=False):
        dirs[:] = [n for n in dirs if not (Path(base) / n).is_symlink()]
        for name in files:
            p = Path(base) / name
            if not p.is_symlink():
                total += p.stat().st_size
    return total

def prune_logs(root, active=None, reserve=0, limit=LOG_LIMIT):
    runs = root / "runs"
    entries = [p for p in runs.iterdir() if p.is_dir() and not p.is_symlink()
               and (p / "managed-run.json").is_file()]
    sizes = {p: run_bytes(p) for p in entries}
    total = sum(sizes.values())
    removed = []
    for p in sorted(entries, key=lambda item: item.stat().st_mtime_ns):
        if total + reserve <= limit:
            break
        if p == active or not (p / "exit.json").is_file():
            continue
        if p.parent != runs or not inside(p, runs):
            raise ValueError("unsafe log cleanup path")
        shutil.rmtree(p)
        total -= sizes[p]
        removed.append(p.name)
    if total + reserve > limit:
        raise RuntimeError("managed log quota exceeded; active/unfinished evidence retained")
    return removed

class Console:
    def __init__(self, path, size=1024*1024, copies=3):
        self.path, self.size, self.copies = path, size, copies
        self.stream = path.open("ab", buffering=0)
    def write(self, data):
        if self.stream.tell() + len(data) > self.size:
            self.stream.close()
            for index in range(self.copies-1, 0, -1):
                old = self.path.with_suffix("." + str(index))
                new = self.path.with_suffix("." + str(index+1))
                if old.exists(): os.replace(old, new)
            os.replace(self.path, self.path.with_suffix(".1"))
            self.stream = self.path.open("wb", buffering=0)
        self.stream.write(data)
    def close(self):
        self.stream.close()

def serve(root, release_id=None, run_id=None, mock=False):
    root = initialize(root)
    with lock(root / "running.lock"):
        with lock(MODEL_LOCK):
            result = verify_unlocked(root, release_id)
        release, info = release_info(root, result["release"])
        prune_logs(root, reserve=64*1024*1024, limit=info["log_limit_bytes"])
        run_id = checked_id(run_id or ("run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:6]))
        run = root / "runs" / run_id
        run.mkdir(mode=0o700)
        save(run / "managed-run.json", {"release": info["id"], "preflight": result})
        fifo = root / "control.fifo"
        if fifo.exists() or fifo.is_symlink():
            if not stat.S_ISFIFO(fifo.lstat().st_mode):
                raise ValueError("control path is not a FIFO")
            fifo.unlink()
        os.mkfifo(fifo, 0o600)
        descriptor = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        console = Console(run / "console.log")
        command = [sys.executable, str(release / "deploy.py"), "_exec", "--binary",
                   str(release / "bin/voice_engine"), "--config", str(release / "config/engine.yaml"),
                   "--output", str(run), "--proof", str(run / "network.json"),
                   "--lock-path", str(MODEL_LOCK)]
        if mock: command.append("--mock")
        process = None
        stop = {"requested": False}
        previous = {}
        for number in (signal.SIGINT, signal.SIGTERM):
            previous[number] = signal.signal(number, lambda *_: stop.update(requested=True))
        error = None
        try:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, cwd=release, env=engine_env(release), start_new_session=True)
            state = {"status": "running", "release": info["id"], "run_id": run_id,
                     "supervisor_pid": os.getpid(), "engine_pid": process.pid, "run_directory": str(run)}
            save(root / "status.json", state)
            save(run / "launcher.json", {**state, "command": command})
            print(json.dumps(state), flush=True)
            selector = selectors.DefaultSelector()
            selector.register(descriptor, selectors.EVENT_READ, "control")
            selector.register(process.stdout, selectors.EVENT_READ, "output")
            pending, stop_time, last_quota = b"", None, time.monotonic()
            while process.poll() is None:
                for key, _ in selector.select(.2):
                    data = os.read(key.fd, 8192)
                    if key.data == "output":
                        if data: console.write(data)
                        else: selector.unregister(key.fd)
                    else:
                        pending += data
                        if len(pending) > 4096:
                            pending = b""
                        while b"\n" in pending:
                            raw, pending = pending.split(b"\n", 1)
                            value = raw.decode("ascii", errors="ignore").strip()
                            if value in COMMANDS:
                                process.stdin.write((value + "\n").encode())
                                process.stdin.flush()
                                if value == "quit": stop_time = time.monotonic()
                if stop["requested"] and stop_time is None:
                    process.stdin.write(b"quit\n"); process.stdin.flush()
                    stop_time = time.monotonic()
                if stop_time is not None and time.monotonic() - stop_time > 10:
                    raise TimeoutError("engine did not stop within service grace period")
                if time.monotonic() - last_quota > 2:
                    prune_logs(root, active=run, limit=info["log_limit_bytes"])
                    last_quota = time.monotonic()
            tail = process.stdout.read()
            if tail: console.write(tail)
            code = process.wait()
        except BaseException as exc:
            error = str(exc)
            code = 1
        finally:
            if process and process.poll() is None:
                try:
                    process.stdin.write(b"quit\n"); process.stdin.flush()
                    process.wait(timeout=5)
                except (OSError, subprocess.TimeoutExpired):
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            if process:
                process.stdin.close(); process.stdout.close()
            os.close(descriptor); console.close()
            for number, handler in previous.items():
                signal.signal(number, handler)
            outcome = {"status": "stopped", "release": info["id"], "run_id": run_id,
                       "exit_code": code, "error": error}
            save(run / "exit.json", outcome)
            save(root / "status.json", outcome)
        return code

def send(root, command):
    if command not in COMMANDS:
        raise ValueError("unsupported local command")
    fifo = Path(root) / "control.fifo"
    if fifo.is_symlink() or not stat.S_ISFIFO(fifo.lstat().st_mode):
        raise ValueError("control path is not a FIFO")
    fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
    try: os.write(fd, (command + "\n").encode("ascii"))
    finally: os.close(fd)

def install_service(root, template):
    root = initialize(root)
    resolve_release(root)
    target = Path.home() / ".config/systemd/user/rk3588-voice-engine.service"
    target.parent.mkdir(parents=True, exist_ok=True)
    text = Path(template).read_text().replace("@ROOT@", str(root).replace("%", "%%"))
    text = text.replace("@PYTHON@", sys.executable.replace("%", "%%"))
    if target.exists() and target.read_text() != text:
        raise ValueError("different service unit already exists; preserve it for explicit update")
    target.write_text(text)
    subprocess.run(["systemd-analyze", "--user", "verify", str(target)], check=True, capture_output=True, text=True)
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
    return {"unit": str(target), "enabled_by_this_command": False, "started_by_this_command": False}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    sub = parser.add_subparsers(dest="action", required=True)
    p = sub.add_parser("stage")
    p.add_argument("--project", type=Path, required=True)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--config", type=Path, required=True)
    p.add_argument("--release", required=True)
    for name in ("verify", "activate"):
        p = sub.add_parser(name); p.add_argument("--release", required=(name == "activate"))
    sub.add_parser("rollback"); sub.add_parser("status")
    p = sub.add_parser("serve")
    p.add_argument("--release"); p.add_argument("--run-id"); p.add_argument("--mock", action="store_true")
    p = sub.add_parser("send"); p.add_argument("command", choices=sorted(COMMANDS))
    p = sub.add_parser("install-service"); p.add_argument("--template", type=Path, required=True)
    p = sub.add_parser("_exec", help=argparse.SUPPRESS)
    for name in ("binary", "config", "output", "proof"): p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--lock-path", type=Path, required=True)
    p.add_argument("--mock", action="store_true")
    args = parser.parse_args()
    try:
        if args.action == "_exec": offline_exec(args)
        elif args.action == "stage": print(json.dumps(stage(args.root,args.project,args.binary,args.config,args.release)))
        elif args.action == "verify": print(json.dumps(verify(args.root,args.release)))
        elif args.action == "activate": print(json.dumps(activate(args.root,args.release)))
        elif args.action == "rollback": print(json.dumps(rollback(args.root)))
        elif args.action == "serve": return serve(args.root,args.release,args.run_id,args.mock)
        elif args.action == "send": send(args.root,args.command)
        elif args.action == "install-service": print(json.dumps(install_service(args.root,args.template)))
        elif args.action == "status":
            root = initialize(args.root)
            data = {name: ((root/name).resolve().name if (root/name).is_symlink() else None) for name in ("current","previous")}
            data["last_run"] = json.loads((root/"status.json").read_text()) if (root/"status.json").exists() else None
            print(json.dumps(data))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, KeyError) as error:
        print("DEPLOY_ERROR: " + str(error), file=sys.stderr)
        return 2
if __name__ == "__main__":
    sys.exit(main())
