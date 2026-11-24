# 使用目标 Paper 实例的实际 API 构建测试观察端，所有产物落在独立测试根。
[CmdletBinding()]
param(
    [string]$FixtureRoot = $env:DFY_MC_TEST_ROOT,
    [string]$Java = $env:DFY_MC_JAVA,
    [ValidateSet('stone-1.21.4', 'playtest-1.21.4')]
    [string]$ServerName = 'stone-1.21.4'
)
$ErrorActionPreference = 'Stop'
$server = Join-Path (Join-Path $FixtureRoot 'servers') $ServerName
$libraries = @(Get-ChildItem -LiteralPath (Join-Path $server 'libraries') -Recurse -File -Filter '*.jar')
if (-not ($libraries.Name -contains 'paper-api-1.21.4-R0.1-SNAPSHOT.jar')) { throw '缺少已启动过的 1.21.4 版 Paper 依赖' }
$classes = Join-Path $FixtureRoot ('observer-build/' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $classes -Force)
$arguments = Join-Path $classes 'class-path.txt'
$classPath = [string]::Join([IO.Path]::PathSeparator, $libraries.FullName).Replace('\', '/')
[IO.File]::WriteAllText($arguments, ('-classpath "' + $classPath + '"'), [Text.UTF8Encoding]::new($false))
$bin = Split-Path -Parent $Java

& (Join-Path $bin 'javac.exe') --release 21 -proc:none -encoding UTF-8 "@$arguments" -d $classes (Join-Path $PSScriptRoot 'StoneObserver.java')
if ($LASTEXITCODE) { throw '观察端编译失败' }
$jar = Join-Path $server 'plugins/DfyStoneObserver.jar'
& (Join-Path $bin 'jar.exe') --create --file $jar -C $classes dafeiyu -C $PSScriptRoot plugin.yml
if ($LASTEXITCODE) { throw '观察端打包失败' }
Get-FileHash -LiteralPath $jar
