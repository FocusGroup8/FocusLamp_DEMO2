param(
    [Parameter(Mandatory=$true)][ValidatePattern('^COM[0-9]+$')][string]$Port,
    [switch]$Rollback
)
$ErrorActionPreference='Stop'
$workspaceRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$pythonExe='C:/Espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe'
$appFile=if($Rollback){Join-Path $workspaceRoot 'version_snapshots/2026-10-30_before_optimized_firmware/mipi_dsi.bin'}else{Join-Path $workspaceRoot 'head/build/mipi_dsi.bin'}
if(!(Test-Path -LiteralPath $pythonExe)){throw 'ESP-IDF Python not found.'}
if(!(Test-Path -LiteralPath $appFile)){throw 'Head application binary not found.'}
$metadata=Get-Content -LiteralPath (Join-Path $workspaceRoot 'head/build/flasher_args.json') -Raw | ConvertFrom-Json
if($metadata.app.offset -ne '0x10000'){throw 'Head application offset changed. Refusing to flash.'}
Write-Host "Head board only: $Port. Close its serial monitor first."
Write-Host "Application: $appFile"
Write-Host 'Writes only the app at 0x10000; does not write NVS, partition table or LittleFS.'
& $pythonExe -m esptool --chip esp32p4 --port $Port --baud 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB 0x10000 $appFile
if($LASTEXITCODE -ne 0){throw "Head application flash failed (exit $LASTEXITCODE)."}
