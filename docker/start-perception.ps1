param([Parameter(Mandatory = $true)][string]$HeadIp)
$ErrorActionPreference = 'Stop'
$parsedIp = $null
if (-not [Net.IPAddress]::TryParse($HeadIp, [ref]$parsedIp) -or $parsedIp.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) { throw 'HeadIp must be the current verified IPv4 address from the head log.' }
Set-Location -LiteralPath $PSScriptRoot
$env:ESP32P4_IP = $HeadIp
$env:ENABLED_ALGORITHMS = 'gesture_mantis,emotion,focus,phone_use'
$env:GESTURE_SAMPLE_EVERY = '1'
# Explicit service list: never starts paid VLM services or the USB camera source.
& docker compose build main-client emotion-detector focus-detector
if ($LASTEXITCODE -ne 0) { throw "Docker build failed ($LASTEXITCODE). The original gesture image was not rebuilt." }
& docker compose build phone-detector
if ($LASTEXITCODE -ne 0) { throw "Local phone detector build failed ($LASTEXITCODE). No cloud service was started." }
& docker compose up -d --no-deps --no-build main-client gesture-mantis-detector emotion-detector focus-detector phone-detector
if ($LASTEXITCODE -ne 0) { throw "Docker startup failed ($LASTEXITCODE). Existing firmware was not changed." }
& docker compose ps main-client gesture-mantis-detector emotion-detector focus-detector phone-detector
