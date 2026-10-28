param([Parameter(Mandatory=$true)][ValidateSet('controller','head')][string]$Project)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../$Project"))
$specifications = if ($Project -eq 'head') {
    @(@{ Component='espressif__esp_wifi_remote'; Version='1.6.2'; File='esp_wifi_remote_net2.c' })
} else {
    @(
        @{ Component='espressif__esp_xiaozhi'; Version='0.1.1'; File='CMakeLists.txt' },
        @{ Component='espressif__esp_xiaozhi'; Version='0.1.1'; File='src/esp_xiaozhi_mcp.c' },
        @{ Component='espressif__esp_xiaozhi'; Version='0.1.1'; File='src/esp_xiaozhi_chat.c' },
        @{ Component='espressif__esp_xiaozhi'; Version='0.1.1'; File='src/esp_xiaozhi_transport.c' },
        @{ Component='espressif__esp_xiaozhi'; Version='0.1.1'; File='src/priv_include/esp_xiaozhi_mcp.h' },
        @{ Component='espressif__esp_xiaozhi'; Version='0.1.1'; File='src/priv_include/esp_xiaozhi_transport.h' },
        @{ Component='espressif__esp_xiaozhi'; Version='0.1.1'; File='src/priv_include/esp_xiaozhi_websocket.h' },
        @{ Component='espressif__esp_websocket_client'; Version='1.7.0'; File='include/esp_websocket_client.h' },
        @{ Component='espressif__esp_xiaozhi'; Version='0.1.1'; File='src/esp_xiaozhi_websocket.c' },
        @{ Component='espressif__esp_websocket_client'; Version='1.7.0'; File='esp_websocket_client.c' }
    )
}
# Validate every destination before copying; do not apply an old overlay to a
# newly upgraded dependency. Run idf.py reconfigure to download dependencies first.
foreach ($spec in $specifications) {
    $componentRoot = Join-Path $projectRoot "managed_components/$($spec.Component)"
    $manifest = Join-Path $componentRoot 'idf_component.yml'
    if (!(Test-Path -LiteralPath $manifest)) { throw "Missing dependency $($spec.Component). Run idf.py reconfigure first." }
    $versionLine = Get-Content -LiteralPath $manifest | Where-Object { $_ -match '^version:' } | Select-Object -First 1
    $version = ($versionLine -replace '^version:\s*','').Trim().Trim('"', "'")
    if ($version -ne $spec.Version) { throw "Version mismatch: $($spec.Component) expected $($spec.Version), got $version. No overlays applied." }
    if (!(Test-Path -LiteralPath (Join-Path $componentRoot $spec.File))) { throw "Missing target source: $($spec.File)" }
    if (!(Test-Path -LiteralPath (Join-Path $projectRoot "dependency_overrides/$($spec.Component)/$($spec.File)"))) { throw "Missing overlay: $($spec.File)" }
}
foreach ($spec in $specifications) {
    $destination = Join-Path $projectRoot "managed_components/$($spec.Component)/$($spec.File)"
    $source = Join-Path $projectRoot "dependency_overrides/$($spec.Component)/$($spec.File)"
    if ((Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $destination).Hash) {
        Write-Output "Already applied: $($spec.Component)/$($spec.File)"
        continue
    }
    $backup = Join-Path $projectRoot "dependency_backups/$($spec.Component)/$($spec.File)"
    if (!(Test-Path -LiteralPath $backup)) {
        New-Item -ItemType Directory -Path (Split-Path $backup) -Force | Out-Null
        Copy-Item -LiteralPath $destination -Destination $backup
    }
    Copy-Item -LiteralPath $source -Destination $destination
    Write-Output "Applied: $($spec.Component)/$($spec.File)"
}
