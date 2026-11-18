# 在独立 Release 目录中验证本地配置和状态面板的真实安装与卸载。
# 步骤：新建安装目录 → 隔离用户设置和外部运行库路径 → 执行本地命令 → 检查命令输出及保留的设置。
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

# 从安装目录外启动正式程序，只发送明确的本地命令并保存进程证据。
# 步骤：配置隔离进程及 UTF-8 管道 → 送入命令并关闭输入 → 验证实际退出并记录输出。
function Invoke-PackagedAgent([string]$Name, [string[]]$Arguments, [string[]]$Commands = @()) {
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
$output = Invoke-PackagedAgent 'status-panel' @('--chat',(Join-Path $package 'config/deepseek.example.json'),'--data',$data) @(
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
Write-Output "Agent Release package verified. Logs: $check"
