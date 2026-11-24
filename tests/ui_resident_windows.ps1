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
function Invoke-Observer([string[]]$ModeArgs) {
    $report = & $Panel --check-observer $config $data @ModeArgs | Out-String
    if ($LASTEXITCODE -ne 0) { throw "Observer failed: $report" }
    $view = $report | ConvertFrom-Json
    if (!$view.profile_cleaned -or !$view.processes_exited -or $view.result -ne 0) { throw "Observer release failed: $report" }
}

# 在同一测试目录核对后台状态，保留进程和实例两个身份。
function Read-Resident {
    $reply = & $App --host-status $data | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'No resident after closing an observer' }
    $state = $reply | ConvertFrom-Json
    if ($state.state -ne 'running') { throw "Unexpected resident state: $reply" }
    return $state
}

$owner = $null
try {
    Invoke-Observer @()
    $first = Read-Resident
    $owner = [Diagnostics.Process]::GetProcessById([int]$first.pid)
    $null = $owner.Handle
    Invoke-Observer @()
    $second = Read-Resident
    if ($second.instance -ne $first.instance -or $second.pid -ne $first.pid) { throw 'Reopening replaced the resident' }
    Invoke-Observer @('stop')
    if (!$owner.WaitForExit(6000) -or $owner.ExitCode -ne 0) { throw 'Resident did not exit cleanly' }
} finally {
    if (!$owner -or !$owner.HasExited) {
        & $App --stop-host $data 2>$null | Out-Null
        if ($owner) { $null = $owner.WaitForExit(6000) }
    }
    if ($owner) { $owner.Dispose() }
    $created = Get-Item -LiteralPath $folder
    if ($created.Parent.FullName -eq $rootPath -and $created.Name -match '^observer-[0-9a-f]{32}$' -and
        !($created.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Remove-Item -LiteralPath $created.FullName -Recurse -Force
    }
}
