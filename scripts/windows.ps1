param(
    [ValidateSet('configure', 'build', 'test', 'install', 'demo')]
    [string]$Action = 'build',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('Core', 'Minimal', 'Test')]
    [string]$Mode = 'Core'
)

$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmakePath = if ($cmakeCommand) { $cmakeCommand.Source } else { $null }
if (-not $cmakePath) {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath) {
        $cmakePath = & $vswherePath -all -products '*' -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
    }
}
if (-not $cmakePath) { throw 'CMake not found. Install CMake 3.28+ or the Visual Studio CMake tools.' }
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
if ($Action -eq 'test' -and -not $PSBoundParameters.ContainsKey('Mode')) { $Mode = 'Test' }
if ($Action -eq 'demo' -and -not $PSBoundParameters.ContainsKey('Mode')) { $Mode = 'Minimal' }
$configurePreset = switch ($Mode) { 'Core' { 'windows-core' }; 'Minimal' { 'windows-minimal' }; 'Test' { 'windows' } }
$binaryDirectory = switch ($Mode) { 'Core' { 'build/windows-core' }; 'Minimal' { 'build/windows-minimal' }; 'Test' { 'build/windows' } }
$installDirectory = switch ($Mode) { 'Core' { 'out/dafeiyu-core-v5' }; 'Minimal' { 'out/dafeiyu-minimal-v5' }; 'Test' { 'out/dafeiyu-test-build-v5' } }

# 执行构建工具或示例程序，并将非零退出码转换为脚本失败。
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with code $LASTEXITCODE" }
}

Push-Location -LiteralPath $projectRoot
try {
    switch ($Action) {
        'configure' { Invoke-Checked $cmakePath @('--preset', $configurePreset) }
        'build' {
            Invoke-Checked $cmakePath @('--preset', $configurePreset)
            Invoke-Checked $cmakePath @('--build', $binaryDirectory, '--config', $Configuration, '--parallel', '4')
        }
        'test' {
            if ($Mode -ne 'Test') { throw 'Tests require -Mode Test.' }
            Invoke-Checked $ctestPath @('--test-dir', $binaryDirectory, '-C', $Configuration, '--parallel', '1', '--output-on-failure')
        }
        'install' { Invoke-Checked $cmakePath @('--install', $binaryDirectory, '--config', $Configuration, '--prefix', $installDirectory) }
        'demo' {
            if ($Mode -eq 'Core') { throw 'The core-only build has no example profile. Use -Mode Minimal.' }
            $executable = Join-Path $projectRoot "$binaryDirectory/run/$Configuration/bin/dafeiyu.exe"
            Invoke-Checked $executable @('demo', '--state', '.runtime/minimal-v5')
        }
    }
} finally { Pop-Location }
