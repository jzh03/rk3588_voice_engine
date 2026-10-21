#!/usr/bin/env python3
"""Install the user-requested, pinned Linux aarch64 RKNN/RKLLM pair.
No network, kernel, boot configuration, model, or Git changes.
Use --check as normal user; --install/--rollback require sudo.
"""
import argparse, hashlib, json, os, pathlib, shutil, stat, subprocess, tempfile
PLAN = {'driver': '0.9.8', 'kernel': '6.1.99-rockchip-rk3588', 'backup': '/var/backups/rk3588-npu-runtimes-20261020', 'entries': [{'source': '/home/orangepi/projects/rk3588_voice_engine/deps/rknn-2.3.2/lib/librknnrt.so', 'target': '/usr/lib/librknnrt.so', 'sha256': 'd31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8', 'mode': 420, 'before_sha256': '73993ed4b440460825f21611731564503cc1d5a0c123746477da6cd574f34885'}, {'source': '/home/orangepi/projects/rk3588_voice_engine/deps/rknn-2.3.2/include/rknn_api.h', 'target': '/usr/include/rknn_api.h', 'sha256': 'c48e11a6f41b451a5fd1e4ad774ea60252d3d94f78bee9b21ea3d21b21deba9a', 'mode': 420, 'before_sha256': None}, {'source': '/home/orangepi/projects/rk3588_voice_engine/deps/rknn-2.3.2/include/rknn_custom_op.h', 'target': '/usr/include/rknn_custom_op.h', 'sha256': 'af5983da0ca244ca31dc3162aa683322b0285531196c7a770f29cd2e3b8ccaa6', 'mode': 420, 'before_sha256': None}, {'source': '/home/orangepi/projects/rk3588_voice_engine/deps/rknn-2.3.2/include/rknn_matmul_api.h', 'target': '/usr/include/rknn_matmul_api.h', 'sha256': 'aaadd9a7118de30a06b222996b6731db77095d00f5931a7a98c83a67f14a4d42', 'mode': 420, 'before_sha256': None}, {'source': '/home/orangepi/projects/rk3588_voice_engine/deps/rkllm-1.3.0/lib/librkllmrt.so', 'target': '/usr/lib/librkllmrt.so', 'sha256': '6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6', 'mode': 420, 'before_sha256': '3cef353105c3bfd31f99c4963fce8498d2fac633d845633c904f523b7c3bcd0a'}, {'source': '/home/orangepi/projects/rk3588_voice_engine/deps/rkllm-1.3.0/include/rkllm.h', 'target': '/usr/include/rkllm.h', 'sha256': '80596a578f7f8e70df6eda1c2cbead3bfced14623a190258f2bd009a3d1f72cf', 'mode': 420, 'before_sha256': 'ecf2949b73b89038a16af708a2f36810176b92100e995a31cb8fbc113eb254a4'}]}
def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None
def require(ok, message):
    if not ok: raise RuntimeError(message)
def atomic_copy(src, dst, mode, uid=0, gid=0):
    fd, name = tempfile.mkstemp(prefix='.'+dst.name+'.npu-',dir=dst.parent)
    try:
        with os.fdopen(fd,'wb') as f:
            f.write(src.read_bytes()); f.flush(); os.fsync(f.fileno())
        os.chmod(name,mode); os.chown(name,uid,gid); os.replace(name,dst)
    finally:
        if os.path.exists(name): os.unlink(name)
def precheck():
    require(pathlib.Path('/sys/module/rknpu/version').read_text().strip()==PLAN['driver'],'Driver version changed')
    require(subprocess.check_output(['uname','-r'],text=True).strip()==PLAN['kernel'],'Kernel changed')
    for e in PLAN['entries']:
        src,dst=pathlib.Path(e['source']),pathlib.Path(e['target'])
        require(not src.is_symlink() and src.is_file(),'Invalid source: '+str(src))
        require(digest(src)==e['sha256'],'Source hash mismatch: '+str(src))
        require(not dst.is_symlink(),'Unexpected target symlink: '+str(dst))
        require(digest(dst)==e['before_sha256'],'System file changed: '+str(dst))
def restore(backup, records):
    for e in records:
        dst=pathlib.Path(e['target'])
        if e['before_sha256'] is None:
            if dst.exists(): dst.unlink()
        else:
            src=backup/dst.name
            require(digest(src)==e['before_sha256'],'Backup hash mismatch: '+str(src))
            atomic_copy(src,dst,e['old_mode'],e['old_uid'],e['old_gid'])
    subprocess.run(['/sbin/ldconfig'],check=True)
    for e in records:
        require(digest(pathlib.Path(e['target']))==e['before_sha256'],'Restore hash mismatch')
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('action',choices=['--check','--install','--rollback'],nargs='?',default='--check')
# Flags are parsed explicitly to keep the command convenient.
import sys
action=sys.argv[1] if len(sys.argv)==2 else '--check'
require(action in ['--check','--install','--rollback'],'Use --check, --install or --rollback')
backup=pathlib.Path(PLAN['backup'])
if action=='--check':
    precheck(); print('PRECHECK_OK; installation requires sudo'); sys.exit(0)
require(os.geteuid()==0,'Run with sudo; do not send passwords to chat')
if action=='--rollback':
    records=json.loads((backup/'backup.json').read_text())
    for e in records:
        require(digest(pathlib.Path(e['target'])) in [e['sha256'],e['before_sha256']],'System file changed since install')
    restore(backup,records)
    (backup/'rollback-result.json').write_text(json.dumps({'status':'rolled_back'},indent=2)+'\n')
    print('ROLLBACK_OK'); sys.exit(0)
precheck()
busy=subprocess.run(['fuser','/dev/dri/renderD129'],capture_output=True,text=True)
require(busy.returncode==1,'NPU in use or fuser failed: '+busy.stdout+busy.stderr)
require(not backup.exists(),'Backup path exists; inspect previous result before retrying')
backup.mkdir(mode=0o755)
records=[]
for e in PLAN['entries']:
    e=dict(e); dst=pathlib.Path(e['target'])
    if dst.exists():
        info=dst.stat()
        e.update(old_mode=stat.S_IMODE(info.st_mode),old_uid=info.st_uid,old_gid=info.st_gid)
        shutil.copy2(dst,backup/dst.name)
        require(digest(backup/dst.name)==e['before_sha256'],'Backup hash mismatch')
    records.append(e)
(backup/'backup.json').write_text(json.dumps(records,indent=2)+'\n')
try:
    for e in records:
        atomic_copy(pathlib.Path(e['source']),pathlib.Path(e['target']),e['mode'])
    subprocess.run(['/sbin/ldconfig'],check=True)
    for e in records:
        require(digest(pathlib.Path(e['target']))==e['sha256'],'Installed hash mismatch')
except Exception:
    restore(backup,records)
    (backup/'install-result.json').write_text(json.dumps({'status':'failed_and_rolled_back'},indent=2)+'\n')
    raise
(backup/'install-result.json').write_text(json.dumps({'status':'installed','driver':PLAN['driver'],'rknn':'2.3.2','rkllm':'1.3.0','entries':records},indent=2)+'\n')
print('INSTALL_OK RKNPU=0.9.8 RKNN=2.3.2 RKLLM=1.3.0 backup='+str(backup))
