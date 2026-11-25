# 在独立 Release 目录中验证本地配置和状态面板的真实安装与卸载。
# 步骤：隔离安装和设置目录 → 启动并绑定原后台 → 核对终端命令及保留的设置 → 等待原进程退出。
param([Parameter(Mandatory)][string]$Build)
$ErrorActionPreference = 'Stop'
$buildPath = (Resolve-Path -LiteralPath $Build).Path
$root = Split-Path -Parent $PSScriptRoot
$check = Join-Path $root ('build/agent-package-check/' + [guid]::NewGuid().ToString('N'))
$package = Join-Path $check 'package'
$data = Join-Path $check 'user'
cmake --install $buildPath --config Release --prefix $package
if ($LASTEXITCODE) { throw "Release installation failed: $LASTEXITCODE" }

$plugins = Join-Path $package 'plugins'
$sample = Join-Path $package 'examples/plugins/example.status'
$executable = Join-Path $package 'dafeiyu.exe'
if (!(Test-Path -LiteralPath $plugins -PathType Container) -or @(Get-ChildItem -LiteralPath $plugins -Force).Count) {
    throw 'Release plugins directory must start empty'
}
foreach ($file in @($executable,(Join-Path $sample 'plugin.json'),(Join-Path $sample 'dfy_status_example.dll'),
    (Join-Path $package 'sdk/examples/status/CMakeLists.txt'))) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Release file is missing: $file" }
}
$settings = Join-Path $data 'plugins/example.status/settings.json'
New-Item -ItemType Directory -Path (Split-Path -Parent $settings) -Force | Out-Null
$settingsText = '{"test_tag":"agent-package"}'
Set-Content -LiteralPath $settings -Value $settingsText -Encoding utf8NoBOM -NoNewline

# 配置隔离的安装包进程与 UTF-8 管道，宿主及终端使用相同的受控环境。
# 步骤：固定工作目录和管道编码 → 隔离运行库与模型环境 → 绑定原始参数。
function New-PackagedProcess([string[]]$Arguments) {
    $process = [System.Diagnostics.Process]::new()
    $info = $process.StartInfo
    $info.FileName = $executable
    $info.WorkingDirectory = $check
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $encoding = [System.Text.UTF8Encoding]::new($false)
    $info.StandardInputEncoding = $encoding
    $info.StandardOutputEncoding = $encoding
    $info.StandardErrorEncoding = $encoding
    $info.Environment['PATH'] = Join-Path $env:SystemRoot 'System32'
    [void]$info.Environment.Remove('DEEPSEEK_API_KEY')
    [void]$info.Environment.Remove('DASHSCOPE_API_KEY')
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    return $process
}

