param([string]$App, [string]$Fixture, [string]$Root, [string]$Program, [string]$Panel, [string]$Tools, [string]$Corpus)
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
    $info.EnvironmentVariables.Remove('DEEPSEEK_API_KEY')
    $info.EnvironmentVariables.Remove('DASHSCOPE_API_KEY')
    $info.EnvironmentVariables.Remove('DFY_TERMINAL_OFFLINE_FIXTURE')
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    return [Diagnostics.Process]::Start($info)
}

# 用实际管道 EOF 等待本终端发起的回答，核对退出码和对应的输出通道。
function Invoke-Lines([string[]]$Options, [string[]]$Lines, [string]$Expected, [bool]$InError = $false, [switch]$Capture) {
    $process = Start-Cli $App $Options
    try {
        foreach ($line in $Lines) { $process.StandardInput.WriteLine($line) }
        $process.StandardInput.Close()
        if (!$process.WaitForExit(7000)) { throw 'Terminal input remained blocked' }
        $outText = $process.StandardOutput.ReadToEnd()
        $errText = $process.StandardError.ReadToEnd()
        $actual = if ($InError) { $errText } else { $outText }
        if ($process.ExitCode -ne 0 -or !$actual.Contains($Expected)) { throw "Terminal returned $($process.ExitCode). $outText $errText" }
        if ($Capture) { return $actual }
    } finally {
        if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}

# 从正式终端的成功输出取一条管理回执。
function Get-TerminalJson([string[]]$Options, [string]$Command) {
    $result = Invoke-Lines $Options @($Command) '{"' -Capture
    return ($result -split '[\r\n]+' | Where-Object { $_.StartsWith('{"') } | Select-Object -Last 1) | ConvertFrom-Json
}

# 等同一档案的管道报告指定替身实例已就绪。
function Wait-TestHost([Diagnostics.Process]$Process, [string]$ProfilePath) {
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        try { $status = (& $App --host-status $ProfilePath 2>$null | Out-String) | ConvertFrom-Json } catch { $status = $null }
        if ($status.state -eq 'running' -and $status.pid -eq $Process.Id) { return }
        if ($Process.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Fixed host unavailable' }
        Start-Sleep -Milliseconds 30
    } while ($true)
}

# 逐字节计算夹具快照或导入原件的指纹。
function Get-TestFileDigest([string]$Path) {
    $hash = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','') }
    finally { $stream.Dispose(); $hash.Dispose() }
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

# 真实终端应在最终模型调用仍等待时看到显式发言，随后只收到一次同文回答。
# 步骤：提交后关闭管道输入 → 在最终文本放行前读到追问 → 允许结算并核对剩余输出。
function Test-EarlyReply([string[]]$Options, [string]$ProfilePath) {
    $process = Start-Cli $App $Options
    $gate = Join-Path $ProfilePath 'allow-final'
    try {
        $process.StandardInput.WriteLine('early-reply')
        $process.StandardInput.Close()
        $deadline = [DateTime]::UtcNow.AddSeconds(6)
        do {
            $line = $process.StandardOutput.ReadLineAsync()
            if (!$line.Wait(5000) -or $null -eq $line.Result -or [DateTime]::UtcNow -gt $deadline) { throw 'Explicit question missing before final answer' }
        } while ($line.Result -ne 'early question')
        [IO.File]::WriteAllText($gate,'release')
        if (!$process.WaitForExit(6000)) { throw 'Terminal did not exit after the final answer' }
        $remaining = $process.StandardOutput.ReadToEnd()
        $errors = $process.StandardError.ReadToEnd()
        if ($process.ExitCode -ne 0 -or $remaining.Contains('early question') -or $errors) {
            throw "Early reply failed with exit $($process.ExitCode). Remaining stdout [$remaining]. Stderr [$errors]"
        }
    } finally {
        [IO.File]::WriteAllText($gate,'release')
        if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}

$default = @('--data',$data,'--plugins',$plugins)
try {
    Invoke-Lines $default @('/status','/exit') 'No accepted question'
    Invoke-Lines $default @('/memory encoder','/exit') 'disabled'
    Invoke-Lines $default @('/resume ffffffffffffffffffffffffffffffff/task/1 "Check the current state"','/exit') 'dialogue: stale' $true
    $owner = (& $App --host-status $data | Out-String) | ConvertFrom-Json
    try { $null = & $App --stop-host $data 'previous-instance' 2>$null } catch {}
    if ($LASTEXITCODE -eq 0) { throw 'A stale shutdown target was accepted' }
    $bundled = Join-Path (Split-Path -Parent $App) 'config/deepseek.example.json'
    Invoke-Lines (@('--chat',$bundled) + $default) @('/plugins list','/exit') 'No plugins installed'
    $same = (& $App --host-status $data | Out-String) | ConvertFrom-Json
    if ($owner.instance -ne $same.instance) { throw 'Default chat and --chat selected different hosts' }
} finally { Stop-TestHost $data }

$data = Join-Path $folder 'fixed profile'
$original = Start-Cli $Fixture @($config,$plugins,$data)
$options = @('--chat',$config,'--data',$data,'--plugins',$plugins)
try {
    Wait-TestHost $original $data
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
    Test-EarlyReply $options $data
    Invoke-Lines $options @('fail') 'fixed model failure' $true
    Invoke-Lines $options @('/status','/unknown','/status') 'Unknown command' $true
} finally {
    if (!$original.HasExited) { Stop-TestHost $data }
    $original.Dispose()
}

# 从原任务已退出的 SQLite 副本启动正式终端，核对原件、现场读回和交付。
# 步骤：结算两条同名任务 → 复制快照并启动另一进程 → CLI 按完整号接续 → 对照两次实际请求。
$seed = Join-Path $folder 'task proof'
$data = Join-Path $folder 'resumed profile'
$trace = Join-Path $folder 'task trace'
& $Program $Panel $Tools $Corpus $seed '--resume-desktop-seed'
if ($LASTEXITCODE -ne 0) { throw 'Could not settle both task fixtures' }
$null = New-Item -ItemType Directory -Path $data
$source = Join-Path $seed 'seed/main-conversation.sqlite3'
$copy = Join-Path $data 'main-conversation.sqlite3'
Copy-Item -LiteralPath $source -Destination $copy
$digest = Get-TestFileDigest $source
if ((Get-TestFileDigest $copy) -ne $digest) { throw 'Terminal received a different task snapshot' }
$selected = [IO.File]::ReadAllText((Join-Path $seed 'task.json')) | ConvertFrom-Json
$original = Start-Cli $Fixture @($config,$plugins,$data,$trace)
$options = @('--chat',$config,'--data',$data,'--plugins',$plugins,'--trace',$trace)
try {
    Wait-TestHost $original $data
    Invoke-Lines $options @(('/plugins install "{0}"' -f $Panel),'/plugins load example.status') 'Loaded example.status.'
    Invoke-Lines $options @(('/resume {0} "Check the current T03 reading"' -f $selected.foreignReference)) 'dialogue: stale' $true
    Invoke-Lines $options @(('/resume {0} "Check the current T03 reading"' -f $selected.reference)) 'T03 checked in this run'
} finally {
    if (!$original.HasExited) { Stop-TestHost $data }
    $original.Dispose()
}
$events = @(Get-Content -LiteralPath (Join-Path $trace 'trace/events.jsonl') -Encoding UTF8 |
    ForEach-Object { ConvertFrom-Json -InputObject $_ })
$sent = @($events | Where-Object { $_.kind -eq 'model.prepared' -and $_.source -eq 'main' })
$work = @($events | Where-Object { $_.kind -eq 'tool.exited' -and $_.source -eq 'main' })
if ($sent.Count -ne 2 -or $work.Count -ne 1) { throw 'Expected two requests and just one new work receipt' }
$before = ($sent[0].input.messages[-1].content | ConvertFrom-Json).items
$after = ($sent[1].input.messages[-1].content | ConvertFrom-Json).items
$past = ($before | Where-Object key -eq 'memory.tasks').value.items
$pending = ($before | Where-Object key -eq 'task.resume').value
$current = ($after | Where-Object key -eq 'task.resume').value
if ($past.Count -ne 1 -or $past[0].reference -ne $selected.reference -or $past[0].actionRef -ne $selected.actionRef -or
    $past[0].receiptRef -ne $selected.receiptRef -or $past[0].readbackRef -ne $selected.readbackRef -or $past[0].verification -ne 'contradicted' -or
    $pending.state -ne 'awaiting_readback' -or $pending.PSObject.Properties['current']) { throw 'First request did not carry the exact historical T03' }
if ($current.state -ne 'satisfied' -or $current.current -ne 'idle' -or $current.expected -ne 'idle' -or
    $current.candidate -ne $work[0].candidate -or $work[0].toolId -ne 'example.status/read' -or
    $work[0].effect -ne 'not_applied') { throw 'Second request did not carry the actual current readback' }
Write-Output ("CLI T03 snapshot {0}; historical=1 actual=2 result=satisfied wireBytes={1},{2}" -f $digest,$sent[0].wireBytes,$sent[1].wireBytes)

# 用正式终端登记外部资料，重启后沿版本号查回并核对撤下。
# 步骤：导入字节并发布出处 → 改写夹具文件重启本体 → 查回原字节 → 核对管理回执与撤权。
$data = Join-Path $folder 'knowledge profile'
$options = @('--chat',$config,'--data',$data,'--plugins',$plugins)
$document = Join-Path $folder 'museum source.txt'
$body = '馆藏交接样本。' + [Environment]::NewLine + '实物核对后存档。' + [char]10
[IO.File]::WriteAllText($document,$body,[Text.UTF8Encoding]::new($false))
$original = Start-Cli $Fixture @($config,$plugins,$data)
try {
    Wait-TestHost $original $data
    $library = (Get-TerminalJson $options '/knowledge define-library manuals external "Intake guides" "Museum owners" "handoff"').libraries[0]
    if ($library.category -ne 'external' -or $library.origin -ne 'Museum owners' -or $library.tags[0] -ne 'handoff') {
        throw 'Terminal lost the library metadata'
    }
    $imported = Get-TerminalJson $options ('/knowledge import guide v1 "Intake guide" "Museum v1" "{0}"' -f $document)
    if ($imported.sha256 -ne (Get-TestFileDigest $document) -or $imported.bytes -ne [Text.Encoding]::UTF8.GetByteCount($body)) {
        throw 'Terminal did not register the original bytes'
    }
    $recorded = Get-TerminalJson $options ('/knowledge propose-in manuals external handoff v1 0 "Handoff" "Intake" "See the intake guide." {0}' -f $imported.reference)
    $published = Get-TerminalJson $options '/knowledge publish handoff v1'
    if ($recorded.state -ne 'recorded' -or !$published.editions[0].current) { throw 'Terminal did not publish the external edition' }
} finally {
    if (!$original.HasExited) { Stop-TestHost $data }
    $original.Dispose()
}
[IO.File]::WriteAllText($document,'replacement')
$original = Start-Cli $Fixture @($config,$plugins,$data)
try {
    Wait-TestHost $original $data
    $source = Get-TerminalJson $options ('/knowledge source {0}' -f $imported.reference)
    if ($source.text -cne $body -or $source.origin -ne 'Museum v1' -or $source.sha256 -ne $imported.sha256) {
        throw 'Restart did not recover the original external bytes'
    }
    $withdrawn = Get-TerminalJson $options ('/knowledge withdraw-source {0}' -f $imported.reference)
    $retired = (Get-TerminalJson $options '/knowledge inspect handoff v1').editions[0]
    $overview = (Get-TerminalJson $options '/knowledge inspect handoff').editions[0]
    if ($withdrawn.state -ne 'withdrawn' -or $retired.current -or !$retired.evidenceUnavailable -or
        $overview.current -or !$overview.evidenceUnavailable -or $retired.PSObject.Properties['sources']) {
        throw 'Terminal still showed an edition with withdrawn evidence as current'
    }
    Invoke-Lines $options @(('/knowledge source {0}' -f $imported.reference)) 'knowledge: unavailable document' $true
} finally {
    if (!$original.HasExited) { Stop-TestHost $data }
    $original.Dispose()
}
Write-Output ("CLI knowledge restarted and withdrew UTF-8 source, bytes={0}" -f $imported.bytes)
