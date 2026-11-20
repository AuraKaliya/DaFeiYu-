# 为回环体验服准备大肥鱼的原版细臂皮肤，素材仅保存在指定测试目录。
[CmdletBinding()]
param([string]$FixtureRoot = $env:DFY_MC_TEST_ROOT)
$ErrorActionPreference = 'Stop'
if (-not $FixtureRoot -or -not [IO.Path]::IsPathRooted($FixtureRoot)) { throw '请指定绝对路径的体验目录' }
$directory = Join-Path $FixtureRoot 'appearance'
[void](New-Item -ItemType Directory -Force -Path $directory)
$hash = 'f72db0a64b13e31225ec8280f712cf496fb044d5a4b3b46d0d812028880de1c0'
$skin = Join-Path $directory 'DeepSeek-926524.png'
if (-not (Test-Path -LiteralPath $skin)) {
    Invoke-WebRequest -UseBasicParsing -Uri "https://littleskin.cn/textures/$hash" -OutFile $skin
}
if ((Get-FileHash -LiteralPath $skin -Algorithm SHA256).Hash -ne $hash) { throw '皮肤与已选的 LittleSkin 材质不一致' }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$pack = Join-Path $directory 'companion-1.21.4.zip'
$temporary = Join-Path $directory ([guid]::NewGuid().ToString() + '.zip')
try {
    $archive = [IO.Compression.ZipFile]::Open($temporary, 'Create')
    try {
        $timestamp = [DateTimeOffset]::new(2026, 11, 7, 0, 0, 0, [TimeSpan]::Zero)
        $entry = $archive.CreateEntry('pack.mcmeta')
        $entry.LastWriteTime = $timestamp
        $metadata = $entry.Open()
        try {
            $bytes = [Text.Encoding]::UTF8.GetBytes('{"pack":{"pack_format":46,"description":"大肥鱼 · DeepSeek大肥鱼 / Lemon_Cat_Tea · LittleSkin 926524"}}')
            $metadata.Write($bytes, 0, $bytes.Length)
        } finally { $metadata.Dispose() }
        # Java 1.21.4 用离线 UUID 的 Java 哈希在 18 个默认模型中选取，DfyCompanion 对应第 8 项（从零计数）。
        $texture = $archive.CreateEntry('assets/minecraft/textures/entity/player/slim/zuri.png')
        $texture.LastWriteTime = $timestamp
        $output = $texture.Open()
        try {
            $pixels = [IO.File]::ReadAllBytes($skin)
            $output.Write($pixels, 0, $pixels.Length)
        } finally { $output.Dispose() }
    } finally { $archive.Dispose() }
    Copy-Item -LiteralPath $temporary -Destination $pack -Force
} finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
Get-FileHash -LiteralPath $pack -Algorithm SHA1