# 从安装目录外运行正式程序，只发送本地命令并保存其退出证据。
# 步骤：启动并读取输出 → 送入命令 → 等待原进程并核对结果。
function Invoke-PackagedAgent([string]$Name, [string[]]$Arguments, [string[]]$Commands = @()) {
    $process = New-PackagedProcess $Arguments
    try {
        if (!$process.Start()) { throw "Cannot start packaged Agent: $Name" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        foreach ($command in $Commands) { $process.StandardInput.WriteLine($command) }
        $process.StandardInput.Close()
        if (!$process.WaitForExit(15000)) { $process.Kill(); $process.WaitForExit(); throw "Packaged Agent timed out: $Name" }
        $output = $stdout.GetAwaiter().GetResult()
        $errors = $stderr.GetAwaiter().GetResult()
        Set-Content -LiteralPath (Join-Path $check "$Name.log") -Value $output -Encoding utf8NoBOM -NoNewline
        Set-Content -LiteralPath (Join-Path $check "$Name.err") -Value $errors -Encoding utf8NoBOM -NoNewline
        if ($process.ExitCode -ne 0 -or $errors.Trim().Length) { throw "Packaged Agent failed in $Name. See $check" }
        return $output
    } finally { $process.Dispose() }
}

foreach ($provider in @('deepseek','qwen')) {
    $configuration = Join-Path $package "config/$provider.example.json"
    $output = Invoke-PackagedAgent $provider @('--check-config',$configuration)
    if (!$output.Contains("Configuration valid: $provider /")) { throw "Unexpected configuration receipt: $provider" }
}
# 从独占测试目录核对刚启动的原进程，只有同一 PID 才能提供本次退出身份。
# 步骤：轮询原进程和后台状态 → 比对进程号 → 返回就绪实例或报告超时。
function Get-PackagedInstance([Diagnostics.Process]$Owner) {
    $end = [DateTime]::UtcNow.AddSeconds(8)
    do {
        if ($Owner.HasExited) { throw "Packaged host exited during startup ($($Owner.ExitCode))" }
        $json = & $executable --host-status $data 2>$null
        if ($LASTEXITCODE -eq 0) {
            $status = $json | ConvertFrom-Json
            if ($status.pid -ne $Owner.Id) { throw 'Isolated profile has a different resident' }
            if ($status.state -eq 'running') { return $status.instance }
        }
        Start-Sleep -Milliseconds 40
    } while ([DateTime]::UtcNow -lt $end)
    throw 'Packaged host did not become ready'
}

$configuration = Join-Path $package 'config/deepseek.example.json'
$owner = New-PackagedProcess @('--host',$configuration,'--data',$data)
$started = $false
$instance = $null
try {
    $started = $owner.Start()
    if (!$started) { throw 'Cannot start isolated packaged host' }
    $null = $owner.Handle
    $hostOutput = $owner.StandardOutput.ReadToEndAsync()
    $hostErrors = $owner.StandardError.ReadToEndAsync()
    $owner.StandardInput.Close()
    $instance = Get-PackagedInstance $owner
    $output = Invoke-PackagedAgent 'status-panel' @('--chat',$configuration,'--data',$data) @(
        '/plugins list',('/plugins install "' + $sample + '"'),'/plugins load example.status','/plugins list',
        '/plugins unload example.status','/plugins remove example.status','/plugins list','/exit')
    foreach ($line in @('Installed example.status.','Loaded example.status.','example.status [running]','Unloaded example.status.',
        'Removed example.status. User settings kept.')) {
        if (!$output.Contains($line)) { throw "Status panel receipt missing: $line" }
    }
    if ([regex]::Matches($output,'No plugins installed or loaded\.').Count -ne 2 -or @(Get-ChildItem -LiteralPath $plugins -Force).Count) {
        throw 'Installed package remains visible after removal'
    }
    if ((Get-Content -LiteralPath $settings -Raw) -cne $settingsText) { throw 'Removal changed existing settings' }
} finally {
    try {
        if ($started) {
            if (!$instance -and !$owner.HasExited) { $instance = Get-PackagedInstance $owner }
            if (!$owner.HasExited) {
                $reply = Invoke-PackagedAgent 'stop-original' @('--stop-host',$data,$instance) | ConvertFrom-Json
                if ($reply.instance -ne $instance -or $reply.operation -ne "stop-$instance") { throw 'Wrong host stop receipt' }
            }
            if (!$owner.WaitForExit(12000) -or $owner.ExitCode -ne 0) { throw 'Packaged host did not exit normally' }
            Set-Content -LiteralPath (Join-Path $check 'resident.log') -Value $hostOutput.GetAwaiter().GetResult() -Encoding utf8NoBOM -NoNewline
            Set-Content -LiteralPath (Join-Path $check 'resident.err') -Value $hostErrors.GetAwaiter().GetResult() -Encoding utf8NoBOM -NoNewline
        }
    } finally {
        if ($started -and !$owner.HasExited) { $owner.Kill(); $owner.WaitForExit() }
        $owner.Dispose()
    }
}
Write-Output "Agent Release package verified. Logs: $check"
