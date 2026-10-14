#!/usr/bin/env python3
"""Check synthetic local manifests through the CLI; never fetch or run models."""

import argparse
import copy
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def file_record(path, contents):
    return {
        "path": path,
        "size_bytes": len(contents),
        "sha256": hashlib.sha256(contents).hexdigest(),
    }


class Fixture:
    def __init__(self, root):
        self.root = root
        self.config_path = root / "config" / "settings.yaml"
        self.models_path = root / "models" / "manifest.json"
        self.deps_path = root / "deps" / "manifest.json"
        for directory in (self.config_path.parent, self.models_path.parent,
                          self.deps_path.parent, root / "unrelated-cwd"):
            directory.mkdir(parents=True)
        model_bytes = b"synthetic model fixture, not an executable model\n"
        runtime_bytes = b"synthetic runtime fixture, not a shared library\n"
        (self.models_path.parent / "sample.bin").write_bytes(model_bytes)
        (self.deps_path.parent / "runtime.bin").write_bytes(runtime_bytes)
        self.model = {
            "id": "sample-asr",
            "source_url": "https://example.invalid/synthetic-model",
            "revision": "synthetic-revision",
            "license": "synthetic-test-only",
            "platform": "rk3588",
            "quantization": "synthetic",
            "toolkit_version": "synthetic-1",
            "runtime_version": "synthetic-1",
            "status": "candidate",
            "files": [file_record("sample.bin", model_bytes)],
            "dependencies": [{"id": "sample-runtime", "version": "test-1"}],
        }
        self.dependency = {
            "id": "sample-runtime",
            "source_url": "https://example.invalid/synthetic-runtime",
            "revision": "synthetic-revision",
            "license": "synthetic-test-only",
            "version": "test-1",
            "files": [file_record("runtime.bin", runtime_bytes)],
        }
        self.models = {"schema_version": 1, "models": [self.model]}
        self.deps = {"schema_version": 1, "dependencies": [self.dependency]}
        self.enabled = {"asr": "sample-asr"}
        self.write_models = True
        self.write_deps = True
        self.raw_models = None
        self.raw_deps = None
        self.models_ref = "../models/manifest.json"
        self.deps_ref = "../deps/manifest.json"
        self.mock = False

    def write(self):
        if self.write_models:
            self.models_path.write_text(
                self.raw_models if self.raw_models is not None else json.dumps(self.models),
                encoding="utf-8",
            )
        if self.write_deps:
            self.deps_path.write_text(
                self.raw_deps if self.raw_deps is not None else json.dumps(self.deps),
                encoding="utf-8",
            )
        lines = [
            "audio:", '  source: "synthetic-input"', '  sink: "synthetic-output"',
            '  profile: "synthetic-profile"', "models:",
        ]
        for component in ("vad", "asr", "llm", "tts", "kws"):
            lines += ["  " + component + ":",
                      "    enabled: " + ("true" if component in self.enabled else "false")]
            if component in self.enabled:
                lines.append("    id: " + json.dumps(self.enabled[component]))
        lines += ["assets:", "  models_manifest: " + json.dumps(self.models_ref),
                  "  deps_manifest: " + json.dumps(self.deps_ref)]
        self.config_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


@dataclass
class Case:
    name: str
    mutate: object
    code: int = 2
    terms: tuple = ()
    counts: str = "ASSETS_OK models=1 dependencies=1 files=2"


def no_assets(fixture):
    fixture.enabled = {}
    fixture.write_models = False
    fixture.write_deps = False


def no_dependencies(fixture):
    fixture.model.pop("dependencies")
    fixture.write_deps = False


def disabled_missing_assets(fixture):
    model = copy.deepcopy(fixture.model)
    model["id"] = "disabled-tts"
    model["files"][0]["path"] = "never-downloaded.bin"
    model["dependencies"] = [{"id": "never-downloaded", "version": "none"}]
    fixture.models["models"].append(model)
    dependency = copy.deepcopy(fixture.dependency)
    dependency["id"] = "disabled-runtime"
    dependency["files"][0]["path"] = "never-downloaded.so"
    fixture.deps["dependencies"].append(dependency)


