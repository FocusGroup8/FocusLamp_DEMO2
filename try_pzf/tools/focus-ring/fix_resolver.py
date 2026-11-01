from pathlib import Path
p=Path('tools/focus-ring/build.ps1');s=p.read_text(encoding='utf8');a=s.index("if($Board -eq 'head')");b=s.index("\n& 'C:/Espressif/python_env",a);s=s[:a]+'''if($Board -eq 'head') {
  $env:FOCUS_HEAD_LOCKED_BUILD='1'
  $env:PYTHONPATH=Join-Path $PSScriptRoot 'build-context'
}
'''+s[b:];p.write_text(s,encoding='utf8')
