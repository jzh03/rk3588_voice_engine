#!/usr/bin/env python3
"""Opt-in A13: three process cold starts x five turns, bad candidate, rollback x five.
Uses frozen releases, user-systemd supervision and private PipeWire; no headset.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import yaml
from audio_probe import PrivatePulse
from wake_runtime import Feeder, Evidence, pcm, save

PROJECT = Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location("deployment",PROJECT/"scripts/deploy.py")
deploy=importlib.util.module_from_spec(spec);spec.loader.exec_module(deploy)

class RunProcess:
    def __init__(self, root, run_id):
        self.root,self.run_id,self.returncode=root,run_id,None
    def poll(self):
        p=self.root/"runs"/self.run_id/"exit.json"
        if p.exists():
            self.returncode=json.loads(p.read_text())["exit_code"]
        return self.returncode

def systemctl(*args):
    result=subprocess.run(["systemctl","--user",*args],capture_output=True,text=True,timeout=25)
    if result.returncode:
        raise RuntimeError("systemctl failed: "+result.stderr)
    return result.stdout

def wait_for(predicate,timeout=180):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        value=predicate()
        if value:return value
        time.sleep(.05)
    raise TimeoutError("deployment startup/control")

def cycle(root, run_id, server, feeder, wake, query, output):
    unit="rk3588-voice-engine-"+run_id
    command=["systemd-run","--user","--unit="+unit,"--property=Type=exec",
             "--property=RemainAfterExit=yes","--property=Restart=no",
             "--property=NoNewPrivileges=yes","--property=RestrictAddressFamilies=AF_UNIX",
             "--property=KillMode=control-group","--property=TimeoutStopSec=15",
             "--property=RuntimeMaxSec=600"]
    for key,value in server.env.items():
        if key.startswith(("XDG_","PULSE_","PIPEWIRE_","WIREPLUMBER_","DBUS_")):
            command.append("--setenv="+key+"="+value)
    command += [sys.executable,str(root/"current/deploy.py"),"--root",str(root),"serve","--run-id",run_id]
    case={"run_id":run_id,"unit":unit,"cold_start":"new supervised process; OS/page cache not reset","rounds":[]}
    state_path=root/"status.json"
    run=root/"runs"/run_id
    try:
        result=subprocess.run(command,capture_output=True,text=True,timeout=20)
        save(output/(run_id+"-launch.json"),{"command":command,"returncode":result.returncode,
                                          "stdout":result.stdout,"stderr":result.stderr})
        if result.returncode:raise RuntimeError(result.stderr)
        def started():
            if not state_path.exists():return None
            state=json.loads(state_path.read_text())
            return state if state.get("run_id")==run_id and state["status"]=="running" else None
        state=wait_for(started)
        process=RunProcess(root,run_id)
        evidence=Evidence(run,process)
        armed=evidence.wait("wake_listening",seconds=180)
        time.sleep(1)
        evidence.refresh()
        waits=[e for e in evidence.events if e["event"]=="device_wait" and e["seq"]>armed["seq"]]
        if waits:armed=evidence.wait("wake_listening",waits[-1]["seq"],15)
        else:
            armed=max((e for e in evidence.events if e["event"]=="wake_listening"),key=lambda e:e["seq"])
        network=json.loads((run/"network.json").read_text())
        assert network["status"]=="passed"
        assert network["socket_checks"]=={"2":"blocked","10":"blocked","AF_UNIX":"allowed"}
        maps=Path("/proc/"+str(state["engine_pid"])+"/maps").read_text()
        (output/(run_id+"-loaded-maps.txt")).write_text(maps)
        release,info=deploy.release_info(root,state["release"])
        expected={name:str((root/"assets"/info["asset_pool"]/rel).resolve())
                  for name,rel in info["libraries"].items()}
        assert all(path in maps for path in expected.values()),expected
        assert "/projects/rk3588_voice_engine/deps/" not in maps
        case.update(release=state["release"],binary_sha256=info["files"]["bin/voice_engine"]["sha256"],
                    network=network,loaded_private_libraries=expected)
        feeder.play(wake)
        detected=evidence.wait("wake_detected",armed["seq"],15)
        opened=evidence.wait("dialogue_window_opened",detected["seq"],5)
        after=opened["seq"]
        for index in range(5):
            if index:
                opened=evidence.wait("dialogue_window_opened",after,10)
            with feeder.lock:pending=len(feeder.frames)
            if pending:time.sleep(pending*.02+.1)
            feeder.play(query)
            complete=evidence.wait("turn_completed",opened["seq"],120,
                                   lambda e:not e["data"]["empty_input"])
            assert complete["data"]["history_committed"]
            evidence.refresh()
            selected=[e for e in evidence.events if opened["seq"]<=e["seq"]<=complete["seq"]]
            assert not any(e["event"] in ("model_error","restart_required","barge_in_detected") for e in selected)
            timing=next(e["data"] for e in selected if e["event"]=="playback_drain_requested")
            assert timing["tts_speed"]==.85 and timing["tts_prebuffer_ms"]==1500
            assert not timing["tts_first_segment_override"] and not timing["tts_sentence_mode"]
            assert all((x["min_chars"],x["target_chars"],x["boundary_lookahead_chars"])==(12,12,2)
                       for x in timing["tts_segment_timings"])
            case["rounds"].append({"round":index+1,"generation_id":complete["generation_id"],"completed":True})
            print(run_id+" turn "+str(index+1)+"/5 completed",flush=True)
            after=complete["seq"]
        deploy.send(root,"quit")
        wait_for(lambda:(run/"exit.json").exists(),20)
        outcome=json.loads((run/"exit.json").read_text())
        assert outcome["exit_code"]==0,outcome
        case["exit"]=outcome
        case["service_properties"]=systemctl("show",unit,
              "-p","NoNewPrivileges","-p","RestrictAddressFamilies","-p","ExecMainStatus","-p","MainPID")
        assert "ExecMainStatus=0" in case["service_properties"]
        evidence.refresh()
        samples=[e["data"] for e in evidence.events if e["event"]=="resource_sample"]
        case["queue_observations"]={k:max(s[k] for s in samples) for k in
              ["capture_overflows","playback_overflows","playback_underruns","capture_holes","dropped_events"]}
        case["status"]="passed"
        save(output/(run_id+".json"),case)
        return case
    finally:
        subprocess.run(["systemctl","--user","stop",unit],capture_output=True,timeout=25)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root",type=Path,required=True)
    parser.add_argument("--config",type=Path,required=True)
    parser.add_argument("--wake-wav",type=Path,required=True)
    parser.add_argument("--query-wav",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--batch-id",default="20261027-r2")
    args=parser.parse_args()
    def interrupted(signum, frame):
        raise KeyboardInterrupt("signal "+str(signum))
    signal.signal(signal.SIGTERM,interrupted)
    args.output.mkdir(parents=True,exist_ok=False)
    root=args.root.resolve()
    report={"status":"running","scope":"A13 local private-audio functional deployment; no physical headset, reboot or A14 soak",
            "cold_starts":3,"turns_per_start":5,"rollback_turns":5,"cycles":[]}
    save(args.output/"review.json",report)
    feeder=None
    try:
        settings=deploy.read_config(args.config)
        original=json.loads(json.dumps(settings))
        settings["audio"].update(source="ve_deploy_source",sink="ve_test_sink",profile="none")
        for key in ("models_manifest","deps_manifest"):
            settings["assets"][key]=str((args.config.parent/settings["assets"][key]).resolve())
        settings["dialogue"]["keywords_file"]=str((args.config.parent/settings["dialogue"]["keywords_file"]).resolve())
        private=args.output/"private-config.yaml"
        save(private,settings)
        normalized={k:v for k,v in original.items() if k not in ("audio","assets")}
        comparison={k:v for k,v in settings.items() if k not in ("audio","assets")}
        normalized["dialogue"]=dict(normalized["dialogue"]);comparison["dialogue"]=dict(comparison["dialogue"])
        normalized["dialogue"].pop("keywords_file");comparison["dialogue"].pop("keywords_file")
        assert normalized==comparison
        suffix=deploy.checked_id(args.batch_id)
        base,candidate,bad=("p7-a13-"+kind+"-"+suffix for kind in ("base","next","bad"))
        for name in (base,candidate):
            deploy.stage(root,PROJECT,PROJECT/"build/model-probes/bin/voice_engine",private,name)
        bad_config=json.loads(json.dumps(settings));bad_config["audio"]["frame_ms"]=0
        bad_path=args.output/"bad-config.yaml";save(bad_path,bad_config)
        deploy.stage(root,PROJECT,PROJECT/"build/model-probes/bin/voice_engine",bad_path,bad)
        report["input_hashes"]={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (args.wake_wav,args.query_wav)}
        wake,query=pcm(args.wake_wav),pcm(args.query_wav)
        with tempfile.TemporaryDirectory(prefix="ve-p7-a13-") as temporary:
            with PrivatePulse(Path(temporary)) as server:
                fifo=Path(temporary)/"input.fifo"
                server.command(["pactl","load-module","module-pipe-source","source_name=ve_deploy_source",
                                "file="+str(fifo),"format=s16le","rate=16000","channels=1","channel_map=mono"],timeout=5)
                feeder=Feeder(fifo)
                deploy.activate(root,base)
                for index in range(3):
                    if index==1:deploy.activate(root,candidate)
                    result=cycle(root,"p7-a13-"+suffix+"-cold-"+str(index+1),server,feeder,wake,query,args.output)
                    report["cycles"].append(result);save(args.output/"review.json",report)
                before=(root/"current").resolve().name
                release=root/"releases"/bad
                (release/"models").rename(release/"models-held")
                try:
                    try:deploy.activate(root,bad)
                    except ValueError as error:report["missing_assets_rejected"]=str(error)
                    else:raise AssertionError("missing assets were accepted")
                finally:(release/"models-held").rename(release/"models")
                try:deploy.activate(root,bad)
                except ValueError as error:report["bad_config_rejected"]=str(error)
                else:raise AssertionError("bad configuration was accepted")
                assert (root/"current").resolve().name==before==candidate
                report["bad_candidate_left_current_unchanged"]=True
                report["rollback"]=deploy.rollback(root)
                assert (root/"current").resolve().name==base
                report["cycles"].append(cycle(root,"p7-a13-"+suffix+"-rollback",server,feeder,wake,query,args.output))
                feeder.close();feeder=None
        assert sum(len(c["rounds"]) for c in report["cycles"])==20
        report["status"]="passed"
        report["total_completed_turns"]=20
        report["configuration_differences"]="private audio source/sink/profile and deployment-relative paths only"
        save(args.output/"review.json",report)
        print("A13_PASSED 3 cold starts x 5; rollback x 5; total 20",flush=True)
    except BaseException as error:
        report["status"]="failed";report["error"]=repr(error)
        save(args.output/"review.json",report)
        raise
    finally:
        if feeder:
            try:feeder.close()
            except RuntimeError as cleanup:
                report["cleanup_observation"]=str(cleanup)
                save(args.output/"review.json",report)
if __name__=="__main__":main()
