param([string]$Panel, [string]$App, [string]$Root)
$ErrorActionPreference = 'Stop'
$rootPath = (Resolve-Path -LiteralPath $Root).Path
$folder = Join-Path $rootPath ('observer-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -Type Directory -Path $folder
$data = Join-Path $folder 'user data'
$config = Join-Path $folder 'offline config.json'
$json = '{"provider":"deepseek","base_url":"https://offline.invalid","model":"offline","api_key_env":"DFY_UNUSED_OBSERVER_FIXTURE"}'
[IO.File]::WriteAllText($config,$json,[Text.UTF8Encoding]::new($false))

# 在隔离目录打开隐藏的正式面板，核对原生协议验收及浏览器的实际回收。
function Invoke-Observer([string[]]$ModeArgs, [string]$ProfilePath = $data) {
    $report = & $Panel --check-observer $config $ProfilePath @ModeArgs | Out-String
    if ($LASTEXITCODE -ne 0) { throw "Observer failed: $report" }
    $view = $report | ConvertFrom-Json
    if (!$view.profile_cleaned -or !$view.processes_exited -or $view.result -ne 0) { throw "Observer release failed: $report" }
}

# 在同一测试目录核对后台状态，保留进程和实例两个身份。
function Read-Resident([string]$ProfilePath = $data) {
    $reply = & $App --host-status $ProfilePath | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'No resident after closing an observer' }
    $state = $reply | ConvertFrom-Json
    if ($state.state -ne 'running') { throw "Unexpected resident state: $reply" }
    return $state
}

# 启动仅使用本测试目录与离线配置的终端，保留标准输入供有界关闭。
function Start-TestRuntime([string]$Mode, [string]$ModelPath = $config) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $App
    $info.Arguments = $Mode + ' "' + $ModelPath + '" --data "' + $data + '" --plugins "' + (Join-Path $data 'plugins') + '"'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    return [Diagnostics.Process]::Start($info)
}

# 核对另一类入口占用目录时，程序拒绝启动且没有创建新的业务会话。
function Assert-RuntimeRefused([string]$Mode) {
    $other = Start-TestRuntime $Mode
    try {
        $other.StandardInput.Close()
        if (!$other.WaitForExit(5000) -or $other.ExitCode -eq 0 -or
            !$other.StandardError.ReadToEnd().Contains('already in use')) { throw "Duplicate runtime accepted: $Mode" }
    } finally {
        if (!$other.HasExited) { $other.Kill(); $other.WaitForExit() }
        $other.Dispose()
    }
}

# 从正式启动文件逐个核对进程、时钟、构建与退出结果。
# 步骤：读取隔离目录的日志 → 检查每个进程自己的首尾 → 对照实际观察到的后台、窗口和终端。
function Assert-SoftwareLogs([int]$HostPid, [int]$TerminalPid) {
    $records = @()
    $runs = foreach ($file in Get-ChildItem -LiteralPath (Join-Path $data 'logs') -Filter 'software-*.jsonl' -File) {
        $rows = @(Get-Content -Encoding UTF8 -LiteralPath $file.FullName | ForEach-Object { $_ | ConvertFrom-Json })
        $records += $rows
        $first, $last = $rows[0], $rows[-1]
        if ($first.event -ne 'process.started' -or $first.seq -ne 1 -or !$first.build -or $first.utcMs -le 0 -or
            $first.level -ne 'info' -or $last.event -ne 'process.exiting' -or $last.pid -ne $first.pid -or
            $last.seq -ne $rows.Count -or $last.elapsedNs -lt $first.elapsedNs) { throw "Incomplete software log: $file" }
        if ($first.component -eq 'window') {
            $joined = ($rows.event -join ',')
            if ($joined -notmatch 'window\.closing,.*webview\.released,profile\.cleaned,window\.destroyed,process\.exiting' -or
                !($rows | Where-Object { $_.event -eq 'webview.released' -and $_.count -gt 0 -and $_.client -eq $last.client })) {
                throw "Window cleanup not verified: $file"
            }
        }
        $last
    }
    $windows = @($runs | Where-Object { $_.component -eq 'window' -and $_.exitCode -eq 0 })
    if ($windows.Count -ne 3 -or !($runs | Where-Object { $_.component -eq 'host' -and $_.pid -eq $HostPid -and $_.exitCode -eq 0 }) -or
        !($runs | Where-Object { $_.component -eq 'terminal' -and $_.pid -eq $TerminalPid -and $_.exitCode -eq 0 })) {
        throw 'Missing process log for a resident, observer or terminal'
    }
    $accepted = @($records | Where-Object { $_.pid -eq $HostPid -and $_.event -eq 'stop.accepted' })
    $sent = @($records | Where-Object { $_.component -eq 'window' -and $_.event -eq 'stop.submitted' })
    $reply = @($records | Where-Object { $_.component -eq 'window' -and $_.event -eq 'stop.reply' })
    if ($accepted.Count -ne 1 -or $sent.Count -ne 1 -or $reply.Count -ne 1 -or !$sent[0].client -or
        $sent[0].operation -ne ('stop-' + $accepted[0].instance) -or $accepted[0].operation -ne $sent[0].operation -or
        $reply[0].operation -ne $accepted[0].operation -or $reply[0].result -ne 'accepted' -or $null -eq $reply[0].durationMs) {
        throw 'Could not link settings confirmation, host acceptance and window reply'
    }
    $connected = @($records | Where-Object { $_.component -eq 'window' -and $_.event -eq 'client.connected' })
    if (@($connected.client | Sort-Object -Unique).Count -ne 3) { throw 'Missing independent window identities' }
    foreach ($view in $connected) {
        if (!($records | Where-Object { $_.pid -eq $HostPid -and $_.event -eq 'client.connected' -and
            $_.instance -eq $view.instance -and $_.client -eq $view.client })) { throw 'Handshake identities differ' }
    }
    if (@($records | Where-Object { $_.event -eq 'profile.claim' -and $_.result -eq 'busy' -and $_.systemError -in @(32,33) }).Count -ne 2 -or
        !($records | Where-Object { $_.event -eq 'configuration.read' -and $_.result -eq 'failed' }) -or
        (($records | ConvertTo-Json -Depth 3) -match 'synthetic-private-config')) { throw 'Startup errors lack bounded evidence' }
}

