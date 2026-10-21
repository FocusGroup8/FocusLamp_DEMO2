# Run with Windows PowerShell 5.1:
# powershell.exe -NoProfile -File controller/tools/generate_welcome_audio.ps1
# Uses the installed Huihui voice; no network service is used.
[CmdletBinding()]
param(
    [string]$AudioDirectory,
    [ValidateRange(-10, 10)]
    [int]$Rate = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSEdition -ne 'Desktop') {
    throw 'Run this script with Windows PowerShell 5.1 (powershell.exe).'
}

if ([string]::IsNullOrWhiteSpace($AudioDirectory)) {
    $scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
    $AudioDirectory = Join-Path $scriptDirectory '../main/audio'
}
$AudioDirectory = (Resolve-Path -LiteralPath $AudioDirectory).Path
$manifestPath = Join-Path $AudioDirectory 'welcome_messages.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.messages.Count -ne 9 -or $manifest.sample_rate_hz -ne 16000 -or
    $manifest.bits_per_sample -ne 16 -or $manifest.channels -ne 1) {
    throw 'Expected nine welcome messages with PCM16, mono, 16000 Hz settings.'
}

function Get-WaveSummary {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $reader = New-Object System.IO.BinaryReader($stream)
    try {
        if ([System.Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -ne 'RIFF') {
            throw "Not a RIFF WAV: $Path"
        }
        $riffSize = $reader.ReadUInt32()
        if ([System.Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -ne 'WAVE' -or
            $riffSize + 8 -ne $stream.Length) {
            throw "Invalid WAV header or size: $Path"
        }
        $formatTag = 0
        $channels = 0
        $sampleRate = 0
        $byteRate = 0
        $blockAlign = 0
        $bits = 0
        $dataBytes = 0
        $nonzero = $false
        while ($stream.Position + 8 -le $stream.Length) {
            $chunkId = [System.Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
            $chunkSize = $reader.ReadUInt32()
            $nextChunk = $stream.Position + $chunkSize + ($chunkSize % 2)
            if ($nextChunk -gt $stream.Length) {
                throw "Truncated WAV chunk: $Path"
            }
            if ($chunkId -eq 'fmt ') {
                if ($chunkSize -lt 16) { throw "Invalid WAV format chunk: $Path" }
                $formatTag = $reader.ReadUInt16()
                $channels = $reader.ReadUInt16()
                $sampleRate = $reader.ReadUInt32()
                $byteRate = $reader.ReadUInt32()
                $blockAlign = $reader.ReadUInt16()
                $bits = $reader.ReadUInt16()
            } elseif ($chunkId -eq 'data') {
                $dataBytes += $chunkSize
                for ($sample = 0; $sample -lt [int]($chunkSize / 2); $sample++) {
                    if ($reader.ReadInt16() -ne 0) { $nonzero = $true }
                }
            }
            $stream.Position = $nextChunk
        }
        if ($formatTag -ne 1 -or $channels -ne 1 -or $sampleRate -ne 16000 -or
            $bits -ne 16 -or $byteRate -ne 32000 -or $blockAlign -ne 2 -or
            $dataBytes -le 0 -or $dataBytes % 2 -ne 0 -or -not $nonzero) {
            throw "Expected non-silent PCM16 mono 16000 Hz audio: $Path"
        }
        [PSCustomObject]@{
            File = [System.IO.Path]::GetFileName($Path)
            Format = 'PCM16 mono 16000 Hz'
            DurationSeconds = [Math]::Round($dataBytes / [double]$byteRate, 3)
            DataBytes = $dataBytes
            FileBytes = $stream.Length
            NonzeroPcm = $nonzero
        }
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
    $synth.SelectVoice([string]$manifest.voice)
    $synth.Rate = $Rate
    $synth.Volume = 100
    $format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
        16000,
        [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono
    )
    $summaries = @(
        foreach ($message in $manifest.messages) {
            $expectedFile = 'welcome_{0:D2}.wav' -f [int]$message.id
            if ($message.file -ne $expectedFile -or $message.id -lt 1 -or $message.id -gt 9) {
                throw 'Invalid welcome message filename or id.'
            }
            $outputPath = Join-Path $AudioDirectory $expectedFile
            $synth.SetOutputToWaveFile($outputPath, $format)
            $synth.Speak([string]$message.text)
            $synth.SetOutputToNull()
            Get-WaveSummary -Path $outputPath
        }
    )
    $summaries | Format-Table -AutoSize
    $totalBytes = ($summaries | Measure-Object -Property FileBytes -Sum).Sum
    Write-Output ('Total WAV bytes: {0}' -f $totalBytes)
} finally {
    $synth.Dispose()
}
