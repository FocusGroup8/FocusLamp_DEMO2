from pathlib import Path
p=Path('tools/focus-ring/Flash-FocusDataFix.ps1')
s=Path('tools/focus-ring/Flash-FocusRing.ps1').read_text(encoding='utf-8-sig').replace("Write-Host 'Both applications written. Next run Update-Docker.ps1 on the algorithm computer.'", "Write-Host 'Both applications written. Local Docker transport fix is already deployed on this computer.'\nWrite-Host 'Reconnect in the webpage, restore control, start camera, then enter focus once.'")
p.write_text(s,encoding='utf8')
