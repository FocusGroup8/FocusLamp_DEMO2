param([Parameter(Mandatory=$true)][string]$ControllerUrl,[int]$Seconds=150)
$ErrorActionPreference='Stop'
$endpoint=$ControllerUrl.TrimEnd('/')+'/api/mode'
Write-Host 'Starts a two-minute focus session. An already-running session returns 409 and is not reset.'
Invoke-RestMethod -Method Post -Uri $endpoint -ContentType 'application/json' -Body '{"mode":"focus","duration":2}' -TimeoutSec 3 | Format-List
$rows=@();$started=[Diagnostics.Stopwatch]::StartNew()
while($started.Elapsed.TotalSeconds -lt $Seconds){
 $state=Invoke-RestMethod -Uri $endpoint -TimeoutSec 3
 $rows+=[pscustomobject]@{wall_sec=[math]::Round($started.Elapsed.TotalSeconds,2);boot=$state.controller_boot_id;session=$state.session_id;seq=$state.seq;remaining_ms=$state.remaining_ms;rate_half=$state.rate_half;reason=$state.pause_reason;completed=$state.completed}
 Write-Host ('remaining={0}ms rate={1}/2 reason={2} completed={3}' -f $state.remaining_ms,$state.rate_half,$state.pause_reason,$state.completed)
 if($state.completed){break}
 Start-Sleep -Milliseconds 500
}
$out=Join-Path $PSScriptRoot ('focus-capture-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.csv')
$rows | Export-Csv -LiteralPath $out -NoTypeInformation -Encoding UTF8
Write-Host "Saved $out. Compare the two physical displays; this script does not claim display-latency measurement."
