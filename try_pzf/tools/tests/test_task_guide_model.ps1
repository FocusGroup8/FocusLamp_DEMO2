$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$zig = Join-Path $root 'tools/focus-ring/test-deps/ziglang/zig.exe'
$outDir = Join-Path $PSScriptRoot '.task-guide-host'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $outDir 'zig-cache'
& $zig c++ (Join-Path $PSScriptRoot 'test_task_guide_model.cpp') `
  -o (Join-Path $outDir 'test_task_guide_model.exe') -std=c++17 -Wall -Wextra -Werror -UNDEBUG
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $outDir 'test_task_guide_model.exe')
exit $LASTEXITCODE
