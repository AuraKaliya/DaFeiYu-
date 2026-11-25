param([string]$Panel, [string]$Fixture, [string]$App, [string]$Root)
$ErrorActionPreference = 'Stop'
$base = (Resolve-Path -LiteralPath $Root).Path
$folder = Join-Path $base ('history-' + [guid]::NewGuid().ToString('N'))
$data = Join-Path $folder 'user data'
$null = New-Item -ItemType Directory -Path $data -Force
$config = Join-Path $folder 'offline.json'
[IO.File]::WriteAllText($config,'{"provider":"deepseek","base_url":"https://offline.invalid","model":"fixture","api_key_env":"DFY_UNUSED_HISTORY_FIXTURE"}')
$children = [Collections.Generic.List[Diagnostics.Process]]::new()

# 所有子进程使用同一隔离目录，保留进程句柄、浏览器退出报告和固定检查名。
function Start-FixtureProcess([string]$Program, [string]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Program
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $child = [Diagnostics.Process]::Start($info)
    $children.Add($child)
    return $child
}

# 等待原数据目录确认刚启动的固定响应本体，再让面板走正式管道。
function Start-Resident {
    $resident = Start-FixtureProcess $Fixture ('serve "'+$config+'" "'+$data+'"')
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    do {
        try { $status = (& $App --host-status $data 2>$null | Out-String) | ConvertFrom-Json }
        catch { $status = $null }
        if ($status -and $status.pid -eq $resident.Id -and $status.state -eq 'running') { return $resident }
        if ($resident.HasExited) { throw ('Fixture exited at startup: '+$resident.StandardError.ReadToEnd()) }
        Start-Sleep -Milliseconds 40
    } while ($elapsed.ElapsedMilliseconds -lt 8000)
    throw 'Fixture resident did not acquire its directory'
}

# 每个模式启动独立的正式面板和自己的 WebView2 配置。
function Start-Panel([string]$Mode) {
    return Start-FixtureProcess $Panel ('--check-observer "'+$config+'" "'+$data+'" '+$Mode)
}

# 同时观察两窗，任一固定检查失败立即报告，只有浏览器和配置全部释放才可放行回答。
function Wait-Panels([Diagnostics.Process[]]$Panels) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $running = 0
        foreach ($window in $Panels) {
            if (!$window.HasExited) { ++$running }
            elseif ($window.ExitCode -ne 0) { throw ('Panel failed: '+$window.StandardError.ReadToEnd()) }
        }
        if (!$running) { break }
        if ($timer.ElapsedMilliseconds -ge 65000) { throw 'Window checks did not finish' }
        Start-Sleep -Milliseconds 35
    } while ($true)
    foreach ($window in $Panels) {
        $report = $window.StandardOutput.ReadToEnd() | ConvertFrom-Json
        if (!$report.profile_cleaned -or !$report.processes_exited -or $report.result -ne 0) { throw 'Window or browser still active' }
    }
}

# 使用本体正常退出协议，再用原进程句柄核对收尾。
function Stop-Resident([Diagnostics.Process]$Resident) {
    & $App --stop-host $data | Out-Null
    if ($LASTEXITCODE -ne 0 -or !$Resident.WaitForExit(7000) -or $Resident.ExitCode -ne 0) { throw 'Original resident failed to stop' }
}

$resident = $null
try {
    $resident = Start-Resident
    $watcher = Start-Panel 'history-watch'
    $sender = Start-Panel 'history-send'
    Wait-Panels @($watcher,$sender)
    [IO.File]::WriteAllText((Join-Path $data 'release-reply'),'ready')
    & $Fixture verify-closed $config $data
    if ($LASTEXITCODE -ne 0) { throw 'Closed-window durable reply verification failed' }
    Wait-Panels @((Start-Panel 'history-reopen'))
    Stop-Resident $resident

    & $Fixture seed-interrupted $config $data
    if ($LASTEXITCODE -ne 0) { throw 'Interrupted-delivery fixture failed' }
    $resident = Start-Resident
    Wait-Panels @((Start-Panel 'history-restart'))
    Stop-Resident $resident
} finally {
    [IO.File]::WriteAllText((Join-Path $data 'release-reply'),'finish')
    if ($resident -and !$resident.HasExited) {
        try { & $App --stop-host $data 2>$null | Out-Null } catch {}
        $null = $resident.WaitForExit(5000)
    }
    foreach ($child in $children) {
        if (!$child.HasExited) {
            $null = $child.CloseMainWindow()
            if (!$child.WaitForExit(2500)) { $child.Kill(); $child.WaitForExit() }
        }
        $child.Dispose()
    }
    $created = Get-Item -LiteralPath $folder
    if ($created.Parent.FullName -eq $base -and $created.Name -match '^history-[0-9a-f]{32}$' -and
        !($created.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Remove-Item -LiteralPath $created.FullName -Recurse -Force }
}
