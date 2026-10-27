#!/usr/bin/env python3
"""Deployment failure/rollback/control regressions with a small synthetic asset."""
import hashlib
import importlib.util
import json
import multiprocessing
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location("deployment_manager", ROOT / "scripts/deploy.py")
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)

class Deployment(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ve-deploy-test-")
        self.project = Path(self.temp.name) / "project"
        self.root = Path(self.temp.name) / "installation"
        self.project.mkdir()
        deploy.MODEL_LOCK = Path(self.temp.name) / "model.lock"
        deploy.PRIVATE_LIBRARIES = set()  # Mock binary uses no private SDK libraries.
        for name in ("config", "models", "deps"):
            (self.project / name).mkdir()
        weight = self.project / "models/vad.bin"
        weight.write_bytes(b"synthetic asset for hash validation only")
        model = {"id":"test-vad","source_url":"fixture","revision":"fixture",
                 "license":"fixture","platform":"fixture","quantization":"fixture",
                 "toolkit_version":"fixture","runtime_version":"fixture","status":"candidate",
                 "files":[{"path":"vad.bin","size_bytes":weight.stat().st_size,
                           "sha256":deploy.digest(weight)}]}
        deploy.save(self.project / "models/manifest.json", {"schema_version":1,"models":[model]})
        deploy.save(self.project / "deps/manifest.json", {"schema_version":1,"dependencies":[]})
        self.settings = {"audio":{"source":"deployment-test-input","sink":"deployment-test-output","profile":"none"},
                         "models":{"vad":{"enabled":True,"id":"test-vad"}},
                         "assets":{"models_manifest":"../models/manifest.json","deps_manifest":"../deps/manifest.json"}}
        self.config = self.project / "config/engine.yaml"
        deploy.save(self.config, self.settings)
        deploy.stage(self.root,self.project,BINARY,self.config,"base")
        deploy.stage(self.root,self.project,BINARY,self.config,"next")

    def tearDown(self):
        self.temp.cleanup()

    def test_shared_assets_and_unchanged_parameters(self):
        a = json.loads((self.root/"releases/base/release.json").read_text())
        b = json.loads((self.root/"releases/next/release.json").read_text())
        self.assertEqual(a["asset_pool"],b["asset_pool"])
        self.assertEqual(a["parameters"],{k:v for k,v in self.settings.items() if k!="assets"})
        self.assertEqual(deploy.verify(self.root,"base")["status"],"passed")
        self.assertEqual((self.root/"releases/base/models").resolve(),(self.root/"releases/next/models").resolve())

    def test_bad_config_never_replaces_current(self):
        deploy.activate(self.root,"base")
        broken = self.root/"releases/next/config/engine.yaml"
        broken.chmod(0o600)
        value = json.loads(broken.read_text()); value["audio"]["frame_ms"] = 0
        deploy.save(broken,value)
        manifest = self.root/"releases/next/release.json"
        info = json.loads(manifest.read_text())
        info["files"]["config/engine.yaml"] = deploy.freeze_metadata(broken)
        deploy.save(manifest,info)
        with self.assertRaises(ValueError): deploy.activate(self.root,"next")
        self.assertEqual((self.root/"current").resolve().name,"base")

    def test_missing_and_corrupted_asset_fail_preflight(self):
        deploy.activate(self.root,"base")
        asset = self.root/"releases/next/models/vad.bin"
        original = asset.read_bytes()
        asset.unlink()
        with self.assertRaises(ValueError): deploy.verify(self.root,"next")
        asset.write_bytes(b"X"*len(original))
        with self.assertRaises(ValueError): deploy.verify(self.root,"next")
        self.assertEqual((self.root/"current").resolve().name,"base")

    def test_rollback_restores_version_and_running_lock_blocks_switch(self):
        deploy.activate(self.root,"base")
        deploy.activate(self.root,"next")
        with deploy.lock(self.root/"running.lock"):
            with self.assertRaises(RuntimeError): deploy.rollback(self.root)
        self.assertEqual((self.root/"current").resolve().name,"next")
        deploy.rollback(self.root)
        self.assertEqual((self.root/"current").resolve().name,"base")
        self.assertEqual((self.root/"previous").resolve().name,"next")

    def test_release_hash_and_path_escape_are_rejected(self):
        binary = self.root/"releases/next/bin/voice_engine"
        binary.chmod(0o600); binary.write_bytes(b"corrupt")
        with self.assertRaises(ValueError): deploy.verify(self.root,"next")
        with self.assertRaises(ValueError): deploy.resolve_release(self.root,"../base")
        with self.assertRaises(ValueError): deploy.child(self.root,"../escape")
        outside = Path(self.temp.name)/"outside";outside.mkdir()
        (self.root/"releases/escape").symlink_to(outside)
        with self.assertRaises(ValueError): deploy.resolve_release(self.root,"escape")

    def test_log_quota_keeps_active_unfinished_and_unmanaged_data(self):
        runs = self.root/"runs"
        for name in ("old","active","unfinished","unmanaged"):
            p=runs/name;p.mkdir();(p/"data").write_bytes(b"x"*80)
            if name!="unmanaged": deploy.save(p/"managed-run.json",{})
            if name in ("old","active"): deploy.save(p/"exit.json",{})
        external=Path(self.temp.name)/"outside-log";external.mkdir()
        (external/"precious").write_text("keep")
        (runs/"external").symlink_to(external)
        removed=deploy.prune_logs(self.root,active=runs/"active",limit=200)
        self.assertEqual(removed,["old"])
        for name in ("active","unfinished","unmanaged","external"):
            self.assertTrue((runs/name).exists())
        self.assertEqual((external/"precious").read_text(),"keep")
        with self.assertRaises(RuntimeError):
            deploy.prune_logs(self.root,active=runs/"active",limit=30)

    def test_mock_service_command_stop_and_inherited_offline_guard(self):
        deploy.activate(self.root,"base")
        def run():
            raise SystemExit(deploy.serve(self.root,run_id="service",mock=True))
        process=multiprocessing.get_context("fork").Process(target=run)
        process.start()
        try:
            proof=self.root/"runs/service/network.json"
            deadline=time.monotonic()+15
            while time.monotonic()<deadline and not proof.exists():
                if not process.is_alive():self.fail("service exited before offline proof")
                time.sleep(.025)
            self.assertTrue(proof.exists())
            result=json.loads(proof.read_text())
            self.assertEqual(result["socket_checks"],{"2":"blocked","10":"blocked","AF_UNIX":"allowed"})
            inode=str(deploy.MODEL_LOCK.stat().st_ino)
            owners=[line.split() for line in Path("/proc/locks").read_text().splitlines()]
            self.assertTrue(any(row[1]=="FLOCK" and row[4]==str(result["pid"])
                                and row[5].split(":")[-1]==inode for row in owners))
            deploy.send(self.root,"status")
            with self.assertRaises(RuntimeError):deploy.activate(self.root,"next")
            deploy.send(self.root,"quit")
            process.join(10)
            self.assertFalse(process.is_alive())
            self.assertEqual(process.exitcode,0)
            self.assertEqual(json.loads((self.root/"runs/service/exit.json").read_text())["exit_code"],0)
        finally:
            if process.is_alive():
                process.terminate();process.join(12)
            if process.is_alive():
                process.kill();process.join()

if __name__=="__main__":
    unittest.main()
