param([ValidateSet('head','controller')][string]$Board)
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$env:ESP_IDF_VERSION='5.5'
$env:IDF_PATH='C:/esp/v5.5.4/esp-idf'
$env:IDF_TOOLS_PATH='C:/Espressif/tools'
$env:IDF_PYTHON_ENV_PATH='C:/Espressif/python_env/idf5.5_py3.12_env'
$env:PATH='C:/Espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin;C:/Espressif/tools/cmake/3.30.2/bin;C:/Espressif/tools/ninja/1.12.1;C:/Program Files/Git/cmd;'+$env:PATH
Set-Location (Join-Path $root $Board)
$buildArgs=@('build')
$env:ESP_ROM_ELF_DIR='C:/Espressif/tools/esp-rom-elfs/20241011'
$env:IDF_COMPONENT_MANAGER='1'
if($Board -eq 'head') {
  $env:FOCUS_HEAD_LOCKED_BUILD='1'
  $env:PYTHONPATH=Join-Path $PSScriptRoot 'build-context'
}

& 'C:/Espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe' 'C:/esp/v5.5.4/esp-idf/tools/idf.py' @buildArgs
exit $LASTEXITCODE

