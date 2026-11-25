param([string]$App, [string]$Fixture, [string]$Root)
$ErrorActionPreference = 'Stop'
$folder = Join-Path $Root ([guid]::NewGuid().ToString('N'))
$plugins = Join-Path $folder 'empty packages'
$null = New-Item -ItemType Directory -Force -Path $plugins
$data = Join-Path $folder 'cold profile'
$config = Join-Path $folder 'fixture.json'
[IO.File]::WriteAllText($config,'{"provider":"deepseek","base_url":"https://offline.invalid","model":"fixture","api_key_env":"DFY_TERMINAL_OFFLINE_FIXTURE"}')

# 在独立进程使用正式的输入与输出管道，参数均来自隔离的测试目录。
function Start-Cli([string]$Program, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Program
    $info.Arguments = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    return [Diagnostics.Process]::Start($info)
}

# 用实际管道 EOF 等待本终端发起的回答，核对退出码和对应的输出通道。
function Invoke-Lines([string[]]$Options, [string[]]$Lines, [string]$Expected, [bool]$InError = $false) {
    $process = Start-Cli $App $Options
    try {
        foreach ($line in $Lines) { $process.StandardInput.WriteLine($line) }
        $process.StandardInput.Close()
        if (!$process.WaitForExit(7000)) { throw 'Terminal input remained blocked' }
        $outText = $process.StandardOutput.ReadToEnd()
        $errText = $process.StandardError.ReadToEnd()
        $actual = if ($InError) { $errText } else { $outText }
        if ($process.ExitCode -ne 0 -or !$actual.Contains($Expected)) { throw "Terminal returned $($process.ExitCode). $outText $errText" }
    } finally {
        if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}

# 只对测试目录的原进程等待退出，测试异常时也释放该实例。
function Stop-TestHost([string]$ProfilePath) {
    $status = (& $App --host-status $ProfilePath | Out-String) | ConvertFrom-Json
    $process = [Diagnostics.Process]::GetProcessById([int]$status.pid)
    try {
        $null = $process.Handle
        & $App --stop-host $ProfilePath | Out-Null
        if (!$process.WaitForExit(6000) -or $process.ExitCode -ne 0) { throw 'Test host failed to drain' }
    } finally { $process.Dispose() }
}

$default = @('--data',$data,'--plugins',$plugins)
try {
    Invoke-Lines $default @('/status','/exit') 'No accepted question'
    $owner = (& $App --host-status $data | Out-String) | ConvertFrom-Json
    $bundled = Join-Path (Split-Path -Parent $App) 'config/deepseek.example.json'
    Invoke-Lines (@('--chat',$bundled) + $default) @('/plugins list','/exit') 'No plugins installed'
    $same = (& $App --host-status $data | Out-String) | ConvertFrom-Json
    if ($owner.instance -ne $same.instance) { throw 'Default chat and --chat selected different hosts' }
} finally { Stop-TestHost $data }

$data = Join-Path $folder 'fixed profile'
$original = Start-Cli $Fixture @($config,$plugins,$data)
$options = @('--chat',$config,'--data',$data,'--plugins',$plugins)
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        try { $status = (& $App --host-status $data 2>$null | Out-String) | ConvertFrom-Json } catch { $status = $null }
        if ($status.state -eq 'running' -and $status.pid -eq $original.Id) { break }
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Fixed host unavailable' }
        Start-Sleep -Milliseconds 30
    } while ($true)
    Invoke-Lines $options @('delayed',"/exit`r`r") 'DaFeiYu'
    Invoke-Lines $options @('/status','/exit') 'Question=pending'
    [IO.File]::WriteAllText((Join-Path $data 'release'),'release')
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $state = '/status' | & $App @options | Out-String
        if ($state.Contains('Question=succeeded')) { break }
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Detached work did not succeed' }
    } while ($true)
    Invoke-Lines $options @('again') 'fixed:again'
    Invoke-Lines $options @('fail') 'fixed model failure' $true
    Invoke-Lines $options @('/status','/unknown','/status') 'Unknown command' $true
} finally {
    if (!$original.HasExited) { Stop-TestHost $data }
    $original.Dispose()
}
