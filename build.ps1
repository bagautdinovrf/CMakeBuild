[CmdletBinding()]
param(
    [ValidateSet('Auto', 'MSVC', 'MinGW')]
    [string] $Toolchain = 'Auto',
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [string] $CMakePath,
    [string] $MinGWPath,
    [string] $FltkSourcePath,
    [switch] $Run
)

$ErrorActionPreference = 'Stop'
$taskOriginalPath = $env:Path
$taskOriginalLocation = Get-Location
$taskOutput = Join-Path $PSScriptRoot 'CMakeBuild.exe'

function Find-Tool([string] $Name, [string[]] $Candidates) {
    $taskCommand = Get-Command $Name -ErrorAction SilentlyContinue
    if ($taskCommand) { return $taskCommand.Source }
    foreach ($taskCandidate in $Candidates) {
        if (Test-Path -LiteralPath $taskCandidate -PathType Leaf) {
            return $taskCandidate
        }
    }
    return $null
}

try {
    Set-Location -LiteralPath $PSScriptRoot
    if (!$CMakePath) {
        $CMakePath = Find-Tool 'cmake.exe' @(
            'C:\Program Files\CMake\bin\cmake.exe',
            'C:\Qt\Tools\CMake_64\bin\cmake.exe',
            'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        )
    }
    if (!$CMakePath -or !(Test-Path -LiteralPath $CMakePath -PathType Leaf)) {
        throw 'CMake not found. Install CMake 3.24+ or use -CMakePath <cmake.exe>.'
    }

    $taskVsWhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    $taskVisualStudio = $null
    if (Test-Path -LiteralPath $taskVsWhere) {
        $taskVisualStudio = & $taskVsWhere -products '*' -version '[17.0,18.0)' -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if ($Toolchain -eq 'Auto') {
        $Toolchain = if ($taskVisualStudio) { 'MSVC' } else { 'MinGW' }
    }

    if ($Toolchain -eq 'MSVC') {
        $taskConfigurePreset = 'msvc'
        $taskBuildPreset = 'msvc-' + $Configuration.ToLowerInvariant()
    } else {
        if (!$MinGWPath) {
            $taskGxx = Find-Tool 'g++.exe' @('C:\Qt\Tools\mingw1310_64\bin\g++.exe')
            if ($taskGxx) { $MinGWPath = Split-Path -Parent $taskGxx }
        }
        if (!$MinGWPath -or !(Test-Path -LiteralPath (Join-Path $MinGWPath 'g++.exe'))) {
            throw 'MinGW not found. Add its bin directory to PATH or use -MinGWPath <bin-directory>.'
        }
        $env:Path = $MinGWPath + ';' + $env:Path
        $taskConfigurePreset = 'mingw-' + $Configuration.ToLowerInvariant()
        $taskBuildPreset = $taskConfigurePreset
    }

    $taskConfigureArguments = @('--preset', $taskConfigurePreset)
    if ($FltkSourcePath) {
        $taskFltkSource = (Resolve-Path -LiteralPath $FltkSourcePath).Path
        $taskConfigureArguments += '-DCMAKEBUILD_FLTK_SOURCE_DIR=' + $taskFltkSource
    }
    & $CMakePath @taskConfigureArguments
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & $CMakePath --build --preset $taskBuildPreset --parallel
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed: $LASTEXITCODE" }
    if (!(Test-Path -LiteralPath $taskOutput -PathType Leaf)) {
        throw "Built application not found at the fixed output path: $taskOutput"
    }
    Write-Output "Application ($Toolchain, $Configuration): $taskOutput"
    if ($Run) { Start-Process -FilePath $taskOutput -WorkingDirectory $PSScriptRoot }
} finally {
    $env:Path = $taskOriginalPath
    Set-Location -LiteralPath $taskOriginalLocation.Path
}
