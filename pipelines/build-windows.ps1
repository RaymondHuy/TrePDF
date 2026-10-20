<#
.SYNOPSIS
Build and package TrePDF locally for Windows x64, using installed tools.
.DESCRIPTION
Requires 64-bit PowerShell, Visual Studio 2022 or 2026 C++ tools + Windows SDK,
Qt 6.8+ MSVC x64 (QML, Quick Controls, LinguistTools, ShaderTools,
ImageFormats and, for Qt 6.12, TaskTree), Python 3.12+, CMake and Ninja.
Visual Studio discovery prefers 2022, then 2026 (including Insiders).
Defaults to E:\Qt\6.12.0\msvc2022_64 when installed. Override with -QtRoot
or QT_ROOT_DIR; otherwise falls back to qmake.exe on PATH.
Does not install Qt, Node.js or build tools. The first run downloads and
builds the pinned PDF/OCR dependencies; later runs reuse their manifest.
Output: artifacts/local/windows-x64/*.zip and *.sha256.
Without -Version / TREPDF_VERSION, increments the highest local 0.0.x output
or staging folder, including incomplete builds. Existing output is never replaced.
.EXAMPLE
.\pipelines\build-windows.ps1
.EXAMPLE
.\pipelines\build-windows.ps1 -QtRoot E:\Qt\6.12.0\msvc2022_64 -Version 0.0.10 -RefreshDependencies
.EXAMPLE
.\pipelines\build-windows.ps1 -CheckOnly
#>
[CmdletBinding()]
param(
    [string]$QtRoot = $env:QT_ROOT_DIR,
    [string]$Version = $env:TREPDF_VERSION,
    [ValidateRange(1, 256)][int]$Parallel = 3,
    [switch]$RefreshDependencies,
    [switch]$CheckOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT' -or [IntPtr]::Size -ne 8) {
    throw 'Run this script in 64-bit PowerShell on Windows.'
}
$Repo = Split-Path -Parent $PSScriptRoot
$Platform = 'windows-x64'

function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    $saved = @{}
    foreach ($name in @('GITHUB_ENV', 'GITHUB_STEP_SUMMARY')) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    try {
        & $Executable @Arguments
        if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE." }
    } finally {
        foreach ($name in $saved.Keys) {
            [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
        }
    }
}
function Get-QtQuery([string]$Key) {
    $result = & $Qmake -query $Key
    if ($LASTEXITCODE -ne 0) { throw "Cannot query Qt: $Key." }
    return ($result -join "`n").Trim()
}

$DefaultQtRoot = 'E:\Qt\6.12.0\msvc2022_64'
if (-not $QtRoot -and (Test-Path -LiteralPath $DefaultQtRoot -PathType Container)) {
    $QtRoot = $DefaultQtRoot
}
if (-not $QtRoot) {
    $qmakeCommand = Get-Command qmake.exe -ErrorAction SilentlyContinue
    if (-not $qmakeCommand) { throw 'Supply -QtRoot <Qt MSVC x64 kit> or set QT_ROOT_DIR / qmake.exe on PATH.' }
    $Qmake = $qmakeCommand.Source
    $QtRoot = Get-QtQuery 'QT_INSTALL_PREFIX'
}
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path.Replace('\', '/')
$Qmake = Join-Path $QtRoot 'bin/qmake.exe'
if (-not (Test-Path -LiteralPath $Qmake -PathType Leaf)) { throw "Missing $Qmake." }
$QtVersion = Get-QtQuery 'QT_VERSION'
if ($QtVersion -notmatch '^6\.[0-9]+\.[0-9]+$' -or [version]$QtVersion -lt [version]'6.8.0') {
    throw "Qt 6.8 or newer is required; found $QtVersion."
}
$QtSpec = Get-QtQuery 'QMAKE_XSPEC'
$QtConfig = Join-Path (Get-QtQuery 'QT_INSTALL_ARCHDATA') 'mkspecs/qconfig.pri'
if ($QtSpec -ne 'win32-msvc' -or
        (Get-Content -LiteralPath $QtConfig -Raw) -notmatch '(?m)^QT_ARCH\s*=\s*x86_64\s*$') {
    throw 'Use a Qt desktop MSVC x64 kit (not MinGW, ARM64, iOS or WebAssembly).'
}
foreach ($relative in @('lib/cmake/Qt6/Qt6Config.cmake', 'bin/windeployqt.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $QtRoot $relative) -PathType Leaf)) {
        throw "The Qt kit is missing $relative."
    }
}
$env:PATH = (Join-Path $QtRoot 'bin') + [IO.Path]::PathSeparator + $env:PATH
$env:QT_VERSION = $QtVersion
$QtTools = Join-Path (Split-Path -Parent (Split-Path -Parent $QtRoot)) 'Tools'
foreach ($relative in @('CMake_64/bin', 'Ninja')) {
    $directory = Join-Path $QtTools $relative
    if (Test-Path -LiteralPath $directory -PathType Container) {
        $env:PATH = $directory + [IO.Path]::PathSeparator + $env:PATH
    }
}

if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -ne 'x64' -or
        $env:VisualStudioVersion -notmatch '^(17|18)\.' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $Vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $Vswhere -PathType Leaf)) {
        throw 'Install Visual Studio 2022 or 2026 C++ build tools and a Windows SDK before running this script.'
    }
    $VsPath = & $Vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $VsPath) {
        $VsPath = & $Vswhere -latest -version '[18.0,19.0)' -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if ($LASTEXITCODE -ne 0 -or -not $VsPath) { throw 'Visual Studio 2022 or 2026 MSVC x64 tools were not found.' }
    Import-Module (Join-Path $VsPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -ne 'x64') {
    throw 'Visual Studio failed to initialize the x64 compiler environment.'
}
foreach ($tool in @('cl.exe', 'cmake.exe', 'ninja.exe')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "Required local tool not found: $tool." }
}
$Python = $null
$PythonArgs = @()
foreach ($candidate in @('python.exe', 'python3.exe', 'py.exe')) {
    $command = Get-Command $candidate -ErrorAction SilentlyContinue
    if (-not $command) { continue }
    $prefix = @()
    if ($candidate -eq 'py.exe') { $prefix = @('-3') }
    try {
        & $command.Source @prefix -c 'import platform, struct, sys; sys.exit(0 if sys.version_info >= (3,12) and struct.calcsize(''P'') == 8 and platform.machine().lower() in (''amd64'', ''x86_64'') else 1)'
    } catch {
        # A Windows App Execution Alias may exist without an installed Python.
        continue
    }
    if ($LASTEXITCODE -eq 0) { $Python = $command.Source; $PythonArgs = $prefix; break }
}
if (-not $Python) { throw 'Python 3.12+ x64 is required on PATH (python.exe or py.exe).' }

$Output = Join-Path $Repo "artifacts/local/$Platform"
$StageRoot = Join-Path $Repo "build/local-stage/$Platform"
if (-not $Version) {
    [long]$patch = 0
    foreach ($directory in @($Output, $StageRoot)) {
        if (Test-Path -LiteralPath $directory -PathType Container) {
            Get-ChildItem -LiteralPath $directory | ForEach-Object {
                if ($_.Name -match '^TrePDF-0\.0\.([1-9][0-9]*)-windows-x64(?:\.zip(?:\.sha256)?)?$') {
                    $patch = [Math]::Max($patch, [long]$Matches[1])
                }
            }
        }
    }
    $Version = "0.0.$($patch + 1)"
}
if ($Version -notmatch '^0\.0\.[1-9][0-9]*$') { throw 'Version must be 0.0.x, with x >= 1.' }
$Archive = Join-Path $Output "TrePDF-$Version-$Platform.zip"
$Stage = Join-Path $StageRoot "TrePDF-$Version-$Platform"
if ((Test-Path -LiteralPath $Archive) -or (Test-Path -LiteralPath "$Archive.sha256") -or
        (Test-Path -LiteralPath $Stage)) {
    throw "Version $Version already has output or a stage folder. Choose a new -Version."
}
$Kit = "$Platform-qt$QtVersion"
$Dependencies = Join-Path $Repo "_deps/local/$Kit"
$ManifestPath = Join-Path $Dependencies 'manifest.json'
$Build = Join-Path $Repo "build/local/$Kit"
Write-Host "TrePDF $Version | $Platform | Qt $QtVersion | $Parallel build jobs"
Write-Host "Build: $Build`nDependencies: $Dependencies`nPackage: $Archive"
if ($CheckOnly) {
    Invoke-Checked 'cmake.exe' @('--version')
    Invoke-Checked 'ninja.exe' @('--version')
    Write-Host 'Preflight passed. No dependencies downloaded and no build started.'
    return
}
New-Item -ItemType Directory -Path (Join-Path $Repo 'build/local') -Force | Out-Null
$LockPath = Join-Path $Repo "build/local/.$Platform.lock"
try {
    $BuildLock = [IO.FileStream]::new($LockPath, [IO.FileMode]::CreateNew,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::None, 4096, [IO.FileOptions]::DeleteOnClose)
} catch {
    throw "Cannot acquire build lock $LockPath. Another Windows build may be running. $($_.Exception.Message)"
}
try {
    if ((Test-Path -LiteralPath $Archive) -or (Test-Path -LiteralPath "$Archive.sha256") -or
            (Test-Path -LiteralPath $Stage)) {
        throw 'Output appeared while acquiring the build lock; run again for the next version.'
    }
    New-Item -ItemType Directory -Path $Output -Force | Out-Null
    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf) -or $RefreshDependencies) {
        Invoke-Checked $Python ($PythonArgs + @((Join-Path $Repo 'scripts/ci/prepare_dependencies.py'),
            '--platform', $Platform, '--directory', $Dependencies))
    }
    $Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if ($Manifest.platform -ne $Platform) { throw 'Dependency manifest platform mismatch; use -RefreshDependencies.' }
    $Roots = @{}
    foreach ($name in @('PDFium_ROOT', 'QPDF_ROOT', 'OCR_ROOT', 'ZLIB_ROOT')) {
        $value = [string]$Manifest.roots.$name
        if (-not $value -or -not (Test-Path -LiteralPath $value -PathType Container)) {
            throw "Invalid dependency root $name; use -RefreshDependencies."
        }
        # CMake embeds these paths in generated scripts and C++ definitions.
        $Roots[$name] = (Resolve-Path -LiteralPath $value).Path.Replace('\', '/')
    }

    Write-Host "Building TrePDF $Version for $Platform using Qt $QtVersion"
    Invoke-Checked 'cmake.exe' @('-S', $Repo, '-B', $Build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF', '-DTREPDF_BUILD_WEB=OFF',
        '-DTREPDF_DEPLOY_RUNTIME=ON', "-DTREPDF_VERSION=$Version",
        "-DQt6_DIR=$QtRoot/lib/cmake/Qt6", "-DCMAKE_PREFIX_PATH=$QtRoot",
        '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
        "-DPDFium_ROOT=$($Roots.PDFium_ROOT)", "-DQPDF_ROOT=$($Roots.QPDF_ROOT)",
        "-DOCR_ROOT=$($Roots.OCR_ROOT)", "-DZLIB_ROOT=$($Roots.ZLIB_ROOT)",
        '-DCMAKE_INSTALL_BINDIR=bin', '-DCMAKE_INSTALL_LIBDIR=lib')
    Invoke-Checked 'cmake.exe' @('--build', $Build, '--target', 'trepdf', '--parallel', "$Parallel")
    Invoke-Checked 'cmake.exe' @('--install', $Build, '--config', 'Release', '--prefix', $Stage)
    Invoke-Checked $Python ($PythonArgs + @((Join-Path $Repo 'scripts/ci/package_uat.py'),
        '--platform', $Platform, '--version', $Version, '--stage', $Stage,
        '--dependencies', $Dependencies, '--output', $Output))
    Write-Host "Ready: $Archive"
} finally {
    $BuildLock.Dispose()
}
