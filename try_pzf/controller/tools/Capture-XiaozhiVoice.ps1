param(
    [Parameter(Mandatory = $true)][string]$ControllerIp,
    [string]$PythonExe = 'C:\Espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe',
    [string]$Ffmpeg = '',
    [switch]$Single,
    [ValidateRange(1, 10)][int]$StartAt = 1
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $PythonExe)) {
    throw 'Python not found. Pass -PythonExe with an installed Python 3.10+ executable.'
}
$captureTool = Join-Path $PSScriptRoot 'capture_xiaozhi_tts.py'
$captureDeps = Join-Path $PSScriptRoot '.tts_capture_deps'
if (-not $Ffmpeg -and -not (Get-Command ffmpeg -ErrorAction SilentlyContinue) -and
    -not (Test-Path -LiteralPath (Join-Path $captureDeps 'imageio_ffmpeg\binaries'))) {
    Write-Host 'Preparing private FFmpeg dependency (first run only)...'
    & $PythonExe -m pip install --disable-pip-version-check --target $captureDeps 'imageio-ffmpeg==0.6.0'
    if ($LASTEXITCODE -ne 0) { throw 'Preparation failed. Supply -Ffmpeg with an existing ffmpeg.exe.' }
}
$captureArgs = @($captureTool, '--host', $ControllerIp)
if (-not $Single) { $captureArgs += @('--batch', '--start-at', [string]$StartAt) }
elseif ($StartAt -ne 1) { throw '-StartAt requires batch mode; do not combine it with -Single.' }
if ($Ffmpeg) { $captureArgs += @('--ffmpeg', $Ffmpeg) }
& $PythonExe @captureArgs
if ($LASTEXITCODE -ne 0) { throw "Capture exited with code $LASTEXITCODE." }
