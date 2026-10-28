#!/usr/bin/env python3
"""Offline asset preparation safety/contract checks; no SDK, network or models."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import prepare_assets as assets
import convert_zipformer as converter
from convert_zipformer import candidate_manifest, MODEL_ID


def record(path, data, **kw):
    return dict(path=path, size_bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), **kw)


class AssetSetupTests(unittest.TestCase):
    def test_manifest_recipe_coverage(self):
        records, archives = assets.load_plan(ROOT)
        self.assertEqual(len({r["path"] for r in records}), len(records))
        for r in records:
            if "archive" in r:
                self.assertIn(r["archive"], archives)
            if "copy" in r and r["copy"].startswith("scripts/"):
                self.assertEqual(assets.digest(ROOT / r["copy"]), r["sha256"])

    def test_atomic_install_hash_and_no_clobber(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            good = record("models/sample.bin", b"good")
            assets.install_stream(root, good, io.BytesIO(b"good"))
            assets.install_stream(root, good, io.BytesIO(b"unused"))
            with self.assertRaises(ValueError):
                assets.install_stream(root, record("models/sample.bin", b"evil"), io.BytesIO(b"evil"))
            self.assertEqual((root / good["path"]).read_bytes(), b"good")
            with self.assertRaises(ValueError):
                assets.install_stream(root, record("bad.bin", b"good"), io.BytesIO(b"evil"))
            self.assertFalse((root / "bad.bin").exists())
            self.assertEqual(list(root.glob(".bad.bin.*")), [])

    def test_path_and_symlink_boundaries(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            for value in ["../escape", "/absolute", "a/../../escape", "C:/escape", "a\\escape", "."]:
                with self.assertRaises(ValueError):
                    assets.safe_path(root, value)
            (root / "link").symlink_to(root, target_is_directory=True)
            with self.assertRaises(ValueError):
                assets.install_stream(root, record("link/x", b"x"), io.BytesIO(b"x"))

    def test_tar_whitelist_and_traversal(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            path = root / "test.tar.bz2"
            with tarfile.open(path, "w:bz2") as out:
                for member, data in [("bundle/keep", b"good"), ("bundle/other", b"ignored")]:
                    info = tarfile.TarInfo(member); info.size = len(data)
                    out.addfile(info, io.BytesIO(data))
            ar = record("test.tar.bz2", path.read_bytes())
            target = record("models/keep", b"good", member="bundle/keep")
            assets.extract_selected(root, ar, [target])
            self.assertEqual((root / "models/keep").read_bytes(), b"good")
            self.assertFalse((root / "bundle").exists())
            with tarfile.open(path, "w:bz2") as out:
                info = tarfile.TarInfo("../escape"); info.size = 1
                out.addfile(info, io.BytesIO(b"x"))
            with self.assertRaises(ValueError):
                assets.extract_selected(root, record("test.tar.bz2", path.read_bytes()),
                                        [record("missing", b"x", member="bundle/missing")])

    def test_zip_and_tar_links_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            path = root / "test.zip"
            with zipfile.ZipFile(path, "w") as out:
                out.writestr("bundle/file", b"good")
            assets.extract_selected(root, record("test.zip", path.read_bytes()),
                                    [record("out", b"good", member="bundle/file")])
            path = root / "test.tar"
            with tarfile.open(path, "w") as out:
                info = tarfile.TarInfo("link"); info.type = tarfile.SYMTYPE
                info.linkname = "../outside"; out.addfile(info)
            with self.assertRaises(ValueError):
                assets.extract_selected(root, record("test.tar", path.read_bytes()),
                                        [record("new", b"x", member="link")])

    def test_candidate_manifest_rejects_nested_parent(self):
        with tempfile.TemporaryDirectory() as name:
            with patch.object(converter, "ROOT", Path(name)):
                with self.assertRaisesRegex(ValueError, "directly in models"):
                    converter.main(["--output-dir", "models/asr/new",
                                    "--candidate-manifest", "models/nested/candidate.json"])

    def test_candidate_does_not_relabel_baseline_verified(self):
        source = json.loads((ROOT / "models/manifest.json").read_text())
        before = copy.deepcopy(source)
        outputs = {r:record("asr/new/" + r + ".rknn", r.encode())
                   for r in ["encoder", "decoder", "joiner"]}
        result = candidate_manifest(source, outputs)
        self.assertEqual(source, before)
        changed = next(x for x in result["models"] if x["id"] == MODEL_ID)
        self.assertEqual(changed["status"], "candidate")
        self.assertNotIn("run_id", changed)
        for role in outputs:
            self.assertEqual(changed["roles"][role], outputs[role]["path"])
            self.assertIn(outputs[role], changed["files"])
        for previous, current in zip(source["models"], result["models"]):
            if previous["id"] != MODEL_ID:
                self.assertEqual(previous, current)


if __name__ == "__main__":
    unittest.main()
