#requires -Version 7.0
# 显式验收独立分发程序的真实多轮服务和插件生命周期；不会由离线 CTest 自动调用。
# 步骤：建立隔离目录与进程 → 空插件记忆验收标记 → 加载观察包后核对上下文 → 卸载后再次核对 → 移除并退出。
param(
    [Parameter(Mandatory=$true)][string]$Distribution,
    [Parameter(Mandatory=$true)][string]$Workspace,
    [Parameter(Mandatory=$true)][ValidateSet('deepseek','qwen')][string]$Provider,
    [switch]$RefreshEnvironment
)
$ErrorActionPreference = 'Stop'

# 收集输出直到预期阶段，任何标准错误或提前退出都作为验收失败，不输出凭据。
# 步骤：有界轮询两个输出流 → 保存普通文本 → 核对预期标记 → 检查错误、退出及超时。
function Wait-Output($State, [string]$Expected) {
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($State.OutputTask -and $State.OutputTask.IsCompleted) {
            $line = $State.OutputTask.GetAwaiter().GetResult()
            $State.OutputTask = if ($null -ne $line) { $State.Process.StandardOutput.ReadLineAsync() } else { $null }
            if ($null -ne $line) {
                $State.Transcript.Add("OUT $line")
                if ($line.Contains($Expected,[StringComparison]::Ordinal)) { return }
            }
        }
        if ($State.ErrorTask -and $State.ErrorTask.IsCompleted) {
            $line = $State.ErrorTask.GetAwaiter().GetResult()
            $State.ErrorTask = if ($null -ne $line) { $State.Process.StandardError.ReadLineAsync() } else { $null }
            if ($line) { $State.Transcript.Add("ERR $line"); throw "Application error: $line" }
        }
        if ($State.Process.HasExited -and -not $State.OutputTask) { throw "Application exited before expected stage: $Expected" }
        Start-Sleep -Milliseconds 5
    }
    throw "Timed out waiting for stage: $Expected"
}

# 写入固定验收提示或本地命令，日志只包含这些非敏感输入。
function Send-Line($State, [string]$Line) {
    $State.Transcript.Add("IN $Line")
    $State.Process.StandardInput.WriteLine($Line)
    $State.Process.StandardInput.Flush()
}

# 进程退出后排空剩余输出，防止最后一个成功标记掩盖随后发生的错误。
function Drain-Output($State) {
    while ($State.OutputTask) {
        $line = $State.OutputTask.GetAwaiter().GetResult()
        $State.OutputTask = if ($null -ne $line) { $State.Process.StandardOutput.ReadLineAsync() } else { $null }
        if ($null -ne $line) { $State.Transcript.Add("OUT $line") }
    }
    while ($State.ErrorTask) {
        $line = $State.ErrorTask.GetAwaiter().GetResult()
        $State.ErrorTask = if ($null -ne $line) { $State.Process.StandardError.ReadLineAsync() } else { $null }
        if ($line) { $State.Transcript.Add("ERR $line"); throw "Application error: $line" }
    }
}

$distributionPath = (Resolve-Path -LiteralPath $Distribution).Path
$workspacePath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Workspace)
$runPath = Join-Path $workspacePath ($Provider+'-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runPath) | Out-Null
$program = Join-Path $distributionPath 'dafeiyu.exe'
$configuration = Join-Path $distributionPath ("config/$Provider.example.json")
$example = Join-Path $distributionPath 'examples/plugins/example.observer'
$credential = if ($Provider -eq 'deepseek') { 'DEEPSEEK_API_KEY' } else { 'DASHSCOPE_API_KEY' }
$start = [Diagnostics.ProcessStartInfo]::new($program)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
$start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
$start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
$start.WorkingDirectory = $runPath
$start.Environment['PATH'] = [Environment]::GetFolderPath('Windows')+'\System32'
if ($RefreshEnvironment) {
    $updated = [Environment]::GetEnvironmentVariable($credential,'User')
    if (-not $updated) { $updated = [Environment]::GetEnvironmentVariable($credential,'Machine') }
    if ($updated) { $start.Environment[$credential] = $updated }
}
if (-not $start.Environment[$credential]) { throw "Credential environment variable is missing: $credential" }
foreach ($argument in @('--chat',$configuration,'--plugins',(Join-Path $runPath 'plugins'),'--data',(Join-Path $runPath 'user'))) {
    $start.ArgumentList.Add($argument)
}
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
$state = [pscustomobject]@{ Process=$process; OutputTask=$null; ErrorTask=$null; Transcript=[Collections.Generic.List[string]]::new() }
$success = $false
$launched = $false
try {
    if (-not $process.Start()) { throw 'Application did not start.' }
    $launched = $true
    $state.OutputTask = $process.StandardOutput.ReadLineAsync()
    $state.ErrorTask = $process.StandardError.ReadLineAsync()
    Wait-Output $state 'Local commands remain available'
    Send-Line $state '/plugins list'
    Wait-Output $state 'No plugins installed or loaded.'
    $marker = 'DFYQA-'+[Guid]::NewGuid().ToString('N').Substring(0,12)
    Send-Line $state "请记住验收标记 $marker。这轮只回复 READY。"
    Wait-Output $state 'READY'
    Write-Output "${Provider}: empty-plugin answer passed."
    Send-Line $state ('/plugins install "'+$example+'"')
    Wait-Output $state 'Installed example.observer.'
    Send-Line $state '/plugins load example.observer'
    Wait-Output $state 'Loaded example.observer.'
    Send-Line $state '我之前让你记住的验收标记是什么？请只回复那个标记。'
    Wait-Output $state $marker
    Write-Output "${Provider}: loaded-observer context passed."
    Send-Line $state '/plugins unload example.observer'
    Wait-Output $state 'Unloaded example.observer.'
    Send-Line $state '再次回复先前的验收标记，不添加其他文字。'
    Wait-Output $state $marker
    Write-Output "${Provider}: post-unload context passed."
    Send-Line $state '/plugins remove example.observer'
    Wait-Output $state 'Removed example.observer.'
    Send-Line $state '/plugins list'
    Wait-Output $state 'No plugins installed or loaded.'
    Send-Line $state '/exit'
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(10000)) { throw 'Application did not finish shutdown.' }
    Drain-Output $state
    if ($process.ExitCode -ne 0) { throw "Application exit code: $($process.ExitCode)" }
    $success = $true
    [pscustomobject]@{ provider=$Provider; empty_plugin=$true; loaded_observer=$true; context_after_unload=$true; removed=$true; exit_code=0; workspace=$runPath } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runPath 'result.json') -Encoding utf8
    Write-Output "${Provider}: live lifecycle passed; result saved in $runPath"
} finally {
    if ($launched -and -not $process.HasExited) {
        try { $process.StandardInput.WriteLine('/exit'); $process.StandardInput.Flush() } catch {}
        if (-not $process.WaitForExit(5000)) { $process.Kill($true); $process.WaitForExit() }
    }
    [IO.File]::WriteAllLines((Join-Path $runPath 'transcript.txt'),$state.Transcript,[Text.UTF8Encoding]::new($false))
    $process.Dispose()
}
if (-not $success) { exit 1 }
