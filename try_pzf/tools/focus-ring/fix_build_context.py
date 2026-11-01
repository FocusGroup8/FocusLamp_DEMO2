from pathlib import Path
import json
p=Path('head/build/config/sdkconfig.json')
s=json.loads(p.read_text());s['ESP_VIDEO_USE_CUSTOMIZED_ESP_H264_VERSION']=False
p.write_text(json.dumps(s))
p=Path('tools/focus-ring/build.ps1');s=p.read_text(encoding='utf8');a=s.index("$buildArgs=@('build')");b=s.index("& 'C:/Espressif/python_env",a);s=s[:a]+'''$buildArgs=@('build')
$env:ESP_ROM_ELF_DIR='C:/Espressif/tools/esp-rom-elfs/20241011'
$env:IDF_COMPONENT_MANAGER='1'
if($Board -eq 'head') {
  # Supply a resolver-only default for a newer registry candidate's option.
  # The locked esp_video 2.3.0 has no such Kconfig symbol. Runtime sdkconfig is unchanged.
  $resolverConfig=Join-Path $root 'head/build/config/sdkconfig.json'
  & python -c "import json;from pathlib import Path;p=Path(r'$resolverConfig');d=json.loads(p.read_text());d['ESP_VIDEO_USE_CUSTOMIZED_ESP_H264_VERSION']=False;p.write_text(json.dumps(d))"
}
'''+s[b:];p.write_text(s,encoding='utf8')
