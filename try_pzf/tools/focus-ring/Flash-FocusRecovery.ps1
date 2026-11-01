param(
 [Parameter(Mandatory=$true)][ValidatePattern('^COM[0-9]+$')][string]$HeadPort,
 [Parameter(Mandatory=$true)][ValidatePattern('^COM[0-9]+$')][string]$ControllerPort
)
$ErrorActionPreference='Stop'
if($HeadPort -eq $ControllerPort){throw 'Head and controller must use different ports.'}
$pythonExe='C:/Espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe'
if(!(Test-Path -LiteralPath $pythonExe)){throw 'Existing ESP-IDF Python environment not found.'}
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
# Verify both files before opening either serial port.
foreach($board in @('head','controller')) {
 $appFile=Join-Path $PSScriptRoot ($board+'.bin')
 if((Get-FileHash -Algorithm SHA256 -LiteralPath $appFile).Hash.ToLower() -ne $manifest.$board.sha256){throw "Checksum mismatch: $board"}
}
foreach($board in @('head','controller')) {
 $appFile=Join-Path $PSScriptRoot ($board+'.bin')
 $targetPort=if($board -eq 'head'){$HeadPort}else{$ControllerPort}
 Write-Host "Writing $board application on $targetPort (0x10000 only). Close its serial monitor first."
 & $pythonExe -m esptool --chip esp32p4 --port $targetPort --baud 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB 0x10000 $appFile
 if($LASTEXITCODE -ne 0){throw "Flash failed: $board. No further boards will be written."}
}
Write-Host 'Both applications written; NVS, partition table, speech models and LittleFS were not erased.'
Write-Host 'Reconnect in the webpage, restore control, start camera, then start a new focus session.'
Write-Host 'This computer already has the accompanying Docker metadata update. See README for checks.'
