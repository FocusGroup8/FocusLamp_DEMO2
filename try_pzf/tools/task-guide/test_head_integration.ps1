param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $root
$zig=Join-Path $root 'tools/focus-ring/test-deps/ziglang/zig.exe'
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'tools/focus-ring/zig-cache-host'
$json='C:/esp/v5.5.4/esp-idf/components/json/cJSON'
$output=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $PSScriptRoot 'build'}
$previews=if($OutputDirectory){Join-Path $output 'previews'}else{Join-Path $PSScriptRoot 'previews'}
New-Item -ItemType Directory -Path $output -Force | Out-Null
New-Item -ItemType Directory -Path $previews -Force | Out-Null
& $zig cc -std=c11 -O1 "-I$json" -Itools/task-guide/host-stubs -c "$json/cJSON.c" -o "$output/cJSON.o"
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& $zig cc -std=c11 -O1 -Wall -Wextra -Werror -Itools/task-guide/host-stubs -c head/main/display/cjk_font_16.c -o "$output/font.o"
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& $zig c++ -std=c++17 -O1 -UNDEBUG -Wall -Wextra -Werror "-I$json" -Itools/task-guide/host-stubs -Itools/focus-ring/test-stubs -Ihead/main/display -Ihead/main/algo tools/task-guide/test_head_integration.cpp head/main/display/task_guide.cpp head/main/display/task_guide_view.cpp head/main/display/celebration_renderer.cpp head/main/display/fola_eyes.cpp head/main/display/fola_gfx.cpp "$output/cJSON.o" "$output/font.o" -o "$output/test_head_integration.exe"
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& "$output/test_head_integration.exe" $previews
exit $LASTEXITCODE
