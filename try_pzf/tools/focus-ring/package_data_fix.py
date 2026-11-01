from pathlib import Path
import shutil,zipfile,json
root=Path.cwd();work=root/'tools/focus-ring';out=work/'data-wait-fix';out.mkdir(exist_ok=True)
for name in ['head.bin','controller.bin','manifest.json']:shutil.copy2(work/'release'/name,out/name)
for name in ['Flash-FocusDataFix.ps1','DATA_WAIT_FIX.md']:shutil.copy2(work/name,out/name)
package=root/'专注数据等待修复_20261030.zip'
with zipfile.ZipFile(package,'w',zipfile.ZIP_DEFLATED) as z:
 for file in out.iterdir():
  if file.is_file():z.write(file,file.name)
with zipfile.ZipFile(package) as z:assert z.testzip() is None
print(package,package.stat().st_size)
