from pathlib import Path
import json,shutil,zipfile
root=Path.cwd();work=root/'tools/focus-ring';out=work/'release'
for name in ['Flash-ControllerFix.ps1','STABILITY_FIX.md']:shutil.copy2(work/name,out/name)
patch=work/'stability-fix';patch.mkdir(exist_ok=True)
for name in ['controller.bin','manifest.json','Flash-ControllerFix.ps1','STABILITY_FIX.md']:shutil.copy2(out/name,patch/name)
package=root/'基座专注稳定性修补_20261030.zip'
with zipfile.ZipFile(package,'w',zipfile.ZIP_DEFLATED) as z:
    for file in patch.iterdir():
        if file.is_file():z.write(file,file.name)
with zipfile.ZipFile(package) as z:assert z.testzip() is None
v=work/'verification.json';data=json.loads(v.read_text());data['tests']['actual_HTTP_contract']='pass: compact max252/512 bytes, full665/1200, bad bodies400, old session409';data['hardware_reset_root_cause']='unconfirmed: boot change observed; HTTP stack budget fixed, requires flash and retest';data['original_http_stack_bytes']=4096;data['fixed_http_stack_bytes']=8192;data['engagement_nested_own_stack_bytes_before']=2096;data['engagement_nested_own_stack_bytes_after']=1296;v.write_text(json.dumps(data,indent=2))
print('Controller-only package:',package.name,package.stat().st_size)
print('Ready to flash:',patch/'controller.bin')
