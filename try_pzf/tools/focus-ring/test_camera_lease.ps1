$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $root
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $PSScriptRoot 'zig-cache-host'
& (Join-Path $PSScriptRoot 'test-deps/ziglang/zig.exe') cc 'tools/focus-ring/test_camera_lease.c' `
  -I 'tools/focus-ring/ws-stubs' -I 'head/components/websocket_manager/include' `
  -o 'tools/focus-ring/test_camera_lease.exe' -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& 'tools/focus-ring/test_camera_lease.exe'
exit $LASTEXITCODE
