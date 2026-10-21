param(
    [Parameter(Mandatory = $true)]
    [string]$ControllerIp
)
$ErrorActionPreference = 'Stop'
# ASCII source also works in Windows PowerShell 5.1 without a UTF-8 BOM.
Add-Type -AssemblyName System.Net.Http
$parsedIp = $null
if (-not [System.Net.IPAddress]::TryParse($ControllerIp, [ref]$parsedIp) -or
    $parsedIp.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) {
    throw 'ControllerIp must be the controller IPv4 address from the serial log.'
}
$handler = New-Object System.Net.Http.HttpClientHandler
$handler.UseProxy = $false
$client = New-Object System.Net.Http.HttpClient($handler)
$client.Timeout = [TimeSpan]::FromSeconds(8)
$baseUrl = 'http://' + $ControllerIp
try {
    $stateResponse = $client.GetAsync($baseUrl + '/api/mode').GetAwaiter().GetResult()
    try {
        $stateResponse.EnsureSuccessStatusCode() | Out-Null
        $stateText = $stateResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        $state = $stateText | ConvertFrom-Json
    } finally {
        $stateResponse.Dispose()
    }
    if ($state.ok -ne $true -or
        $state.voice_active -isnot [bool] -or $state.voice_active) {
        throw 'Controller is not confirmed idle. No TTS request sent. End the conversation yourself, then rerun.'
    }
    # One request only. A timeout may mean the server already accepted it:
    # do not automatically resend.
    $requestJson = '{"text":"\u8bf7\u76f4\u63a5\u8bf4\u51fa\u8fd9\u53e5\u8bdd\uff0c\u4e0d\u89e3\u91ca\u3001\u4e0d\u8ffd\u95ee\uff1a\u4f60\u5df2\u7ecf\u5750\u4e86\u5f88\u4e45\uff0c\u8bf7\u7ad9\u8d77\u6765\u6d3b\u52a8\u6d3b\u52a8\u5427\uff5e","priority":1}'
    $content = New-Object System.Net.Http.StringContent(
        $requestJson, [System.Text.Encoding]::UTF8, 'application/json')
    try {
        $response = $client.PostAsync($baseUrl + '/api/tts/speak', $content).GetAwaiter().GetResult()
        try {
            $response.EnsureSuccessStatusCode() | Out-Null
            $replyText = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            Write-Output ('API response: ' + $replyText)
            $reply = $replyText | ConvertFrom-Json
            if ($reply.ok -ne $true) { throw 'Controller rejected the TTS request.' }
            Write-Output 'Request QUEUED, not playback confirmed.'
            Write-Output 'Check TTS sentence logs AND listen: exact reminder, no prefix, no extra reply.'
        } finally {
            $response.Dispose()
        }
    } finally {
        $content.Dispose()
    }
} finally {
    $client.Dispose()
    $handler.Dispose()
}