# 异常收尾时按本次隔离目录查找仍持有租约的原后台，包括终端冷启动的后继实例。
# 步骤：核对目录租约 → 取得原实例及进程句柄 → 按同号请求停止并等待实际退出。
function Stop-FailedResident([string]$ProfilePath) {
    $lease = Join-Path $ProfilePath '.dafeiyu-runtime.lock'
    $deadline = [DateTime]::UtcNow.AddSeconds(6)
    while (Test-Path -LiteralPath $lease) {
        try {
            $probe = [IO.File]::Open($lease,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
            $probe.Dispose()
            return
        } catch {
            if (($_.Exception.GetBaseException().HResult -band 0xffff) -notin @(32,33)) { throw }
        }
        $state = $null
        try { $state = (& $App --host-status $ProfilePath 2>$null | Out-String) | ConvertFrom-Json } catch {}
        if ($state.instance) {
            $original = [Diagnostics.Process]::GetProcessById([int]$state.pid)
            try {
                $null = $original.Handle
                if ($original.Path -ne (Resolve-Path -LiteralPath $App).Path) { throw 'Unexpected fixture executable' }
                $null = & $App --stop-host $ProfilePath $state.instance
                if (!$original.WaitForExit(6000) -or $original.ExitCode -ne 0) { throw 'Fixture resident did not drain' }
                return
            } finally { $original.Dispose() }
        }
        if ([DateTime]::UtcNow -ge $deadline) { throw "Could not identify resident holding $ProfilePath" }
        Start-Sleep -Milliseconds 40
    }
}

$owner = $null
$blocked = Join-Path $folder 'blocked user'
$complete = $false
try {
    Invoke-Observer @()
    $first = Read-Resident
    $owner = [Diagnostics.Process]::GetProcessById([int]$first.pid)
    $null = $owner.Handle
    $joining = Start-TestRuntime '--chat'
    try {
        $joining.StandardInput.WriteLine('/status')
        $joining.StandardInput.WriteLine('/exit')
        $joining.StandardInput.Close()
        if (!$joining.WaitForExit(6000) -or $joining.ExitCode -ne 0 -or
            !$joining.StandardOutput.ReadToEnd().Contains('No accepted question')) { throw 'Terminal did not join the first resident' }
    } finally { $joining.Dispose() }
    Assert-RuntimeRefused '--host'
    Invoke-Observer @()
    $second = Read-Resident
    if ($second.instance -ne $first.instance -or $second.pid -ne $first.pid) { throw 'Reopening replaced the resident' }
    Invoke-Observer @('stop')
    if (!$owner.WaitForExit(6000) -or $owner.ExitCode -ne 0) { throw 'Resident did not exit cleanly' }
    $terminal = Start-TestRuntime '--chat'
    try {
        $banner = $terminal.StandardOutput.ReadLineAsync()
        if (!$banner.Wait(5000) -or $banner.Result -ne 'DaFeiYu') { throw 'Terminal could not cold-start the resident' }
        $cold = Read-Resident
        Assert-RuntimeRefused '--host'
        $terminal.StandardInput.WriteLine('/exit')
        $terminal.StandardInput.Close()
        if (!$terminal.WaitForExit(5000) -or $terminal.ExitCode -ne 0) { throw 'Terminal failed to exit cleanly' }
        $invalid = Start-TestRuntime '--chat' (Join-Path $folder 'synthetic-private-config.json')
        try {
            $invalid.StandardInput.Close()
            if (!$invalid.WaitForExit(5000) -or $invalid.ExitCode -eq 0) { throw 'Missing configuration accepted' }
        } finally { $invalid.Dispose() }
        $stillRunning = Read-Resident
        if ($cold.instance -ne $stillRunning.instance) { throw 'Leaving terminal lost the original resident' }
        $coldOwner = [Diagnostics.Process]::GetProcessById([int]$cold.pid)
        try {
            $null = $coldOwner.Handle
            & $App --stop-host $data $cold.instance | Out-Null
            if (!$coldOwner.WaitForExit(6000) -or $coldOwner.ExitCode -ne 0) { throw 'Cold resident failed to drain' }
        } finally { $coldOwner.Dispose() }
        Assert-SoftwareLogs $owner.Id $terminal.Id
    } finally {
        if (!$terminal.HasExited) { $terminal.Kill(); $terminal.WaitForExit() }
        $terminal.Dispose()
    }
    Invoke-Observer @()
    $restarted = Read-Resident
    if ($restarted.instance -eq $first.instance) { throw 'Restart reused the previous instance identity' }
    $restartedOwner = [Diagnostics.Process]::GetProcessById([int]$restarted.pid)
    try {
        $null = $restartedOwner.Handle
        $reply = (& $App --stop-host $data $restarted.instance | Out-String) | ConvertFrom-Json
        if ($reply.operation -ne ('stop-' + $restarted.instance) -or !$restartedOwner.WaitForExit(6000) -or
            $restartedOwner.ExitCode -ne 0) { throw 'Terminal stop did not finish on the new instance' }
        $history = @(Get-ChildItem -LiteralPath (Join-Path $data 'logs') -Filter '*.jsonl' -File |
            ForEach-Object { Get-Content -Encoding UTF8 -LiteralPath $_.FullName | ForEach-Object { $_ | ConvertFrom-Json } })
        if (!($history | Where-Object { $_.component -eq 'terminal' -and $_.event -eq 'stop.reply' -and $_.operation -eq $reply.operation }) -or
            !($history | Where-Object { $_.event -eq 'stop.accepted' -and $_.instance -eq $first.instance })) {
            throw 'Restart lost the original shutdown evidence or the terminal receipt'
        }
    } finally {
        if (!$restartedOwner.HasExited) { & $App --stop-host $data $restarted.instance | Out-Null; $null = $restartedOwner.WaitForExit(6000) }
        $restartedOwner.Dispose()
    }
    $null = New-Item -ItemType Directory -Path $blocked
    [IO.File]::WriteAllText((Join-Path $blocked 'logs'),'keep')
    Invoke-Observer @('log-error') $blocked
    $blockedState = Read-Resident $blocked
    $blockedOwner = [Diagnostics.Process]::GetProcessById([int]$blockedState.pid)
    try {
        $null = $blockedOwner.Handle
        & $App --stop-host $blocked $blockedState.instance | Out-Null
        if (!$blockedOwner.WaitForExit(6000) -or $blockedOwner.ExitCode -ne 0) { throw 'Log failure blocked shutdown' }
    } finally { $blockedOwner.Dispose() }
    $complete = $true
} finally {
    if (!$complete) {
        Write-Host "Observer failure evidence: $folder"
        try { Stop-FailedResident $data } finally { Stop-FailedResident $blocked }
    }
    if ($owner) { $owner.Dispose() }
    $created = Get-Item -LiteralPath $folder
    if ($complete -and $created.Parent.FullName -eq $rootPath -and $created.Name -match '^observer-[0-9a-f]{32}$' -and
        !($created.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Remove-Item -LiteralPath $created.FullName -Recurse -Force
    }
}
