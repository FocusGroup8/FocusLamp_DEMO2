$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $root
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'controller/build/host_toolchain/cache'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $root 'controller/build/task_guide_zig_cache'
$zig=Join-Path $root 'controller/build/host_toolchain/ziglang/zig.exe'
$json=Join-Path $root 'controller/managed_components/espressif__cjson/cJSON'
$includes=@('-Icontroller/tests/task_guide_host/include','-Icontroller/components/task_guide/include','-Icontroller/components/controller_modes/include','-Icontroller/components/common/include','-Icontroller/managed_components/espressif__mcp-c-sdk/include',('-I'+$json))
& $zig cc -std=c11 -Wall -Wextra -Werror -DNDEBUG @includes controller/tests/task_guide_component_test.c controller/components/task_guide/task_guide.c (Join-Path $json 'cJSON.c') -o controller/build/task_guide_component_test.exe
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& controller/build/task_guide_component_test.exe
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& $zig cc -std=c11 -Wall -Wextra -Werror -DNDEBUG controller/tests/task_guide_gate_test.c -o controller/build/task_guide_gate_test.exe
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& controller/build/task_guide_gate_test.exe
exit $LASTEXITCODE
