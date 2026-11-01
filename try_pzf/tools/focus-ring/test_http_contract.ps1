$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $root
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $PSScriptRoot 'zig-cache-host'
$compiler=Join-Path $PSScriptRoot 'test-deps/ziglang/zig.exe'
$cjson=Join-Path $root 'controller/managed_components/espressif__cjson/cJSON'
& $compiler cc 'tools/focus-ring/test_http_contract.c' 'controller/main/mode_http_api.c' `
  'controller/components/controller_modes/src/controller_modes.c' (Join-Path $cjson 'cJSON.c') `
  '-Itools/focus-ring/http-stubs' '-Itools/focus-ring/controller-stubs' `
  '-Icontroller/components/controller_modes/include' ('-I'+$cjson) `
  -o 'tools/focus-ring/test_http_contract.exe' -std=c11 -Wall -Wextra -UNDEBUG -Wno-unused-parameter -Wno-unused-variable
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& 'tools/focus-ring/test_http_contract.exe'
exit $LASTEXITCODE
