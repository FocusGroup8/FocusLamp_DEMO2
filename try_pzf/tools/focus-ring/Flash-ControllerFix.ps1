param([ValidatePattern('^COM[0-9]+$')][string]$Port='COM6')
$ErrorActionPreference='Stop'
$pythonExe='C:/Espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe'
$appFile=Join-Path $PSScriptRoot 'controller.bin'
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
if((Get-FileHash -LiteralPath $appFile -Algorithm SHA256).Hash.ToLower() -ne $manifest.controller.sha256){throw 'Firmware checksum mismatch.'}
Write-Host "Controller only: $Port. Close the COM6 serial monitor first."
Write-Host 'Writes only 0x10000 app; leaves NVS, partition table and speech models intact.'
& $pythonExe -m esptool --chip esp32p4 --port $Port --baud 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB 0x10000 $appFile
if($LASTEXITCODE -ne 0){throw 'Controller flash failed. Do not continue until the error is resolved.'}
Write-Host 'Done. Reconnect on the webpage, restore control explicitly, then start focus once.'
