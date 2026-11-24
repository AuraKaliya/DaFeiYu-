# 准备上游浏览器客户端并打开本机体验服；试玩的下载、缓存和浏览器资料均放在指定目录。
[CmdletBinding()]
param([Parameter(Mandatory)][string]$FixtureRoot, [string]$ModelConfig = $env:DFY_MC_MODEL_CONFIG,
    [string]$BuildDirectory, [string]$Configuration = 'Debug', [switch]$PrepareOnly,
    [ValidateSet('inherit','off','low')][string]$PlayerThinking)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $FixtureRoot).Path
$dir = Join-Path $root 'mwc'
$env:TEMP = $env:TMP = Join-Path $root 'tmp'
$env:npm_config_cache = Join-Path $root 'npm-cache'
$env:DFY_MC_TEST_ROOT = $root
$env:NODE_PATH = Join-Path $dir 'relay/node_modules'
$relay = Join-Path $dir 'relay'
[void](New-Item -ItemType Directory -Force -Path $relay)

$archive = Join-Path $dir 'self-host-v2.4.0.zip'
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -UseBasicParsing 'https://github.com/zardoy/minecraft-web-client/releases/download/v2.4.0/self-host.zip' -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive).Hash -ne '6580af1f2850b2e492998520a3ee66f97142d1ac62b2ea125b6f3be121958f4f') {
    throw 'WebMC 发布包校验不符'
}
if (-not (Test-Path -LiteralPath (Join-Path $dir '2.4.0/dist/index.html'))) {
    Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $dir '2.4.0') -Force
}
$lock = Join-Path $PSScriptRoot 'package-lock.json'
$stamp = Join-Path $relay 'installed-lock.txt'
$digest = (Get-FileHash -LiteralPath $lock).Hash
if (-not (Test-Path -LiteralPath $stamp) -or (Get-Content -LiteralPath $stamp -Raw).Trim() -ne $digest -or
    -not (Test-Path -LiteralPath (Join-Path $env:NODE_PATH 'ws/package.json'))) {
    Copy-Item -LiteralPath $lock,(Join-Path $PSScriptRoot 'package.json') -Destination $relay
    & npm.cmd ci --prefix $relay --no-audit --no-fund --ignore-scripts
    if ($LASTEXITCODE) { throw '本机连接依赖安装失败' }
    Set-Content -LiteralPath $stamp -Value $digest
}

# 核对本机 TCP 监听是否存在，避免重复启动体验进程。
function Test-Listening([int]$Port) {
    $client = [Net.Sockets.TcpClient]::new()
    try { $client.Connect('127.0.0.1', $Port); return $true }
    catch { return $false }
    finally { $client.Dispose() }
}

$stampTime = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$build = if ($BuildDirectory) { (Resolve-Path -LiteralPath $BuildDirectory).Path }
if (Test-Listening 25580) {
    $info = Join-Path $root 'servers/playtest-1.21.4/dfy-launch.json'
    if (-not (Test-Path -LiteralPath $info)) { throw '该端口已有旧体验服或其他服务，结束该场后可开启模型伙伴。' }
    $active = Get-Content -LiteralPath $info -Raw | ConvertFrom-Json
    $activeThinking = if ($active.playerThinking) { $active.playerThinking } else { 'inherit' }
    if ($PlayerThinking -and $activeThinking -ne $PlayerThinking) { throw '当前体验服使用另一玩家思考档位，结束该场后重启即可切换。' }
    if ($active.mode -ne '--live' -or ($ModelConfig -and $active.config -ne (Resolve-Path -LiteralPath $ModelConfig).Path)) {
        throw '当前体验服使用另一种模式或模型配置，结束该场后重启即可切换。'
    }
    if (($build -and $active.buildDirectory -ne $build) -or
        (($build -or $PSBoundParameters.ContainsKey('Configuration')) -and $active.configuration -ne $Configuration)) {
        throw '当前体验服使用另一构建，结束该场后可启动指定版本。'
    }
} else {
    $paper = Join-Path (Split-Path $PSScriptRoot) 'start-playtest.ps1'
    $out = Join-Path $dir "$stampTime-paper.log"
    $err = Join-Path $dir "$stampTime-paper.err"
    $arguments = @('-NoProfile','-File',('"' + $paper + '"'),'-FixtureRoot',('"' + $root + '"'))
    if ($build) { $arguments += @('-BuildDirectory',('"' + $build + '"')) }
    $arguments += @('-Configuration',('"' + $Configuration + '"'))
    if ($ModelConfig) { $arguments += @('-ModelConfig',('"' + (Resolve-Path -LiteralPath $ModelConfig).Path + '"')) }
    if ($PlayerThinking) { $arguments += @('-PlayerThinking',$PlayerThinking) }
    $job = Start-Process -FilePath (Get-Process -Id $PID).Path -WindowStyle Hidden -PassThru -ArgumentList $arguments -RedirectStandardOutput $out -RedirectStandardError $err
    $until = (Get-Date).AddSeconds(90)
    while (-not (Select-String -Path $out -SimpleMatch '127.0.0.1:25580' -Quiet -ErrorAction SilentlyContinue)) {
        if ($job.HasExited -or (Get-Date) -gt $until) { throw "体验服未就绪，详见 $err 和 $out" }
        Start-Sleep -Milliseconds 350
    }
}
if (-not (Test-Listening 25582)) {
    $err = Join-Path $dir "$stampTime-web.err"
    $job = Start-Process -FilePath (Get-Command node).Source -WindowStyle Hidden -PassThru -ArgumentList @(
        ('"' + (Join-Path $PSScriptRoot 'serve.cjs') + '"')) -RedirectStandardOutput (Join-Path $dir "$stampTime-web.log") -RedirectStandardError $err
    $until = (Get-Date).AddSeconds(10)
    while (-not (Test-Listening 25582)) {
        if ($job.HasExited -or (Get-Date) -gt $until) { throw "浏览器入口未就绪，详见 $err" }
        Start-Sleep -Milliseconds 200
    }
}
$url = 'http://127.0.0.1:25582'
if ((Invoke-RestMethod "$url/health").service -ne 'dafeiyu-webmc') { throw '浏览器端口未返回本项目的就绪信息' }
if (-not $PrepareOnly) {
    $edge = Join-Path ${env:ProgramFiles(x86)} 'Microsoft/Edge/Application/msedge.exe'
    Start-Process -FilePath $edge -ArgumentList @(('--user-data-dir="' + (Join-Path $dir 'player-profile') + '"'),'--no-first-run',$url)
}
Write-Output $url
