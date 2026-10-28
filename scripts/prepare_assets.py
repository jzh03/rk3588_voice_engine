#!/usr/bin/env python3
"""Prepare pinned project-local assets. Default: read-only plan.

No system installation, inference, manifest edits or overwrite of different
files. Run fetch/verify under the project's validation flock.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import sys
import tarfile
import tempfile
from urllib.parse import urlsplit
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def relative_name(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError("Invalid relative path: " + repr(value))
    p = PurePosixPath(value)
    if not p.parts or p.is_absolute() or ".." in p.parts:
        raise ValueError("Path must remain inside the project: " + value)
    return p


def safe_path(root, relative):
    p = root
    for part in relative_name(relative).parts:
        p = p / part
        if p.is_symlink():
            raise ValueError("Symlink is not accepted: " + str(p))
    if not p.resolve().is_relative_to(root.resolve()):
        raise ValueError("Path escapes project: " + relative)
    return p


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as src:
        for block in iter(lambda: src.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def present(root, record):
    path = safe_path(root, record["path"])
    if not path.exists():
        return False
    if not path.is_file() or path.stat().st_size != record["size_bytes"]:
        raise ValueError("Existing file differs; preserved: " + record["path"])
    if digest(path) != record["sha256"]:
        raise ValueError("Existing file hash differs; preserved: " + record["path"])
    return True


def install_stream(root, record, stream):
    path = safe_path(root, record["path"])
    if present(root, record):
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    safe_path(root, record["path"])
    fd, temp = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    try:
        count, h = 0, hashlib.sha256()
        with os.fdopen(fd, "wb") as out:
            while True:
                block = stream.read(min(1024 * 1024, record["size_bytes"] - count + 1))
                if not block:
                    break
                count += len(block)
                if count > record["size_bytes"]:
                    raise ValueError("Oversized file: " + record["path"])
                out.write(block)
                h.update(block)
            out.flush()
            os.fsync(out.fileno())
        if count != record["size_bytes"] or h.hexdigest() != record["sha256"]:
            raise ValueError("Content differs: " + record["path"])
        safe_path(root, record["path"])
        # Atomic no-clobber install, including concurrent destination creation.
        os.link(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def download(root, record):
    if present(root, record):
        return
    url = record["url"]
    if urlsplit(url).scheme != "https":
        raise ValueError("Downloads require HTTPS")
    path = safe_path(root, record["path"])
    path.parent.mkdir(parents=True, exist_ok=True)
    part = safe_path(root, record["path"] + ".part")
    if part.exists() and (not part.is_file() or part.stat().st_size > record["size_bytes"]):
        raise ValueError("Invalid partial file; preserved: " + str(part))
    subprocess.run([
        "curl", "--fail", "--location", "--proto", "=https", "--proto-redir", "=https",
        "--connect-timeout", "15", "--max-time", "3600", "--retry", "3",
        "--max-filesize", str(record["size_bytes"]), "--continue-at", "-",
        "--output", str(part), url,
    ], check=True)
    with part.open("rb") as stream:
        install_stream(root, record, stream)
    part.unlink()


def extract_selected(root, archive, records):
    pending = {}
    for record in records:
        relative_name(record["member"])
        if not present(root, record):
            pending.setdefault(record["member"], []).append(record)
    if not pending:
        return
    archive_path = safe_path(root, archive["path"])
    if not present(root, archive):
        raise ValueError("Archive must be verified before extraction")
    seen = set()

    def consume(name, is_regular, size, opener):
        name = relative_name(name).as_posix()
        if name in seen:
            raise ValueError("Duplicate archive member: " + name)
        seen.add(name)
        if name not in pending:
            return
        if not is_regular:
            raise ValueError("Selected member is not a regular file: " + name)
        for record in pending.pop(name):
            if size != record["size_bytes"]:
                raise ValueError("Archive member size differs: " + name)
            with opener() as stream:
                install_stream(root, record, stream)

    if archive_path.name.endswith(".zip"):
        with zipfile.ZipFile(archive_path) as src:
            for member in src.infolist():
                mode = member.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise ValueError("Archive symlink is not accepted: " + member.filename)
                consume(member.filename, not member.is_dir(), member.file_size,
                        lambda m=member: src.open(m))
    else:
        with tarfile.open(archive_path, "r|*") as src:
            for member in src:
                if member.issym() or member.islnk() or member.isdev() or member.isfifo():
                    raise ValueError("Archive links/special files are not accepted: " + member.name)
                consume(member.name, member.isfile(), member.size,
                        lambda m=member: src.extractfile(m))
    if pending:
        raise ValueError("Archive members missing: " + ", ".join(pending))


def load_plan(root):
    sources = json.loads((root / "scripts/assets/sources.json").read_text())
    identities = {}
    for base, collection in [("models", "models"), ("deps", "dependencies")]:
        manifest = json.loads((root / base / "manifest.json").read_text())
        for entry in manifest[collection]:
            for file in entry["files"]:
                path = base + "/" + file["path"]
                record = dict(file, path=path)
                if path in identities and identities[path] != record:
                    raise ValueError("Conflicting asset identity: " + path)
                identities[path] = record
    records, seen = [], set()
    for recipe in sources["files"]:
        path = recipe["path"]
        if path not in identities or path in seen:
            raise ValueError("Missing/duplicate manifest mapping: " + path)
        seen.add(path)
        records.append(dict(identities[path], **{k: v for k, v in recipe.items() if k != "path"}))
    if seen != set(identities):
        raise ValueError("Some manifest files have no acquisition recipe")
    records.extend(sources.get("extras", []))
    for record in records:
        relative_name(record["path"])
        if record.get("group") not in ["models", "deps", "converter"]:
            raise ValueError("Unknown group")
        if sum(k in record for k in ["url", "archive", "copy", "generated_by"]) != 1:
            raise ValueError("Exactly one acquisition method is required")
        if not isinstance(record.get("size_bytes"), int) or record["size_bytes"] < 0:
            raise ValueError("Invalid file size")
        if len(record.get("sha256", "")) != 64:
            raise ValueError("Missing SHA-256")
    return records, sources["archives"]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["plan", "fetch", "verify"], nargs="?", default="plan")
    parser.add_argument("--group", choices=["runtime", "models", "deps", "converter", "all"],
                        default="runtime", help="runtime=models+deps; converter=Toolkit2 wheel")
    args = parser.parse_args(argv)
    records, archives = load_plan(ROOT)
    records = [r for r in records if args.group == "all" or
               (args.group == "runtime" and r["group"] in ["models", "deps"]) or
               r["group"] == args.group]
    if args.action == "plan":
        for r in records:
            method = next(k for k in ["url", "archive", "copy", "generated_by"] if k in r)
            print(method.upper(), r["path"], "->", r[method])
        print("PLAN_OK files=" + str(len(records)) + " network=none writes=none")
        return 0
    if args.action == "verify":
        missing = [r["path"] for r in records if not present(ROOT, r)]
        if missing:
            for path in missing:
                print("MISSING", path)
            return 2
        print("ASSET_FILES_OK files=" + str(len(records)) + " (identity only, no inference)")
        return 0
    for r in records:
        if "url" in r:
            download(ROOT, r)
    by_archive = {}
    for r in records:
        if "archive" in r:
            by_archive.setdefault(r["archive"], []).append(r)
    for key, members in by_archive.items():
        download(ROOT, archives[key])
        extract_selected(ROOT, archives[key], members)
    for r in records:
        if "copy" in r and not present(ROOT, r):
            with safe_path(ROOT, r["copy"]).open("rb") as stream:
                install_stream(ROOT, r, stream)
    pending = [r["path"] for r in records if "generated_by" in r and not present(ROOT, r)]
    for path in pending:
        print("CONVERSION_REQUIRED", path)
    print("FETCH_OK pending_generated=" + str(len(pending)))
    print("Published baseline: use prepare_assets.py verify. Local candidate: select its")
    print("manifest in your local config, then run voice_engine --config ... --check-config.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.CalledProcessError, tarfile.TarError,
            zipfile.BadZipFile) as exc:
        print("ASSET_SETUP_ERROR:", exc, file=sys.stderr)
        raise SystemExit(2)
