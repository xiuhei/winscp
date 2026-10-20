# SPDX-License-Identifier: MIT
# Build a portable Release package with all Qt and vcpkg runtime dependencies.
[CmdletBinding()]
param(
    [string]$QtRoot = $env:WINSCPQT_QT_ROOT,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$PackageName = ""
)

$ErrorActionPreference = "Stop"
$sourceRoot = Split-Path $PSScriptRoot -Parent
if (-not $QtRoot) { $QtRoot = "C:\environment\Qt\6.10.3\msvc2022_64" }
if (-not $VcpkgRoot) { $VcpkgRoot = "C:\environment\vcpkg" }

& (Join-Path $PSScriptRoot "build.ps1") -Configuration Release -QtRoot $QtRoot -VcpkgRoot $VcpkgRoot
if ($LASTEXITCODE -ne 0) { throw "Release build failed" }

$version = "1.0.0"
$releaseDir = Join-Path $sourceRoot "build\Release"
$distRoot = Join-Path $sourceRoot "dist"
if (-not $PackageName) { $PackageName = "WinSCPQt-$version-win64" }
$packageDir = Join-Path $distRoot $PackageName
$zipPath = Join-Path $distRoot "$PackageName.zip"

$resolvedPackageDir = [IO.Path]::GetFullPath($packageDir)
$resolvedDistRoot = [IO.Path]::GetFullPath($distRoot).TrimEnd('\') + '\'
if (-not $resolvedPackageDir.StartsWith($resolvedDistRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Package path must remain inside the distribution directory"
}
if ((Test-Path -LiteralPath (Join-Path $packageDir "data")) -or
    (Test-Path -LiteralPath (Join-Path $packageDir "downloads"))) {
    throw "The package directory contains user data. Move it before rebuilding the package."
}
if (Test-Path -LiteralPath $packageDir) { Remove-Item -LiteralPath $packageDir -Recurse -Force }
New-Item -ItemType Directory -Force $packageDir | Out-Null

Copy-Item (Join-Path $releaseDir "WinSCPQt.exe") $packageDir
Copy-Item (Join-Path $sourceRoot "README.md") $packageDir
Copy-Item (Join-Path $sourceRoot "LICENSE") $packageDir
Copy-Item (Join-Path $sourceRoot "THIRD_PARTY.md") $packageDir
Copy-Item (Join-Path $sourceRoot "licenses") (Join-Path $packageDir "licenses") -Recurse
foreach ($moduleName in @("qtbase", "qtsvg")) {
    Get-ChildItem -LiteralPath (Join-Path $QtRoot "sbom") -Filter "$moduleName-*.spdx" |
        Where-Object { $_.Name -notlike "*.source.spdx" } |
        Copy-Item -Destination (Join-Path $packageDir "licenses/qt")
}
Set-Content -Path (Join-Path $packageDir "portable.ini") -Value "" -Encoding ASCII

$windeployqt = Join-Path $QtRoot "bin\windeployqt.exe"
if (-not (Test-Path $windeployqt)) { throw "windeployqt not found: $windeployqt" }
Write-Host "Deploying Qt runtime..."
& $windeployqt --release --compiler-runtime --no-translations --no-opengl-sw `
    --no-system-d3d-compiler (Join-Path $packageDir "WinSCPQt.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }
Copy-Item -LiteralPath (Join-Path $QtRoot "plugins/tls/qopensslbackend.dll") `
    -Destination (Join-Path $packageDir "tls/qopensslbackend.dll")

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw "Visual Studio locator not found" }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$redist = Get-ChildItem -LiteralPath (Join-Path $vsRoot "VC/Redist/MSVC") -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $redist) { throw "MSVC redistributable runtime not found" }
$crt = Get-ChildItem -LiteralPath (Join-Path $redist.FullName "x64") -Directory -Filter "Microsoft.VC*.CRT" |
    Select-Object -First 1
if (-not $crt) { throw "MSVC x64 runtime not found" }
Get-ChildItem -LiteralPath $crt.FullName -Filter "*.dll" | Copy-Item -Destination $packageDir

$runtimeDlls = @(
    "libssh2.dll",
    "libcrypto-3-x64.dll",
    "libssl-3-x64.dll",
    "z.dll"
)
foreach ($dll in $runtimeDlls) {
    $source = Join-Path $VcpkgRoot "installed\x64-windows\bin\$dll"
    if (Test-Path $source) {
        Copy-Item $source $packageDir
    } else {
        throw "Runtime DLL not found: $source"
    }
}

if (Test-Path $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Write-Host "Creating $zipPath..."
Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $zipPath

$savedPath = $env:PATH
$savedPlatform = $env:QT_QPA_PLATFORM
$savedHome = $env:WINSCPQT_HOME
$savedFontDir = $env:QT_QPA_FONTDIR
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_QPA_PLATFORM = "windows"
    $env:QT_QPA_FONTDIR = Join-Path $env:SystemRoot "Fonts"
    $env:WINSCPQT_HOME = Join-Path $sourceRoot "build/deploy-smoke-home"
    $screenshot = Join-Path $sourceRoot "build/deploy-smoke.png"
    $process = Start-Process -FilePath (Join-Path $packageDir "WinSCPQt.exe") -WindowStyle Hidden `
        -ArgumentList @("--demo", "--screenshot", "`"$screenshot`"") -Wait -PassThru `
        -WorkingDirectory $packageDir
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $screenshot) -or
        (Get-Item -LiteralPath $screenshot).Length -lt 1000) {
        throw "Deployed application smoke test failed"
    }
} finally {
    $env:PATH = $savedPath
    $env:QT_QPA_PLATFORM = $savedPlatform
    $env:WINSCPQT_HOME = $savedHome
    $env:QT_QPA_FONTDIR = $savedFontDir
}

Write-Host ""
Write-Host "Package: $packageDir"
Write-Host "Archive: $zipPath"