def two_models_shared_dependency(fixture):
    model = copy.deepcopy(fixture.model)
    model["id"] = "sample-tts"
    fixture.models["models"].append(model)
    fixture.enabled["tts"] = "sample-tts"


def lexical_duplicate_file(fixture):
    record = copy.deepcopy(fixture.model["files"][0])
    record["path"] = "./sample.bin"
    fixture.model["files"].append(record)


def file_symlink(fixture, escape):
    target = fixture.root / "outside.bin" if escape else fixture.models_path.parent / "sample.bin"
    if escape:
        target.write_bytes(b"outside the model directory")
    (fixture.models_path.parent / "alias.bin").symlink_to(target)
    fixture.model["files"][0] = file_record("alias.bin", target.read_bytes())


def large_stream(fixture):
    contents = bytes(range(256)) * 4097
    (fixture.models_path.parent / "sample.bin").write_bytes(contents)
    fixture.model["files"][0] = file_record("sample.bin", contents)


def shell_filename(fixture):
    name = "$(touch should-not-exist);' model.bin"
    contents = b"filename is data, never shell syntax"
    (fixture.models_path.parent / name).write_bytes(contents)
    fixture.model["files"][0] = file_record(name, contents)


def cases():
    result = [
        Case("valid candidates", lambda f: None, 0),
        Case("all disabled without manifests", no_assets, 0,
             counts="ASSETS_OK models=0 dependencies=0 files=0"),
        Case("enabled without dependencies manifest", no_dependencies, 0,
             counts="ASSETS_OK models=1 dependencies=0 files=1"),
        Case("disabled entries may lack asset files", disabled_missing_assets, 0),
        Case("shared dependency checked once", two_models_shared_dependency, 0,
             counts="ASSETS_OK models=2 dependencies=1 files=3"),
        Case("same model referenced twice checked once",
             lambda f: f.enabled.update(tts="sample-asr"), 0),
        Case("uppercase hash", lambda f: f.model["files"][0].update(
            sha256=f.model["files"][0]["sha256"].upper()), 0),
        Case("asset larger than hash buffer", large_stream, 0),
        Case("shell metacharacters in filename", shell_filename, 0),
        Case("internal asset symlink", lambda f: file_symlink(f, False), 0),
        Case("verified with evidence reference",
             lambda f: f.model.update(status="verified", run_id="synthetic-run"), 0),
        Case("absolute manifest paths",
             lambda f: (setattr(f, "models_ref", str(f.models_path)),
                        setattr(f, "deps_ref", str(f.deps_path))), 0),
        Case("additional metadata preserved",
             lambda f: f.model.update(io={"sample_rate": 16000},
                                      conversion={"notes": "synthetic fixture"}), 0),
        Case("missing models manifest", lambda f: setattr(f, "write_models", False),
             terms=("assets.models_manifest", "manifest.json")),
        Case("missing deps manifest", lambda f: setattr(f, "write_deps", False),
             terms=("assets.deps_manifest", "manifest.json")),
        Case("unknown enabled model", lambda f: f.enabled.update(asr="unknown"),
             terms=("models.asr.id", "unknown", "manifest.json")),
        Case("duplicate model id", lambda f: f.models["models"].append(copy.deepcopy(f.model)),
             terms=("duplicate", "sample-asr")),
        Case("duplicate dependency id",
             lambda f: f.deps["dependencies"].append(copy.deepcopy(f.dependency)),
             terms=("duplicate", "sample-runtime")),
        Case("duplicate dependency declaration",
             lambda f: f.model["dependencies"].append(copy.deepcopy(f.model["dependencies"][0])),
             terms=("duplicate", "sample-runtime")),
        Case("unknown required dependency", lambda f: f.deps.update(dependencies=[]),
             terms=("missing dependency", "sample-runtime")),
        Case("dependency version mismatch", lambda f: f.dependency.update(version="wrong"),
             terms=("version", "test-1")),
        Case("dependencies type", lambda f: f.model.update(dependencies={}),
             terms=("dependencies",)),
        Case("dependency declaration type", lambda f: f.model.update(dependencies=[1]),
             terms=("dependencies",)),
        Case("dependency declaration version type",
             lambda f: f.model["dependencies"][0].update(version=1), terms=("version",)),
        Case("verified without evidence", lambda f: f.model.update(status="verified"),
             terms=("run_id",)),
        Case("candidate evidence type", lambda f: f.model.update(run_id=123),
             terms=("run_id",)),
        Case("unsupported status", lambda f: f.model.update(status="downloaded"),
             terms=("status",)),
        Case("missing model file", lambda f: f.model["files"][0].update(path="absent.bin"),
             terms=("path", "absent.bin")),
        Case("missing dependency file", lambda f: f.dependency["files"][0].update(path="absent.so"),
             terms=("assets.deps_manifest", "absent.so")),
        Case("model file is directory", lambda f: f.model["files"][0].update(path="."),
             terms=("regular file",)),
        Case("size mismatch", lambda f: f.model["files"][0].update(size_bytes=1),
             terms=("size_bytes", "sample.bin")),
        Case("hash mismatch", lambda f: f.model["files"][0].update(sha256="0" * 64),
             terms=("sha256", "sample.bin")),
        Case("malformed hash", lambda f: f.model["files"][0].update(sha256="z" * 64),
             terms=("sha256",)),
        Case("short hash", lambda f: f.model["files"][0].update(sha256="a" * 63),
             terms=("sha256",)),
        Case("negative size", lambda f: f.model["files"][0].update(size_bytes=-1),
             terms=("size_bytes",)),
        Case("floating size", lambda f: f.model["files"][0].update(size_bytes=1.5),
             terms=("size_bytes",)),
        Case("boolean size", lambda f: f.model["files"][0].update(size_bytes=True),
             terms=("size_bytes",)),
        Case("string size", lambda f: f.model["files"][0].update(size_bytes="1"),
             terms=("size_bytes",)),
        Case("duplicate file", lexical_duplicate_file, terms=("duplicate file",)),
        Case("parent traversal", lambda f: f.model["files"][0].update(path="../outside.bin"),
             terms=("path", "manifest directory")),
        Case("normalized parent traversal",
             lambda f: f.model["files"][0].update(path="folder/../sample.bin"),
             terms=("path", "manifest directory")),
        Case("absolute asset path",
             lambda f: f.model["files"][0].update(path=str(f.models_path.parent / "sample.bin")),
             terms=("path", "manifest directory")),
        Case("symlink escape", lambda f: file_symlink(f, True),
             terms=("path", "escapes")),
        Case("NUL file path", lambda f: f.model["files"][0].update(path="sample.bin\0x"),
             terms=("path",)),
        Case("empty files", lambda f: f.model.update(files=[]), terms=("files",)),
        Case("files type", lambda f: f.model.update(files={}), terms=("files",)),
        Case("file entry type", lambda f: f.model.update(files=["sample.bin"]),
             terms=("files",)),
        Case("models schema version", lambda f: f.models.update(schema_version=2),
             terms=("schema_version",)),
        Case("models schema version string", lambda f: f.models.update(schema_version="1"),
             terms=("schema_version",)),
        Case("deps schema version", lambda f: f.deps.update(schema_version=2),
             terms=("schema_version",)),
        Case("models collection type", lambda f: f.models.update(models={}),
             terms=("models",)),
        Case("model entry type", lambda f: f.models.update(models=[1]),
             terms=("models",)),
        Case("deps collection type", lambda f: f.deps.update(dependencies={}),
             terms=("dependencies",)),
        Case("malformed model JSON", lambda f: setattr(f, "raw_models", "{"),
             terms=("invalid JSON", "manifest.json")),
        Case("scalar model JSON", lambda f: setattr(f, "raw_models", "1"),
             terms=("root",)),
        Case("duplicate root JSON key",
             lambda f: setattr(f, "raw_models", '{"schema_version":1,"schema_version":1,"models":[]}'),
             terms=("duplicate JSON key", "schema_version")),
        Case("duplicate nested JSON key",
             lambda f: setattr(f, "raw_models", '{"schema_version":1,"models":[{"id":"one","id":"two"}]}'),
             terms=("duplicate JSON key", "id")),
        Case("duplicate dependency JSON key",
             lambda f: setattr(f, "raw_deps", '{"schema_version":1,"dependencies":[],"dependencies":[]}'),
             terms=("duplicate JSON key", "dependencies")),
        Case("excessive JSON nesting",
             lambda f: setattr(f, "raw_models", '[' * 70 + '0' + ']' * 70),
             terms=("nesting",)),
        Case("oversized JSON metadata",
             lambda f: setattr(f, "raw_models", ' ' * (4 * 1024 * 1024 + 1)),
             terms=("4 MiB",)),
    ]
    for field in ("id", "source_url", "revision", "license", "platform", "quantization",
                  "toolkit_version", "runtime_version", "status"):
        result.append(Case("missing model " + field,
                           lambda f, key=field: f.model.pop(key), terms=(field,)))
        result.append(Case("model metadata type " + field,
                           lambda f, key=field: f.model.update({key: 123}), terms=(field,)))
    for field in ("id", "source_url", "revision", "license", "version"):
        result.append(Case("missing dependency " + field,
                           lambda f, key=field: f.dependency.pop(key), terms=(field,)))
        result.append(Case("dependency metadata type " + field,
                           lambda f, key=field: f.dependency.update({key: []}), terms=(field,)))
    for field in ("path", "size_bytes", "sha256"):
        result.append(Case("missing file " + field,
                           lambda f, key=field: f.model["files"][0].pop(key), terms=(field,)))

    def conflict(fixture):
        two_models_shared_dependency(fixture)
        fixture.models["models"][1]["dependencies"][0]["version"] = "different"

    result.append(Case("conflicting dependency versions", conflict,
                       terms=("conflicting", "sample-runtime")))

    def mock_missing(fixture):
        fixture.mock = True
        fixture.write_models = False
        fixture.write_deps = False

    result.append(Case("mock bypasses enabled asset loading", mock_missing, 0))
    return result


