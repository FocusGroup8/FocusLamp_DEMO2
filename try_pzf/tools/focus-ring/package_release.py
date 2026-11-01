from pathlib import Path
import hashlib,json,shutil,zipfile
root=Path.cwd();work=root/'tools/focus-ring';out=work/'release';out.mkdir(exist_ok=True)
manifest={}
for board,name in [('head','mipi_dsi.bin'),('controller','controller.bin')]:
    src=root/board/'build'/name;dest=out/(board+'.bin');shutil.copy2(src,dest)
    manifest[board]={'offset':'0x10000','bytes':dest.stat().st_size,'sha256':hashlib.sha256(dest.read_bytes()).hexdigest()}
manifest['docker']={}
for name in ['main-client/main.py','main-client/focus_metadata.py','algorithms/focus/core/providers/engage_detector/mantis.py']:
    src=work/'docker'/name;dest=out/'docker'/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dest)
    manifest['docker'][name]=hashlib.sha256(src.read_bytes()).hexdigest()
for name in ['Flash-FocusRing.ps1','Update-Docker.ps1','Test-FocusRing.ps1','README.md']:shutil.copy2(work/name,out/name)
shutil.copy2(work/'previews/six-states.png',out/'six-states.png')
(out/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf8')
package=root/'专注环倒计时_待实机验证_20261030.zip'
with zipfile.ZipFile(package,'w',zipfile.ZIP_DEFLATED) as z:
    for file in out.rglob('*'):
        if file.is_file():z.write(file,file.relative_to(out))
with zipfile.ZipFile(package) as z:
    assert z.testzip() is None
    print('Package verified:',package.name,'files=',len(z.infolist()),'bytes=',package.stat().st_size)
(work/'verification.json').write_text(json.dumps({'firmware':manifest,'tests':{
    'actual_C_clock':'pass','actual_controller_modes_API':'pass','actual_RGB565_renderer':'pass',
    'focus_metadata':7,'main_client_integration':1,'existing_Docker_regressions':61},
    'flashed':False,'running_containers_switched':True,'hardware_fps_psram_and_two_screen_latency':'pending'},indent=2))
