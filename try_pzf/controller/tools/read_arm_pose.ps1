# Requires Windows PowerShell 5.1 or newer. ASCII source avoids BOM dependence.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ControllerUrl,

    [ValidateRange(1, 10000)]
    [int]$Samples = 20,

    [ValidateRange(0.1, 3600)]
    [double]$IntervalSeconds = 2,

    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# Accept only an origin, optionally with its final slash. Never accept an API
# path, credentials, a query, or a fragment supplied through ControllerUrl.
$origin = $null
if ($ControllerUrl -cnotmatch '\Ahttps?://[^\s/?#\\]+/?\z' -or
    -not [Uri]::TryCreate($ControllerUrl, [UriKind]::Absolute, [ref]$origin) -or
    $origin.Scheme -notin @('http', 'https') -or
    [string]::IsNullOrEmpty($origin.Host) -or
    -not [string]::IsNullOrEmpty($origin.UserInfo) -or
    $origin.AbsolutePath -ne '/' -or
    -not [string]::IsNullOrEmpty($origin.Query) -or
    -not [string]::IsNullOrEmpty($origin.Fragment)) {
    throw 'ControllerUrl must be an http://host[:port] or https://host[:port] origin only; no credentials, path, query, or fragment.'
}
$endpoint = New-Object Uri($origin.GetLeftPart([UriPartial]::Authority) + '/api/arm/pose')

function Assert-Number {
    param($Value, [string]$Name, [switch]$Nullable, [switch]$Whole)
    if ($null -eq $Value) {
        if ($Nullable) { return }
        throw ($Name + ' must not be null.')
    }
    $numeric = $Value -is [byte] -or $Value -is [sbyte] -or
        $Value -is [int16] -or $Value -is [uint16] -or
        $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64] -or
        $Value -is [single] -or $Value -is [double] -or $Value -is [decimal]
    if (-not $numeric -or [double]::IsNaN([double]$Value) -or
        [double]::IsInfinity([double]$Value)) {
        throw ($Name + ' must be a finite JSON number.')
    }
    if ($Whole -and ($Value -lt 0 -or [math]::Truncate([double]$Value) -ne $Value)) {
        throw ($Name + ' must be a non-negative integer.')
    }
}

function Assert-PoseResponse {
    param($Pose)
    if ($Pose.ok -isnot [bool] -or -not $Pose.ok -or
        $Pose.read_only -isnot [bool] -or -not $Pose.read_only -or
        $Pose.source -cne 'servo-feedback-cache' -or
        $Pose.sampling_mode -cne 'idle_only' -or
        $Pose.sampling_started -isnot [bool] -or -not $Pose.sampling_started -or
        $Pose.motion_busy -isnot [bool] -or $Pose.stale_after_ms -ne 10000 -or
        @($Pose.axes).Count -ne 5) {
        throw 'Response does not match the read-only five-axis cache contract.'
    }
    Assert-Number $Pose.captured_at_ms 'captured_at_ms' -Whole
    Assert-Number $Pose.stale_after_ms 'stale_after_ms' -Whole
    $seen = @{}
    foreach ($axis in $Pose.axes) {
        Assert-Number $axis.axis 'axis' -Whole
        Assert-Number $axis.bus_id 'bus_id' -Whole
        if ($axis.axis -gt 4 -or $seen.ContainsKey([int]$axis.axis)) {
            throw 'Response axis IDs must be unique and cover 0 through 4.'
        }
        $index = [int]$axis.axis
        $seen[$index] = $true
        $protocol = if ($index -eq 0) { 'em3' } else { 'lx' }
        if ($axis.bus_id -ne $busIds[$index] -or $axis.protocol -cne $protocol -or
            $axis.valid -isnot [bool] -or $axis.stale -isnot [bool]) {
            throw ('Invalid axis mapping or flags for axis ' + $index + '.')
        }
        Assert-Number $axis.raw 'raw' -Nullable
        foreach ($name in @('age_ms', 'last_attempt_at_ms', 'last_ok_at_ms')) {
            Assert-Number $axis.$name $name -Nullable -Whole
        }
        foreach ($name in @('attempts', 'successes', 'failures')) {
            Assert-Number $axis.$name $name -Whole
            if ($axis.$name -gt [uint32]::MaxValue) { throw ($name + ' exceeds uint32.') }
        }
        if ($axis.valid -and ($null -eq $axis.raw -or $axis.stale)) {
            throw 'A valid axis must have a non-stale raw value.'
        }
    }
}

