#!/usr/bin/env python3
"""Opt-in regression for user-service stop. Uses frozen engine --mock, no audio/models."""
import argparse,json,os,subprocess,sys,time
from pathlib import Path
from deployment_runtime import deploy,systemctl,wait_for
from wake_runtime import save
PROJECT=Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root",type=Path,default=deploy.DEFAULT_ROOT)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--run-id",required=True)
    args=parser.parse_args()
    root=args.root.resolve()
    run_id=deploy.checked_id(args.run_id)
    args.output.mkdir(parents=True,exist_ok=False)
    report={"status":"running","scope":"single mock systemd stop regression; no headset/model inference"}
    save(args.output/"review.json",report)
    unit_name="rk3588-voice-engine-"+run_id+".service"
    unit_dir=Path("/run/user")/str(os.getuid())/"systemd/user"
    unit_dir.mkdir(parents=True,exist_ok=True)
    unit=unit_dir/unit_name
    assert not unit.exists()
    template=(PROJECT/"packaging/rk3588-voice-engine.service.in").read_text()
    assert "KillMode=mixed" in template
    text=template.replace("@ROOT@",str(root)).replace("@PYTHON@",sys.executable)
    # Preserve all shipped properties; change only this test's engine mode and run identity.
    text=text.replace(' serve\n',' serve --mock --run-id '+run_id+'\n')
    unit.write_text(text)
    (args.output/"test-unit.service").write_text(text)
    try:
        subprocess.run(["systemd-analyze","--user","verify",str(unit)],check=True,capture_output=True)
        systemctl("daemon-reload")
        systemctl("start",unit_name)
        run=root/"runs"/run_id
        def is_ready():
            if (run/"exit.json").exists():
                raise RuntimeError("mock service exited before ready: "+(run/"exit.json").read_text())
            p=run/"console.log"
            return p.exists() and "MOCK_READY" in p.read_text(errors="replace")
        wait_for(is_ready,180)
        state=json.loads((run/"launcher.json").read_text())
        report["run"]=state
        report["before_stop"]=systemctl("show",unit_name,"-p","KillMode","-p","MainPID","-p","NRestarts")
        assert "KillMode=mixed" in report["before_stop"]
        systemctl("stop",unit_name)
        wait_for(lambda:(run/"exit.json").exists(),15)
        outcome=json.loads((run/"exit.json").read_text())
        report["exit"]=outcome
        report["after_stop"]=systemctl("show",unit_name,"-p","ActiveState","-p","SubState","-p","Result",
                                              "-p","MainPID","-p","ExecMainStatus","-p","NRestarts")
        assert outcome["exit_code"]==0,outcome
        assert "Result=success" in report["after_stop"]
        assert "ActiveState=inactive" in report["after_stop"]
        assert "ExecMainStatus=0" in report["after_stop"] and "NRestarts=0" in report["after_stop"]
        assert not Path("/proc",str(state["supervisor_pid"])).exists()
        assert not Path("/proc",str(state["engine_pid"])).exists()
        report["status"]="passed"
        save(args.output/"review.json",report)
        print("SERVICE_STOP_MOCK_PASSED exit=0 Result=success no remaining child",flush=True)
    except BaseException as error:
        report.update(status="failed",error=repr(error))
        save(args.output/"review.json",report)
        raise
    finally:
        subprocess.run(["systemctl","--user","stop",unit_name],capture_output=True,timeout=25)
        unit.unlink()
        systemctl("daemon-reload")

if __name__=="__main__":main()
