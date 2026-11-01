param(
 [Parameter(Mandatory=$true)][ValidatePattern('^COM[0-9]+$')][string]$HeadPort,
 [Parameter(Mandatory=$true)][ValidatePattern('^COM[0-9]+$')][string]$ControllerPort
)
$ErrorActionPreference='Stop'
if($HeadPort -eq $ControllerPort){throw 'Head and controller must use different ports.'}
$pythonExe='C:/Espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe'
if(!(Test-Path -LiteralPath $pythonExe)){throw 'ESP-IDF Python not found. Install the existing ESP-IDF environment first.'}
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
foreach($board in @('head','controller')) {
 $appFile=Join-Path $PSScriptRoot ($board+'.bin')
 $expected=$manifest.$board.sha256
 if((Get-FileHash -Algorithm SHA256 -LiteralPath $appFile).Hash.ToLower() -ne $expected){throw "Checksum mismatch: $board"}
 $targetPort=if($board -eq 'head'){$HeadPort}else{$ControllerPort}
 Write-Host "Writing $board application on $targetPort (0x10000 only). Close its serial monitor first."
 & $pythonExe -m esptool --chip esp32p4 --port $targetPort --baud 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB 0x10000 $appFile
 if($LASTEXITCODE -ne 0){throw "Flash failed: $board. Docker has not been changed."}
}
Write-Host 'Both applications written. Next run Update-Docker.ps1 on the algorithm computer.'
