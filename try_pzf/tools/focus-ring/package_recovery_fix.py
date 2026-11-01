from pathlib import Path
import hashlib
import json
import shutil
import zipfile

root = Path(__file__).resolve().parents[2]
work = root / 'tools/focus-ring'
out = work / 'recovery-fix'
out.mkdir(exist_ok=True)
manifest = {}
for board, filename in [('head', 'mipi_dsi.bin'), ('controller', 'controller.bin')]:
    dest = out / (board+'.bin')
    shutil.copy2(root / board / 'build' / filename, dest)
    manifest[board] = {'offset': '0x10000', 'bytes': dest.stat().st_size,
                       'sha256': hashlib.sha256(dest.read_bytes()).hexdigest()}
manifest['docker'] = {}
for filename in ['main-client/main.py', 'main-client/focus_metadata.py',
                 'algorithms/focus/core/providers/engage_detector/mantis.py']:
    manifest['docker'][filename] = hashlib.sha256((work/'docker'/filename).read_bytes()).hexdigest()
(out/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf8')
shutil.copy2(work/'Flash-FocusRecovery.ps1', out/'Flash-FocusRecovery.ps1')
shutil.copy2(work/'RECOVERY_FIX.md', out/'README.md')
package = root/'专注数据链路修复_20261031.zip'
with zipfile.ZipFile(package, 'w', zipfile.ZIP_DEFLATED) as archive:
    for filename in ['head.bin', 'controller.bin', 'manifest.json', 'Flash-FocusRecovery.ps1', 'README.md']:
        archive.write(out/filename, filename)
with zipfile.ZipFile(package) as archive:
    assert archive.testzip() is None
verification = {'firmware': manifest, 'tests': {
    'actual_camera_websocket_heartbeat': 'pass: actual C frame sender and admission',
    'actual_C_clock': 'pass: 5/2/1.5fps, genuine stale and invalid pause',
    'actual_controller_modes_API': 'pass', 'actual_HTTP_contract': 'pass',
    'focus_metadata': 11, 'existing_Docker_regressions': 61},
    'flashed': False, 'running_containers_switched': True,
    'hardware_fps_psram_and_two_screen_latency': 'pending',
    'camera_first_disconnect_cause': 'not established',
    'recovery_hardware_verified_after_flash': False}
(work/'verification.json').write_text(json.dumps(verification, indent=2), encoding='utf8')
print(package, package.stat().st_size)
print(json.dumps(manifest, indent=2))
