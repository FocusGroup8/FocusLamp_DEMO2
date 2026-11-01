from pathlib import Path
p=Path('tools/focus-ring/Update-Docker.ps1');s=p.read_text(encoding='utf8');marker='Push-Location $resolvedRoot';s=s.replace(marker,'''New-Item -ItemType Directory -Path $backup -Force | Out-Null
$rollbackStamp=Get-Date -Format 'yyyyMMddHHmmss'
$mainImage=& docker inspect --format '{{.Image}}' main-client
if($LASTEXITCODE -ne 0){throw 'Running main-client image could not be saved.'}
$focusImage=& docker inspect --format '{{.Image}}' focus-detector
if($LASTEXITCODE -ne 0){throw 'Running focus-detector image could not be saved.'}
$mainTag="focus-main-client:before-ring-$rollbackStamp"
$focusTag="focus-detector:before-ring-$rollbackStamp"
& docker tag $mainImage $mainTag
if($LASTEXITCODE -ne 0){throw 'Image backup failed.'}
& docker tag $focusImage $focusTag
if($LASTEXITCODE -ne 0){throw 'Image backup failed.'}
@"
services:
  main-client:
    image: $mainTag
  focus-detector:
    image: $focusTag
"@ | Set-Content -LiteralPath (Join-Path $backup 'rollback-images.yml') -Encoding UTF8
Push-Location $resolvedRoot''');p.write_text(s,encoding='utf8')
