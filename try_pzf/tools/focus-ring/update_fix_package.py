from pathlib import Path
import json,shutil,hashlib,zipfile
root=Path.cwd();work=root/'tools/focus-ring'
size=(root/'controller/build/controller.bin').stat().st_size
p=work/'README.md';s=p.read_text(encoding='utf8').replace('当前未烧录实机；运行中的 Docker 容器仍保持原版本。','HTTP栈修补固件尚未烧录；本机Docker名称映射修复已部署。').replace('4,049,808字节',f'{size:,}字节').replace('6项元数据测试','7项元数据测试');p.write_text(s,encoding='utf8')
p=work/'package_release.py';s=p.read_text(encoding='utf-8-sig').replace("'focus_metadata':6","'focus_metadata':7").replace("'running_containers_switched':False","'running_containers_switched':True");p.write_text(s,encoding='utf8')
