from pathlib import Path
p=Path('tools/focus-ring/build.ps1');s=p.read_text(encoding='utf8');s=s.replace("& 'C:/Espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe' 'C:/esp/v5.5.4/esp-idf/tools/idf.py' build", """$buildArgs=@('build')
if($Board -eq 'head') {
  # Existing locked dependencies are already installed. Avoid registry candidate
  # manifests requiring Kconfig options absent from the locked esp_video version.
  $env:IDF_COMPONENT_MANAGER='0'
  $existingComponents=(Get-ChildItem -LiteralPath (Join-Path $root 'head/managed_components') -Directory | ForEach-Object {$_.FullName -replace '\\\\','/'}) -join ';'
  $buildArgs=@('-D',('EXTRA_COMPONENT_DIRS='+$existingComponents),'build')
}
& 'C:/Espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe' 'C:/esp/v5.5.4/esp-idf/tools/idf.py' @buildArgs""");p.write_text(s,encoding='utf8')
