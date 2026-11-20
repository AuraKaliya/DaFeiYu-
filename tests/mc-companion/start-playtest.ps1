# 在已接受协议的本机测试环境旁启动独立的真人体验世界。
[CmdletBinding()]
param([string]$FixtureRoot = $env:DFY_MC_TEST_ROOT, [string]$BuildDirectory, [string]$Configuration = 'Debug',
    [string]$ModelConfig = $env:DFY_MC_MODEL_CONFIG, [switch]$Probe, [switch]$PartnerProbe)
$ErrorActionPreference = 'Stop'
if (-not $FixtureRoot -or -not [IO.Path]::IsPathRooted($FixtureRoot)) { throw '请给出已准备好的测试根目录' }
$root = (Resolve-Path -LiteralPath $FixtureRoot).Path
$baseServer = Join-Path $root 'servers/stone-1.21.4'
if ((Get-Content -LiteralPath (Join-Path $baseServer 'eula.txt')) -notcontains 'eula=true') { throw '本项目本机测试服尚未接受 Minecraft EULA' }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root 'native' }
if ($Probe -and $PartnerProbe) { throw '请选择一种验收场景' }
$exe = if ($Probe -or $PartnerProbe) { Join-Path $BuildDirectory "tests/$Configuration/dfy_mc_playtest.exe" } else {
    Join-Path $BuildDirectory "bin/$Configuration/dafeiyu.exe"
}
$package = Join-Path $BuildDirectory "plugins/mc/$Configuration/mc.companion"
if (-not (Test-Path -LiteralPath $exe)) { throw "请先构建游戏宿主：$exe" }
if (-not ($Probe -or $PartnerProbe)) {
    if (-not $ModelConfig -and (Test-Path -LiteralPath (Join-Path $root 'partner-model.json'))) { $ModelConfig = Join-Path $root 'partner-model.json' }
    if (-not $ModelConfig) { $ModelConfig = Join-Path (Split-Path $exe) 'config/deepseek.example.json' }
    $ModelConfig = (Resolve-Path -LiteralPath $ModelConfig).Path
    & $exe --check-config $ModelConfig
    if ($LASTEXITCODE) { throw '模型配置校验失败' }
}
$env:DFY_MC_TEST_ROOT = $root
if (-not $env:DFY_MC_JAVA) { $env:DFY_MC_JAVA = (Get-ChildItem "$root/jdk/*/bin/java.exe" | Select-Object -First 1).FullName }
if (-not $env:DFY_MC_PAPER_JAR) { $env:DFY_MC_PAPER_JAR = Join-Path $root 'downloads/paper-1.21.4-232.jar' }
$env:NODE_PATH = Join-Path $root 'bridge/node_modules'
$env:TEMP = $env:TMP = Join-Path $root 'tmp'
$env:JAVA_TOOL_OPTIONS = '"-Duser.home=' + (Join-Path $root 'profile') + '" "-Djava.io.tmpdir=' + $env:TEMP + '"'

$server = Join-Path $root 'servers/playtest-1.21.4'
$marker = Join-Path $server 'dfy-local-playtest'
if ((Test-Path -LiteralPath $server) -and -not (Test-Path -LiteralPath $marker)) { throw "已有同名目录：$server" }
[void](New-Item -ItemType Directory -Force -Path $server)
$lock = [IO.File]::Open($marker, 'OpenOrCreate', 'ReadWrite', 'None')
$launchInfo = Join-Path $server 'dfy-launch.json'
try {
    # 复用已准备好的同版本服务器分发文件；世界、玩家数据和插件运行记录都留在体验目录。
    foreach ($part in @('cache', 'versions', 'libraries')) {
        if (-not (Test-Path -LiteralPath (Join-Path $server $part))) {
            Copy-Item -LiteralPath (Join-Path $baseServer $part) -Destination $server -Recurse
        }
    }
    Copy-Item -LiteralPath (Join-Path $baseServer 'eula.txt') -Destination $server
    [void](New-Item -ItemType Directory -Force -Path (Join-Path $server 'plugins'))
    Copy-Item -LiteralPath (Join-Path $baseServer 'plugins/DfyStoneObserver.jar') -Destination (Join-Path $server 'plugins')
    $pack = & (Join-Path $PSScriptRoot 'prepare-appearance.ps1') -FixtureRoot $root
    $properties = @(
        'server-ip=127.0.0.1', 'server-port=25580', 'online-mode=false', 'enforce-secure-profile=false',
        'enable-rcon=false', 'enable-query=false', 'level-name=playtest-world', 'level-seed=4417', 'level-type=minecraft\:flat',
        'generator-settings={}', 'difficulty=peaceful', 'spawn-protection=0', 'view-distance=4', 'simulation-distance=3',
        'max-players=3', 'motd=DaFeiYu local playtest', 'resource-pack=http://127.0.0.1:25581/companion.zip',
        ('resource-pack-sha1=' + $pack.Hash.ToLowerInvariant()), 'require-resource-pack=false'
    )
    [IO.File]::WriteAllLines((Join-Path $server 'server.properties'), [string[]]$properties, [Text.UTF8Encoding]::new($false))
    [string[]]$extra = if ($Probe) { '--probe' } elseif ($PartnerProbe) { '--partner-probe' } else { '--live', $ModelConfig }
    [IO.File]::WriteAllText($launchInfo, (@{ mode = $extra[0]; config = $ModelConfig } | ConvertTo-Json -Compress))
    & node (Join-Path $PSScriptRoot 'playtest.cjs') $exe $package @extra
    if ($LASTEXITCODE) { throw "真人体验进程退出码 $LASTEXITCODE" }
} finally {
    if (Test-Path -LiteralPath $launchInfo) { Remove-Item -LiteralPath $launchInfo }
    $lock.Dispose()
}
