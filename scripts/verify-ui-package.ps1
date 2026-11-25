# 验证独立 UI 运行目录，隐藏启动且不操作桌面，不调用真实模型。
# 步骤：定位运行目录 → 在受限环境执行各模式 → 核对原生回执 → 验证负向资产与 CLI → 保存结果。
param([Parameter(Mandatory)][string]$Package)
$ErrorActionPreference = 'Stop'
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$executable = Join-Path $packagePath 'dafeiyu-ui-probe.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw 'UI verification executable is missing' }
$repoRoot = Split-Path -Parent $PSScriptRoot
$logs = Join-Path $repoRoot ('build/ui-package-check/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $logs | Out-Null
$profileTemp = Join-Path $logs 'webview-temp'
New-Item -ItemType Directory -Path $profileTemp | Out-Null
$results = @()

foreach ($mode in @('--smoke','--offline','--interaction','--lifecycle','--close-early','--close-controller','--missing-runtime')) {
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo.FileName = $executable
    $process.StartInfo.WorkingDirectory = Join-Path $env:SystemRoot 'System32'
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $process.StartInfo.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)
    $process.StartInfo.Environment['PATH'] = Join-Path $env:SystemRoot 'System32'
    $process.StartInfo.Environment['TEMP'] = $profileTemp
    $process.StartInfo.Environment['TMP'] = $profileTemp
    $process.StartInfo.ArgumentList.Add($mode)
    try {
        if (-not $process.Start()) { throw "Cannot start $mode" }
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(35000)) { $process.Kill(); $process.WaitForExit(); throw "UI check timed out: $mode" }
        $stdout = $output.GetAwaiter().GetResult(); $stderr = $errors.GetAwaiter().GetResult()
        $stdout | Set-Content -LiteralPath (Join-Path $logs ($mode.Substring(2) + '.json')) -Encoding utf8
        $stderr | Set-Content -LiteralPath (Join-Path $logs ($mode.Substring(2) + '.err')) -Encoding utf8
        $expected = if ($mode -eq '--missing-runtime') { 4 } else { 0 }
        if ($process.ExitCode -ne $expected) { throw "Unexpected exit for ${mode}: $($process.ExitCode); $stderr" }
        $report = $null
        if ($expected -eq 0) {
            $report = $stdout | ConvertFrom-Json
            if ($report.result -ne 0 -or -not $report.processes_exited -or -not $report.profile_cleaned -or
                $report.pending_callbacks -or $report.protocol_pending -or $report.active_frames -or $report.live_channels) { throw "Incomplete resource receipt: $mode" }
            if ($mode -eq '--offline' -and -not $report.offline_enabled) { throw 'Offline mode was not established' }
            if ($mode -eq '--lifecycle' -and ($report.created_frames -ne 20 -or $report.destroyed_frames -ne 20)) { throw 'Frame lifecycle coverage incomplete' }
        } elseif (-not $stderr.Contains('WebView2 Runtime unavailable')) { throw 'Runtime failure reason missing' }
        $results += [pscustomobject]@{mode=$mode;exit_code=$process.ExitCode;receipt=$report}
    } finally { $process.Dispose() }
}

$withoutAssets = Join-Path $logs 'without-ui'
New-Item -ItemType Directory -Path $withoutAssets | Out-Null
Copy-Item -LiteralPath $executable -Destination $withoutAssets
Get-ChildItem -LiteralPath $packagePath -Filter '*.dll' -File | Copy-Item -Destination $withoutAssets
$missing = Start-Process -FilePath (Join-Path $withoutAssets 'dafeiyu-ui-probe.exe') -ArgumentList '--smoke' -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $logs 'missing-assets.err') -RedirectStandardOutput (Join-Path $logs 'missing-assets.json')
try {
    if (-not $missing.WaitForExit(15000)) { $missing.Kill(); $missing.WaitForExit(); throw 'Missing-assets check timed out' }
    if ($missing.ExitCode -ne 1 -or -not (Get-Content -LiteralPath (Join-Path $logs 'missing-assets.err') -Raw).Contains('UI assets missing')) { throw 'Missing-assets refusal was not confirmed' }
} finally { $missing.Dispose() }

& (Join-Path $packagePath 'dafeiyu.exe') --check-config (Join-Path $packagePath 'config/deepseek.example.json') *> (Join-Path $logs 'cli.log')
if ($LASTEXITCODE) { throw 'Independent CLI check failed' }
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $logs 'results.json') -Encoding utf8
Write-Output "UI package: 7 modes, missing-assets refusal and CLI check passed. Logs: $logs"