$busIds = @(4, 1, 2, 3, 5)
$stats = @()
for ($i = 0; $i -lt 5; $i++) {
    $stats += [pscustomobject]@{
        axis = $i; bus_id = $busIds[$i]
        protocol = $(if ($i -eq 0) { 'em3' } else { 'lx' })
        baseline_seen = $false; previous_attempts = $null
        previous_attempt_at_ms = $null; latest = $null
        response_snapshots = 0; repeated_cache_snapshots = 0
        new_attempt_observations = 0; new_completed_attempts = [long]0
        completion_count_is_lower_bound = $false
        fresh_valid_observations = 0; fresh_invalid_observations = 0
        observed_valid_raw_min = $null; observed_valid_raw_max = $null
        baseline_segments = 0
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path (Get-Location).Path 'arm_readonly_capture'
}
$runName = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$outputBase = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$runDirectory = Join-Path $outputBase $runName
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
$samplesPath = Join-Path $runDirectory 'arm_pose_samples.csv'
$summaryPath = Join-Path $runDirectory 'arm_pose_summary.csv'
$reportPath = Join-Path $runDirectory 'arm_pose_report.txt'
$startedUtc = [DateTime]::UtcNow.ToString('o')
$requestsOk = 0
$requestsFailed = 0
$previousCaptureMs = $null
$observedClockRegressions = 0
$csvStarted = $false

Add-Type -AssemblyName System.Net.Http
$handler = New-Object System.Net.Http.HttpClientHandler
$handler.AllowAutoRedirect = $false
$handler.UseProxy = $false
$client = New-Object System.Net.Http.HttpClient($handler)
$client.Timeout = [TimeSpan]::FromSeconds(8)
$client.DefaultRequestHeaders.CacheControl = New-Object System.Net.Http.Headers.CacheControlHeaderValue
$client.DefaultRequestHeaders.CacheControl.NoCache = $true
Write-Host ('GET only: ' + $endpoint.AbsoluteUri)
Write-Host ('Output: ' + $runDirectory)

try {
    for ($requestIndex = 1; $requestIndex -le $Samples; $requestIndex++) {
        $pcUtc = [DateTime]::UtcNow.ToString('o')
        $pose = $null
        $statusCode = $null
        $errorText = $null
        $response = $null
        try {
            # Exactly one GET per scheduled request. Redirects and retries are disabled.
            $response = $client.GetAsync($endpoint).GetAwaiter().GetResult()
            $statusCode = [int]$response.StatusCode
            if ($statusCode -ne 200) { throw ('HTTP ' + $statusCode + '; no redirect or retry followed.') }
            $bodyText = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            $candidate = $bodyText | ConvertFrom-Json
            Assert-PoseResponse $candidate
            $pose = $candidate
            $requestsOk++
        } catch {
            $errorText = $_.Exception.Message
            $requestsFailed++
            Write-Warning ('Request ' + $requestIndex + '/' + $Samples + ': ' + $errorText)
        } finally {
            if ($null -ne $response) { $response.Dispose() }
        }

        $clockRegressed = $false
        if ($null -ne $pose) {
            $clockRegressed = $null -ne $previousCaptureMs -and $pose.captured_at_ms -lt $previousCaptureMs
            if ($clockRegressed) { $observedClockRegressions++ }
            $previousCaptureMs = $pose.captured_at_ms
        }
        $rows = @()
        for ($i = 0; $i -lt 5; $i++) {
            $state = $stats[$i]
            $axis = $null
            $newObservation = $false
            $newCompletions = $null
            $baseline = $false
            $counterRegressed = $false
            $lowerBound = $false
            if ($null -ne $pose) {
                $axis = @($pose.axes | Where-Object { $_.axis -eq $i })[0]
                $state.response_snapshots++
                $state.latest = $axis
                if ($state.baseline_seen) {
                    $counterRegressed = $axis.attempts -lt $state.previous_attempts -or
                        ($null -ne $axis.last_attempt_at_ms -and $null -ne $state.previous_attempt_at_ms -and
                         $axis.last_attempt_at_ms -lt $state.previous_attempt_at_ms)
                }
                if (-not $state.baseline_seen -or $clockRegressed -or $counterRegressed) {
                    # A first snapshot (including after observed reset) is a baseline,
                    # not an independent sample completed during this collection.
                    $baseline = $true
                    $state.baseline_seen = $true
                    $state.baseline_segments++
                    $newCompletions = 0
                } else {
                    $timestampChanged = $axis.last_attempt_at_ms -ne $state.previous_attempt_at_ms
                    $counterChanged = $axis.attempts -gt $state.previous_attempts
                    $newObservation = $timestampChanged -or $counterChanged
                    $newCompletions = [long]$axis.attempts - [long]$state.previous_attempts
                    if ($newObservation) {
                        if ($axis.attempts -eq [uint32]::MaxValue) {
                            $lowerBound = $true
                            $state.completion_count_is_lower_bound = $true
                        }
                        if ($newCompletions -eq 0) {
                            # Saturated counters can still expose a new completion time.
                            $newCompletions = 1
                            $lowerBound = $true
                            $state.completion_count_is_lower_bound = $true
                        }
                        $state.new_attempt_observations++
                        $state.new_completed_attempts += $newCompletions
                        if ($axis.valid) {
                            $state.fresh_valid_observations++
                            if ($null -eq $state.observed_valid_raw_min -or $axis.raw -lt $state.observed_valid_raw_min) {
                                $state.observed_valid_raw_min = $axis.raw
                            }
                            if ($null -eq $state.observed_valid_raw_max -or $axis.raw -gt $state.observed_valid_raw_max) {
                                $state.observed_valid_raw_max = $axis.raw
                            }
                        } else { $state.fresh_invalid_observations++ }
                    } else { $state.repeated_cache_snapshots++ }
                }
                $state.previous_attempts = $axis.attempts
                $state.previous_attempt_at_ms = $axis.last_attempt_at_ms
            }

            # No cast of nullable fields: JSON null remains an empty CSV cell, not 0.
            $rows += [pscustomobject][ordered]@{
                request_index = $requestIndex; pc_observed_utc = $pcUtc
                request_ok = ($null -ne $pose); http_status = $statusCode; error = $errorText
                ok = $(if ($null -ne $pose) { $pose.ok } else { $null })
                read_only = $(if ($null -ne $pose) { $pose.read_only } else { $null })
                source = $(if ($null -ne $pose) { $pose.source } else { $null })
                sampling_mode = $(if ($null -ne $pose) { $pose.sampling_mode } else { $null })
                sampling_started = $(if ($null -ne $pose) { $pose.sampling_started } else { $null })
                motion_busy = $(if ($null -ne $pose) { $pose.motion_busy } else { $null })
                captured_at_ms = $(if ($null -ne $pose) { $pose.captured_at_ms } else { $null })
                stale_after_ms = $(if ($null -ne $pose) { $pose.stale_after_ms } else { $null })
                axis = $i; bus_id = $state.bus_id; protocol = $state.protocol
                raw = $(if ($null -ne $axis) { $axis.raw } else { $null })
                valid = $(if ($null -ne $axis) { $axis.valid } else { $null })
                stale = $(if ($null -ne $axis) { $axis.stale } else { $null })
                age_ms = $(if ($null -ne $axis) { $axis.age_ms } else { $null })
                last_attempt_at_ms = $(if ($null -ne $axis) { $axis.last_attempt_at_ms } else { $null })
                last_ok_at_ms = $(if ($null -ne $axis) { $axis.last_ok_at_ms } else { $null })
                attempts = $(if ($null -ne $axis) { $axis.attempts } else { $null })
                successes = $(if ($null -ne $axis) { $axis.successes } else { $null })
                failures = $(if ($null -ne $axis) { $axis.failures } else { $null })
                baseline = $baseline; observed_clock_regression = $clockRegressed
                observed_counter_regression = $counterRegressed
                new_attempt_observation = $newObservation; new_completed_attempts = $newCompletions
                completion_count_is_lower_bound = $lowerBound
            }
        }
        if ($csvStarted) { $rows | Export-Csv -LiteralPath $samplesPath -NoTypeInformation -Encoding UTF8 -Append }
        else { $rows | Export-Csv -LiteralPath $samplesPath -NoTypeInformation -Encoding UTF8; $csvStarted = $true }
        Write-Host ('Request ' + $requestIndex + '/' + $Samples + ': ' + $(if ($null -ne $pose) { 'cache recorded' } else { 'failure recorded' }))
        # Delay after completion; no catch-up burst after a slow or failed GET.
        if ($requestIndex -lt $Samples) { Start-Sleep -Milliseconds ([int][math]::Ceiling($IntervalSeconds * 1000)) }
    }
} finally {
    $client.Dispose()
    $handler.Dispose()
}

$summary = foreach ($state in $stats) {
    $latest = $state.latest
    [pscustomobject][ordered]@{
        axis = $state.axis; bus_id = $state.bus_id; protocol = $state.protocol
        latest_raw = $(if ($null -ne $latest) { $latest.raw } else { $null })
        latest_valid = $(if ($null -ne $latest) { $latest.valid } else { $null })
        latest_stale = $(if ($null -ne $latest) { $latest.stale } else { $null })
        latest_age_ms = $(if ($null -ne $latest) { $latest.age_ms } else { $null })
        latest_last_attempt_at_ms = $(if ($null -ne $latest) { $latest.last_attempt_at_ms } else { $null })
        latest_last_ok_at_ms = $(if ($null -ne $latest) { $latest.last_ok_at_ms } else { $null })
        latest_attempts = $(if ($null -ne $latest) { $latest.attempts } else { $null })
        latest_successes = $(if ($null -ne $latest) { $latest.successes } else { $null })
        latest_failures = $(if ($null -ne $latest) { $latest.failures } else { $null })
        response_snapshots = $state.response_snapshots
        baseline_segments = $state.baseline_segments
        repeated_cache_snapshots = $state.repeated_cache_snapshots
        new_attempt_observations = $state.new_attempt_observations
        new_completed_attempts = $state.new_completed_attempts
        completion_count_is_lower_bound = $state.completion_count_is_lower_bound
        fresh_valid_observations = $state.fresh_valid_observations
        fresh_invalid_observations = $state.fresh_invalid_observations
        observed_valid_raw_min = $state.observed_valid_raw_min
        observed_valid_raw_max = $state.observed_valid_raw_max
        reference_raw = $null; mechanical_safe_min = $null; mechanical_safe_max = $null
        stability_assessment = 'not_assessed'; mechanical_safety_assessment = 'not_assessed'
    }
}
$summary | Export-Csv -LiteralPath $summaryPath -NoTypeInformation -Encoding UTF8
$table = $summary | Format-Table axis, bus_id, protocol, latest_raw, latest_valid, latest_stale,
    new_completed_attempts, fresh_valid_observations -AutoSize | Out-String -Width 220
Write-Host $table
$report = @(
    'ARM READ-ONLY CACHE COLLECTION - no stability or mechanical safety verdict',
    ('Endpoint: GET ' + $endpoint.AbsoluteUri),
    ('PC collection started UTC: ' + $startedUtc),
    ('PC collection ended UTC: ' + [DateTime]::UtcNow.ToString('o')),
    ('Requested GETs: ' + $Samples + '; valid responses: ' + $requestsOk + '; failed requests/responses: ' + $requestsFailed),
    ('Observed controller clock regressions: ' + $observedClockRegressions),
    'First valid response per axis/observed reset is a baseline, excluded from new sample counts.',
    'New counts use only attempts/last_attempt_at_ms changes. Repeated cache is not an independent sample.',
    'new_completed_attempts is a counter delta, not the number of raw values captured by this PC.',
    'Counter saturation may make that count a lower bound; unobserved resets cannot be ruled out.',
    'fresh_valid_observations counts newly observed latest attempts whose reported valid flag is true.',
    'Min/max include only those fresh valid observations; missed intermediate raw values are unknown.',
    'latest_* describes the last valid HTTP response, not a refreshed value at report-writing time.',
    'Controller milliseconds are monotonic uptime. Never subtract PC UTC from controller timestamps.',
    'Null raw/timestamps remain empty CSV cells. Numeric 0 is preserved as a value.',
    'Raw units differ between protocols; no degree conversion or reference pose is assumed.',
    'Reference pose and mechanical safe limits are UNKNOWN and intentionally blank.',
    'One successful read does not demonstrate stability. This collector does not assess stability.',
    'No motion, home, limit write, restart, serial access, or flashing is performed.',
    '', $table,
    ('Full per-axis request records: ' + $samplesPath),
    ('Five-axis summary: ' + $summaryPath)
)
$report | Set-Content -LiteralPath $reportPath -Encoding UTF8
Write-Host ('Report: ' + $reportPath)
if ($requestsFailed -gt 0) { Write-Warning 'Some GETs failed or violated the contract; see request_ok/error in the sample CSV.' }
