param([ValidateSet('head','controller')][string]$Board,[switch]$SampledDemo,
      [switch]$CameraBaseline,[ValidateSet(5,8)][int]$CameraHighFps=5,
      [switch]$CameraFixedDiag,[string]$BuildDirectory,
      [ValidateSet(0,15000)][int]$CameraPreflightWaitUs=0,[switch]$CameraTcpTrace)
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$env:ESP_IDF_VERSION='5.5'
$env:IDF_PATH='C:/esp/v5.5.4/esp-idf'
$env:IDF_TOOLS_PATH='C:/Espressif/tools'
$env:IDF_PYTHON_ENV_PATH='C:/Espressif/python_env/idf5.5_py3.12_env'
$env:PATH='C:/Espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin;C:/Espressif/tools/cmake/3.30.2/bin;C:/Espressif/tools/ninja/1.12.1;C:/Program Files/Git/cmd;'+$env:PATH
Set-Location (Join-Path $root $Board)
$profile=if($SampledDemo){'ON'}else{'OFF'}
$buildArgs=@('-D',"FOCUS_SAMPLED_DEMO=$profile",'build')
$env:ESP_ROM_ELF_DIR='C:/Espressif/tools/esp-rom-elfs/20241011'
$env:IDF_COMPONENT_MANAGER='1'
if($Board -eq 'head') {
  $preflight=if($CameraBaseline){'OFF'}else{'ON'}
  $fixedDiag=if($CameraFixedDiag){'ON'}else{'OFF'}
  $trace=if($CameraTcpTrace){'ON'}else{'OFF'}
  $buildArgs=@('-D',"FOCUS_SAMPLED_DEMO=$profile",'-D',"CAMERA_PREFLIGHT_ENABLED=$preflight",
               '-D',"CAMERA_PREFLIGHT_WAIT_US=$CameraPreflightWaitUs",'-D',"CAMERA_TCP_TRACE=$trace",
               '-D',"CAMERA_TRIAL_HIGH_FPS=$CameraHighFps",'-D',"CAMERA_FIXED_DIAG=$fixedDiag",'build')
  $env:FOCUS_HEAD_LOCKED_BUILD='1'
  $env:PYTHONPATH=Join-Path $PSScriptRoot 'build-context'
}
if($BuildDirectory) {
  $isolatedBuild=[System.IO.Path]::GetFullPath($BuildDirectory)
  New-Item -ItemType Directory -Path $isolatedBuild -Force | Out-Null
  $isolatedConfig=Join-Path $isolatedBuild 'sdkconfig'
  if(!(Test-Path -LiteralPath $isolatedConfig)) {
    Copy-Item -LiteralPath (Join-Path (Get-Location) 'sdkconfig') -Destination $isolatedConfig
  }
  $buildArgs=@('-B',$isolatedBuild,'-D',"SDKCONFIG=$isolatedConfig")+$buildArgs
}

& 'C:/Espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe' 'C:/esp/v5.5.4/esp-idf/tools/idf.py' @buildArgs
exit $LASTEXITCODE

