# SPDX-License-Identifier: MIT
# Configure and build WinSCP Qt.
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$QtRoot = $env:DBGATE_QT_ROOT,
    [string]$VcpkgRoot = $env:VCPKG_ROOT
)

$ErrorActionPreference = "Stop"
$sourceRoot = Split-Path $PSScriptRoot -Parent

if (-not $QtRoot) { $QtRoot = "C:\environment\Qt\6.10.3\msvc2022_64" }
if (-not $VcpkgRoot) { $VcpkgRoot = "C:\environment\vcpkg" }

$buildDir = Join-Path $sourceRoot "build"
Write-Host "Configuring WinSCP Qt ($Configuration)..."
cmake -S $sourceRoot -B $buildDir -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_PREFIX_PATH="$QtRoot" `
    -DCMAKE_TOOLCHAIN_FILE="$VcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
    -DVCPKG_MANIFEST_MODE=OFF `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DVCPKG_INSTALLED_DIR="$VcpkgRoot\installed"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }

Write-Host "Building..."
cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

Write-Host "Built: $(Join-Path $buildDir "$Configuration\WinSCPQt.exe")"