def run_case(executable, directory, case):
    fixture = Fixture(directory)
    case.mutate(fixture)
    fixture.write()
    command = [str(executable), "--config", str(fixture.config_path)]
    if fixture.mock:
        command += ["--mock", "--control", "stdin", "--output-dir",
                    str(directory / "runs"), "--run-id", "asset-fixture"]
    else:
        command += ["--check-config"]
    completed = subprocess.run(command, input="quit\n", capture_output=True,
                               encoding="utf-8", errors="replace", timeout=10,
                               cwd=directory / "unrelated-cwd", check=False)
    problems = []
    if completed.returncode != case.code:
        problems.append("exit {}, expected {}".format(completed.returncode, case.code))
    for term in case.terms:
        if term not in completed.stderr:
            problems.append("stderr missing " + repr(term))
    if case.code == 0:
        expected = "MOCK_READY" if fixture.mock else case.counts
        if expected not in completed.stdout:
            problems.append("stdout missing " + repr(expected))
    elif "CONFIG_ERROR" not in completed.stderr:
        problems.append("missing CONFIG_ERROR")
    if (directory / "unrelated-cwd" / "should-not-exist").exists():
        problems.append("filename executed as shell code")
    if problems:
        return "{}: {}\nstdout: {}\nstderr: {}".format(
            case.name, "; ".join(problems), completed.stdout, completed.stderr)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    failures = []
    test_cases = cases()
    with tempfile.TemporaryDirectory(prefix="voice-assets-") as temporary:
        for index, case in enumerate(test_cases):
            try:
                failure = run_case(executable, Path(temporary) / str(index), case)
            except (OSError, subprocess.TimeoutExpired) as error:
                failure = "{}: {}".format(case.name, error)
            if failure:
                failures.append(failure)
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("{} asset checks passed".format(len(test_cases)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
