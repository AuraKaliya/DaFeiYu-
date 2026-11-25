[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [Alias('Profile')][ValidateNotNullOrEmpty()][string]$ConfigurePreset = 'windows',
    [ValidateNotNullOrEmpty()][string]$Target,
    [ValidateRange(1, 2147483647)][int]$Jobs
)
$ErrorActionPreference = 'Stop'

# 定位命令行或 Visual Studio 安装的 CMake，供配置和编译共用。
# 步骤：检查 PATH → 查询已安装的 VS 工具 → 报告缺失的构建工具。
function Find-CMake {
    $command = Get-Command cmake.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Path }

    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $locator) {
        $bundled = & $locator -products '*' -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if ($bundled) { return ($bundled | Select-Object -First 1) }
    }
    throw '找不到 CMake。请将 CMake 3.28 或更高版本加入 PATH，或安装 Visual Studio 的 CMake 工具。'
}

$cmake = Find-CMake
$buildPreset = "$ConfigurePreset-$($Configuration.ToLowerInvariant())"
$buildArgs = @('--build', '--preset', $buildPreset)
if ($Target) { $buildArgs += @('--target', $Target) }
if ($PSBoundParameters.ContainsKey('Jobs')) { $buildArgs += @('--parallel', "$Jobs") }

Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    & $cmake --preset $ConfigurePreset
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $cmake @buildArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
