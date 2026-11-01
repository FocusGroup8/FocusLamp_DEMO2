$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$zig = Join-Path $root 'tools/focus-ring/test-deps/ziglang/zig.exe'
$outDir = Join-Path $PSScriptRoot '.task-guide-host'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $outDir 'zig-cache'
& $zig cc (Join-Path $PSScriptRoot 'test_cjk_font.c') `
  (Join-Path $root 'head/main/display/cjk_font_16.c') `
  '-I' (Join-Path $root 'head/main/display') `
  -o (Join-Path $outDir 'test_cjk_font.exe') -std=c11 -Wall -Wextra -Werror
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $outDir 'test_cjk_font.exe')
exit $LASTEXITCODE
