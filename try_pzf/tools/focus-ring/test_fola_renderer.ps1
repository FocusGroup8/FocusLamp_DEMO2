param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $root
if(!$OutputDirectory){$OutputDirectory=Join-Path $PSScriptRoot 'fola-focus-20261104'}
$output=[System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$previews=Join-Path $output 'previews'
New-Item -ItemType Directory -Path $previews -Force | Out-Null
$zig=Join-Path $PSScriptRoot 'test-deps/ziglang/zig.exe'
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $PSScriptRoot 'zig-cache-host'
$json='C:/esp/v5.5.4/esp-idf/components/json/cJSON'
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -c head/main/display/cjk_font_16.c -o "$output/font.o"
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& $zig c++ -std=c++17 -O1 -UNDEBUG -Wall -Wextra -Werror "-I$json" -Itools/task-guide/host-stubs -Itools/focus-ring/test-stubs -Ihead/main/display -Ihead/main/algo tools/focus-ring/test_renderer.cpp head/main/display/focus_ring_renderer.cpp head/main/display/fola_eyes.cpp head/main/display/fola_gfx.cpp "$output/font.o" -o "$output/test_renderer.exe"
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& "$output/test_renderer.exe" $previews
exit $LASTEXITCODE
