param([string]$DockerRoot='D:/Docker_demo2/docker')
$ErrorActionPreference='Stop'
$resolvedRoot=(Resolve-Path -LiteralPath $DockerRoot).Path
if(!(Test-Path -LiteralPath (Join-Path $resolvedRoot 'docker-compose.yml'))){throw 'Docker compose file not found.'}
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
$backup=Join-Path $resolvedRoot ('focus-ring-backup-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$items=@('main-client/main.py','main-client/focus_metadata.py','algorithms/focus/core/providers/engage_detector/mantis.py')
foreach($item in $items){
 $source=Join-Path (Join-Path $PSScriptRoot 'docker') $item
 $expected=$manifest.docker.PSObject.Properties[$item].Value
 if((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash.ToLower() -ne $expected){throw "Checksum mismatch: $item"}
 $destination=Join-Path $resolvedRoot $item
 if(Test-Path -LiteralPath $destination){
  $saved=Join-Path $backup $item
  New-Item -ItemType Directory -Path (Split-Path $saved -Parent) -Force | Out-Null
  Copy-Item -LiteralPath $destination -Destination $saved
 }
 Copy-Item -LiteralPath $source -Destination $destination -Force
}
New-Item -ItemType Directory -Path $backup -Force | Out-Null
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
Push-Location $resolvedRoot
try{
 & docker compose build main-client focus-detector
 if($LASTEXITCODE -ne 0){throw 'Docker build failed; current containers have not been restarted.'}
 & docker compose up -d --no-deps main-client focus-detector
 if($LASTEXITCODE -ne 0){throw 'Docker startup failed. Inspect docker compose logs.'}
 & docker compose ps main-client focus-detector
}finally{Pop-Location}
Write-Host "Source backup: $backup"
Write-Host 'Wait for healthy. Existing models need valid face samples before the focus ring can run.'
