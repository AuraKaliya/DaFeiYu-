param([string]$Panel, [string]$App, [string]$Root, [switch]$Unexpected)
$ErrorActionPreference = 'Stop'
$base = (Resolve-Path -LiteralPath $Root).Path
$folder = Join-Path $base ('exit-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $folder
$data = Join-Path $folder 'user data'
$config = Join-Path $folder 'offline.json'
[IO.File]::WriteAllText($config,'{"provider":"deepseek","base_url":"https://offline.invalid","model":"fixture","api_key_env":"DFY_UNUSED_OBSERVER_FIXTURE"}')

# 打开使用隔离目录的隐藏观察窗口，保留系统句柄以独立核对实际退出。
function Start-Panel([string]$Mode) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Panel
    $info.Arguments = '--check-observer "' + $config + '" "' + $data + '" ' + $Mode
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    return [Diagnostics.Process]::Start($info)
}

# 等待独立窗口实际通过设置页检查，再由测试观察进程退出。
# 步骤：按剩余期限读取诊断行 → 匹配已通过的页面检查或报告超时。
function Wait-Ready([Diagnostics.Process]$Process, [string]$Marker = 'Interaction protocol checks passed') {
    $until = [Diagnostics.Stopwatch]::StartNew()
    while ($until.ElapsedMilliseconds -lt 15000) {
        $read = $Process.StandardError.ReadLineAsync()
        $remaining = [Math]::Max(1, [int](15000 - $until.ElapsedMilliseconds))
        if (!$read.Wait($remaining) -or $null -eq $read.Result) { break }
        if ($read.Result.Contains($Marker)) { return }
    }
    throw 'Passive window did not reach the real settings page'
}

# 在测试夹具清理前观察窗口退出码、WebView2 进程与独立临时目录。
# 步骤：等待窗口进程自行退出 → 从正式回收报告核对各项实效。
function Assert-Closed([Diagnostics.Process]$Process) {
    if (!$Process.WaitForExit(8000)) { throw "Original host exited but window $($Process.Id) stayed alive" }
    $report = $Process.StandardOutput.ReadToEnd() | ConvertFrom-Json
    if ($Process.ExitCode -ne 0 -or $report.result -ne 0 -or !$report.profile_cleaned -or !$report.processes_exited -or
        $report.browser_processes -lt 1) { throw "Incomplete native window cleanup: $($Process.Id)" }
}

# 从本次隔离目录的各进程日志读出完整事件行。
function Read-SoftwareLog {
    Get-ChildItem -LiteralPath (Join-Path $data 'logs') -Filter '*.jsonl' -File |
        ForEach-Object { Get-Content -Encoding UTF8 -LiteralPath $_.FullName | ForEach-Object { $_ | ConvertFrom-Json } }
}

