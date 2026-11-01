$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $root
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $PSScriptRoot 'zig-cache-host'
& (Join-Path $PSScriptRoot 'test-deps/ziglang/zig.exe') cc 'tools/focus-ring/test_transport_age.c' `
  -o 'tools/focus-ring/test_transport_age.exe' -std=c11 -Wall -Wextra -Werror
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& 'tools/focus-ring/test_transport_age.exe'
exit $LASTEXITCODE
