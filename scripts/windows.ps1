param(
    [ValidateSet('configure', 'build', 'test', 'install', 'demo')]
    [string]$Action = 'build',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
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
$preset = 'windows-' + $Configuration.ToLowerInvariant()

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with code $LASTEXITCODE" }
}

Push-Location -LiteralPath $projectRoot
try {
    switch ($Action) {
        'configure' { Invoke-Checked $cmakePath @('--preset', 'windows', '-DBUILD_TESTING=ON') }
        'build' {
            Invoke-Checked $cmakePath @('--preset', 'windows', '-DBUILD_TESTING=ON')
            Invoke-Checked $cmakePath @('--build', '--preset', $preset, '--parallel', '4')
        }
        'test' { Invoke-Checked $ctestPath @('--preset', $preset, '--parallel', '4') }
        'install' { Invoke-Checked $cmakePath @('--install', 'build/windows', '--config', $Configuration, '--prefix', 'out/dafeiyu') }
        'demo' {
            $executable = Join-Path $projectRoot "build\windows\run\$Configuration\bin\dafeiyu.exe"
            $requestId = 'demo-' + [guid]::NewGuid().ToString('N')
            Invoke-Checked $executable @('run', '--state', '.runtime/demo', '--request-id', $requestId)
        }
    }
} finally { Pop-Location }
