from pathlib import Path
import shutil, hashlib, json
root=Path(__file__).resolve().parents[2]
stage=root/'tools/focus-ring/docker'
actual=Path('D:/Docker_demo2/docker')
backup=root/'version_snapshots/2026-10-30_before_focus_ring/docker'
files=['main-client/main.py','main-client/focus_metadata.py','algorithms/focus/core/providers/engage_detector/mantis.py']
manifest={}
for name in files:
    src=stage/name; dest=actual/name
    if not src.is_file():raise SystemExit(f'Missing staged file {name}')
    old=backup/name
    if dest.exists() and not old.exists():old.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(dest,old)
    # Atomic per-file replacement, original retained in workspace backup.
    temp=dest.with_name(dest.name+'.focus-ring.tmp');shutil.copy2(src,temp);temp.replace(dest)
    manifest[name]=hashlib.sha256(dest.read_bytes()).hexdigest()
(root/'tools/focus-ring/installed-sources.json').write_text(json.dumps(manifest,indent=2))
print('Installed 3 Docker source files; originals backed up. Running containers unchanged.')
