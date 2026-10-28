#!/usr/bin/env python3
"""Convert fixed Zipformer ONNX inputs on ARM64; create a local candidate manifest.

Uses RKNN Toolkit2's public Python API. The input/configuration match the pinned
Rockchip Model Zoo Zipformer recipe, with batch=1 supplied explicitly.
No inference, system installation, tracked-manifest change or asset overwrite.
"""
import argparse
import copy
import importlib.metadata
import json
import platform
from pathlib import Path
import sys

from prepare_assets import ROOT, digest, present, safe_path

MODEL_ID = "zipformer-bilingual-zh-en-rk3588-fp16"


def candidate_manifest(manifest, outputs):
    result = copy.deepcopy(manifest)
    model = next(m for m in result["models"] if m["id"] == MODEL_ID)
    model["status"] = "candidate"
    model.pop("run_id", None)
    model["local_conversion"] = {
        "toolkit_version": "2.3.2", "target_platform": "rk3588",
        "batch": 1, "do_quantization": False,
        "validation": "not run; byte identity is not an inference acceptance result",
    }
    for role, record in outputs.items():
        old = model["roles"][role]
        model["files"] = [f for f in model["files"] if f["path"] != old]
        model["files"].append(record)
        model["roles"][role] = record["path"]
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True,
                        help="New project-relative directory below models/asr/")
    parser.add_argument("--candidate-manifest", default="models/manifest.local.json",
                        help="New JSON directly in models/ (relative to project root)")
    args = parser.parse_args(argv)
    out = safe_path(ROOT, args.output_dir)
    candidate = safe_path(ROOT, args.candidate_manifest)
    if not out.is_relative_to(ROOT / "models/asr") or out == ROOT / "models/asr":
        raise ValueError("Output must be a new directory below models/asr/")
    if candidate.parent != ROOT / "models" or candidate.suffix != ".json":
        raise ValueError("Candidate manifest must be a new JSON directly in models/")
    if out.exists() or candidate.exists():
        raise ValueError("Choose new output and manifest paths; existing files are preserved")
    if platform.machine() != "aarch64" or sys.version_info[:2] != (3, 10):
        raise ValueError("This conversion recipe is pinned to aarch64 Python 3.10")
    if importlib.metadata.version("rknn-toolkit2") != "2.3.2":
        raise ValueError("RKNN Toolkit2 must be 2.3.2")
    import onnx
    from rknn.api import RKNN

    manifest = json.loads((ROOT / "models/manifest.json").read_text())
    model = next(m for m in manifest["models"] if m["id"] == MODEL_ID)
    outputs, report = {}, {}
    # Validate every source before starting conversion.
    sources = {}
    for role in ["decoder", "joiner", "encoder"]:
        source_name = role + "-epoch-99-avg-1.onnx"
        record = next(f for f in model["files"] if Path(f["path"]).name == source_name)
        if not present(ROOT / "models", record):
            raise ValueError("Run prepare_assets.py fetch first: " + record["path"])
        sources[role] = ROOT / "models" / record["path"]
    out.mkdir(parents=True, exist_ok=False)
    for role, source in sources.items():
        graph = onnx.load(str(source))
        initializers = {x.name for x in graph.graph.initializer}
        names, shapes = [], []
        for tensor in graph.graph.input:
            if tensor.name in initializers:
                continue
            dims = []
            for dim in tensor.type.tensor_type.shape.dim:
                if dim.HasField("dim_value") and dim.dim_value > 0:
                    dims.append(dim.dim_value)
                elif dim.dim_param in ["N", "batch_size", "batch"]:
                    dims.append(1)
                else:
                    raise ValueError("Unsupported dynamic dimension in " + tensor.name)
            names.append(tensor.name)
            shapes.append(dims)
        path = out / (role + "-epoch-99-avg-1.rknn")
        runtime = RKNN(verbose=False)
        try:
            for stage, value in [
                ("config", lambda: runtime.config(target_platform="rk3588")),
                ("load_onnx", lambda: runtime.load_onnx(
                    model=str(source), inputs=names, input_size_list=shapes)),
                ("build", lambda: runtime.build(do_quantization=False)),
                ("export_rknn", lambda: runtime.export_rknn(str(path))),
            ]:
                rc = value()
                if rc not in (None, 0):
                    raise RuntimeError(stage + " failed: " + str(rc))
        finally:
            runtime.release()
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError("Converter did not produce a model: " + str(path))
        record = {"path": path.relative_to(ROOT / "models").as_posix(),
                  "size_bytes": path.stat().st_size, "sha256": digest(path)}
        outputs[role] = record
        baseline = next(f for f in model["files"] if f["path"] == model["roles"][role])
        report[role] = {
            "source_sha256": digest(source), "input_names": names, "input_shapes": shapes,
            "output": record, "matches_published_baseline": record["sha256"] == baseline["sha256"],
        }
    candidate.parent.mkdir(parents=True, exist_ok=True)
    with candidate.open("x") as stream:
        json.dump(candidate_manifest(manifest, outputs), stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    (out / "conversion-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("LOCAL_CANDIDATE_CREATED", candidate.relative_to(ROOT))
    print("No inference was run. Select this manifest explicitly in your local config.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, RuntimeError, StopIteration,
            importlib.metadata.PackageNotFoundError) as exc:
        print("ASR_CONVERSION_ERROR:", exc, file=sys.stderr)
        raise SystemExit(2)
