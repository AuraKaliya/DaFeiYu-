param([string]$Panel, [string]$Fixture, [string]$App, [string]$Root, [string]$MailPlugin, [switch]$PluginSettings, [switch]$PluginActions)
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
# 步骤：跟踪窗口和同伴进程 → 核对退出码 → 检查浏览器与配置回收报告。
function Wait-Panels([Diagnostics.Process[]]$Panels, [Diagnostics.Process]$Peer) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $running = 0
        if ($Peer -and $Peer.HasExited -and $Peer.ExitCode -ne 0) { throw ('Observer failed: '+$Peer.StandardError.ReadToEnd()) }
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
    if ($MailPlugin) {
        # 由真正的 C 插件产生来信，只有这个独立终端控制端可以请求取件或重载。
        $package = Join-Path $data 'plugins/mail'
        $null = New-Item -ItemType Directory -Path $package -Force
        Copy-Item -LiteralPath $MailPlugin -Destination (Join-Path $package 'entry.dll')
        [IO.File]::WriteAllText((Join-Path $package 'plugin.json'),'{"manifest_version":1,"id":"test.mail","name":"Mailbox fixture","version":"1","abi_version":2,"entry":"entry.dll","default_config":"defaults.json"}')
        [IO.File]::WriteAllText((Join-Path $package 'defaults.json'),'{"mode":"t"}')
        if ($PluginSettings -or $PluginActions) {
            $second = Join-Path $data 'plugins/second'
            $null = New-Item -ItemType Directory -Path $second
            Copy-Item -LiteralPath $MailPlugin -Destination (Join-Path $second 'entry.dll')
            [IO.File]::WriteAllText((Join-Path $second 'plugin.json'),'{"manifest_version":1,"id":"test.second","name":"Second fixture","version":"2","abi_version":2,"entry":"entry.dll","default_config":"defaults.json"}')
            [IO.File]::WriteAllText((Join-Path $second 'defaults.json'),'{"mode":"t"}')
            $resident = Start-Resident
            if ($PluginActions) {
                $watcher = Start-Panel 'action-observe'
                $manager = Start-Panel 'action-write'
                Wait-Panels @($watcher,$manager)
                Wait-Panels @((Start-Panel 'action-reopen'))
                $original = (& $App --host-status $data | Out-String) | ConvertFrom-Json
                if ($original.pid -ne $resident.Id -or (Test-Path -LiteralPath (Join-Path $data 'fixed-model-calls'))) {
                    throw 'Package operations changed the resident or called the model'
                }
                Stop-Resident $resident
                return
            }
            $watcher = Start-Panel 'plugins-watch'
            $manager = Start-Panel 'plugins-manage'
            $timer = [Diagnostics.Stopwatch]::StartNew()
            foreach ($name in 'plugins-watch.json','plugins-manage.json') {
                while (!(Test-Path -LiteralPath (Join-Path $data $name))) {
                    foreach ($window in $watcher,$manager) {
                        if ($window.HasExited) { throw ('Directory window exited before sampling: '+$window.StandardError.ReadToEnd()) }
                    }
                    if ($timer.ElapsedMilliseconds -ge 20000) { throw ('Missing directory sample: '+$name) }
                    Start-Sleep -Milliseconds 40
                }
            }
            if ([IO.File]::ReadAllText((Join-Path $data 'plugins-watch.json')) -ne [IO.File]::ReadAllText((Join-Path $data 'plugins-manage.json'))) {
                throw 'The two windows did not observe the same resident, revision and generation'
            }
            & $Fixture reload $config $data
            if ($LASTEXITCODE -ne 0) { throw 'Independent reload did not finish' }
            [IO.File]::WriteAllText((Join-Path $data 'plugin-reloaded'),'ready')
            Wait-Panels @($manager,$watcher)
            if (Test-Path -LiteralPath (Join-Path $data 'fixed-model-calls')) { throw 'Reading package settings started the model' }
            Stop-Resident $resident
            return
        }
        $resident = Start-Resident
        $watcher = Start-Panel 'mailbox-follow'
        $first = Start-Panel 'mailbox-first'
        Wait-Panels @($first) $watcher
        $expected = [IO.File]::ReadAllText((Join-Path $data 'first-letter.json'))
        $observed = [IO.File]::ReadAllText((Join-Path $data 'observer-letter.json'))
        $counter = Join-Path $data 'fixed-model-calls'
        if ($expected -ne $observed -or (Test-Path -LiteralPath $counter)) { throw 'Window reads changed the letter or started the model' }
        [IO.File]::WriteAllText((Join-Path $data 'first-closed'),'closed')
        foreach ($step in @(@('observer-offline','review','system-reviewed'), @('received-visible','reload','plugin-reloaded'))) {
            $timer = [Diagnostics.Stopwatch]::StartNew()
            while (!(Test-Path -LiteralPath (Join-Path $data $step[0]))) {
                if ($watcher.HasExited) { throw ('Mailbox observer exited: '+$watcher.StandardError.ReadToEnd()) }
                if ($timer.ElapsedMilliseconds -gt 15000) { throw ('Missing mailbox milestone: '+$step[0]) }
                Start-Sleep -Milliseconds 40
            }
            if ($step[1] -eq 'reload') {
                $reopened = Start-Panel 'mailbox-reopen'
                Wait-Panels @($reopened) $watcher
            }
            & $Fixture $step[1] $config $data
            if ($LASTEXITCODE -ne 0) { throw ('Independent system command failed: '+$step[1]) }
            [IO.File]::WriteAllText((Join-Path $data $step[2]),'ready')
        }
        Wait-Panels @($watcher)
        if ([IO.File]::ReadAllLines($counter).Count -ne 1) { throw 'Only the explicit system review should run the fixed model' }
        Stop-Resident $resident
        return
    }
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
    if (!$MailPlugin) { [IO.File]::WriteAllText((Join-Path $data 'release-reply'),'finish') }
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
