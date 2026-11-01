param([ValidatePattern('^COM[0-9]+$')][string]$HeadPort='COM4')
$ErrorActionPreference='Stop'
$pythonExe='C:/Espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe'
$appFile=Join-Path $PSScriptRoot 'head.bin'
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw|ConvertFrom-Json
if((Get-FileHash -LiteralPath $appFile -Algorithm SHA256).Hash.ToLower() -ne $manifest.head.sha256){throw 'head.bin checksum mismatch'}
Write-Host "HEAD ONLY on $HeadPort. Confirm this is the head and close its serial monitor."
& $pythonExe -m esptool --chip esp32p4 --port $HeadPort --baud 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB 0x10000 $appFile
if($LASTEXITCODE -ne 0){throw 'Head flash failed'}
Write-Host 'Head application updated. Controller/NVS/models/partitions unchanged. Reconnect camera and verify latency.'