$observer = $null
$initiator = $null
$owner = $null
$replacement = $null
try {
    if ($Unexpected) {
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = $App
        $info.Arguments = '--host "' + $config + '" --data "' + $data + '" --plugins "' + (Join-Path $data 'plugins') + '"'
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $owner = [Diagnostics.Process]::Start($info)
        $null = $owner.Handle
        $observer = Start-Panel 'unexpected'
        Wait-Ready $observer 'Observer ready for unexpected exit'
        $state = (& $App --host-status $data | Out-String) | ConvertFrom-Json
        if ($state.pid -ne $owner.Id) { throw 'Window did not attach to the fixture-owned process' }
        $owner.Kill()
        if (!$owner.WaitForExit(5000)) { throw 'Original fixture process did not exit' }
        $replacement = [Diagnostics.Process]::Start($info)
        $until = [Diagnostics.Stopwatch]::StartNew()
        do {
            Start-Sleep -Milliseconds 70
            try { $current = (& $App --host-status $data 2>$null | Out-String) | ConvertFrom-Json }
            catch { $current = $null }
        } while ((!$current -or $current.state -ne 'running') -and !$replacement.HasExited -and $until.ElapsedMilliseconds -lt 5000)
        if ($current.pid -ne $replacement.Id -or $current.instance -eq $state.instance) { throw 'Replacement did not take the old directory' }
        [IO.File]::WriteAllText((Join-Path $data 'replacement.ready'),'ready')
        Wait-Ready $observer
        Assert-Closed $observer
        $still = (& $App --host-status $data | Out-String) | ConvertFrom-Json
        if ($still.instance -ne $current.instance -or $still.state -ne 'running') { throw 'Old window changed the replacement resident' }
        $witness = @(Read-SoftwareLog | Where-Object { $_.pid -eq $observer.Id -and $_.event -eq 'host.exited' })
        if ($witness.Count -ne 1 -or $witness[0].peerPid -ne $owner.Id -or $witness[0].result -ne 'failed' -or
            $witness[0].operation -or $witness[0].exitCode -ne ([long]$owner.ExitCode -band 4294967295L)) { throw 'Incorrect unexpected-exit witness' }
        return
    }
    $observer = Start-Panel 'wait'
    Wait-Ready $observer
    $state = (& $App --host-status $data | Out-String) | ConvertFrom-Json
    $owner = [Diagnostics.Process]::GetProcessById([int]$state.pid)
    $null = $owner.Handle
    $initiator = Start-Panel 'stop'
    if (!$owner.WaitForExit(15000) -or $owner.ExitCode -ne 0) { throw 'Original host did not exit normally' }
    Assert-Closed $observer
    Assert-Closed $initiator
    $rows = @(Read-SoftwareLog)
    $registration = @($rows | Where-Object { $_.event -eq 'tray.start' })
    $removal = @($rows | Where-Object { $_.event -eq 'tray.removed' })
    if ($registration.Count -ne 1 -or $registration[0].pid -ne $owner.Id -or
        $registration[0].instance -ne $state.instance) { throw 'Two panels must share the original resident tray' }
    if ($registration[0].result -eq 'ok') {
        if ($removal.Count -ne 1 -or $removal[0].pid -ne $owner.Id -or $removal[0].result -ne 'ok' -or
            $removal[0].operation -ne ('stop-' + $state.instance)) { throw 'Original tray removal was not confirmed' }
    } elseif ($registration[0].result -ne 'failed' -or $removal.Count -ne 0) { throw 'Invalid unavailable-desktop receipts' }
    foreach ($window in @($observer,$initiator)) {
        $witness = @($rows | Where-Object { $_.pid -eq $window.Id -and $_.event -eq 'host.exited' })
        if ($witness.Count -ne 1 -or $witness[0].peerPid -ne $owner.Id -or $witness[0].exitCode -ne 0 -or
            $witness[0].operation -ne ('stop-' + $state.instance)) { throw 'Window lacks an original-process exit witness' }
    }
} finally {
    foreach ($child in @($observer,$initiator)) {
        if ($child) {
            if (!$child.HasExited) { $null = $child.CloseMainWindow(); if (!$child.WaitForExit(2000)) { $child.Kill(); $child.WaitForExit() } }
            $child.Dispose()
        }
    }
    if ($owner -and !$owner.HasExited) { & $App --stop-host $data | Out-Null; $null = $owner.WaitForExit(6000) }
    if ($owner) { $owner.Dispose() }
    if ($replacement) {
        if (!$replacement.HasExited) { & $App --stop-host $data | Out-Null; if (!$replacement.WaitForExit(6000)) { $replacement.Kill(); $replacement.WaitForExit() } }
        $replacement.Dispose()
    }
    $created = Get-Item -LiteralPath $folder
    if ($created.Parent.FullName -eq $base -and $created.Name -match '^exit-[0-9a-f]{32}$' -and
        !($created.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Remove-Item -LiteralPath $created.FullName -Force -Recurse }
}
