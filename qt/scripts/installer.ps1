[CmdletBinding()]
param(
    [string]$PackageName = "WinSCPQt-1.0.0-win64-portable",
    [string]$CompilerPath = $env:INNOSETUP_COMPILER
)

$ErrorActionPreference = "Stop"
$sourceRoot = Split-Path $PSScriptRoot -Parent
$distRoot = [IO.Path]::GetFullPath((Join-Path $sourceRoot "dist"))
$packageDir = [IO.Path]::GetFullPath((Join-Path $distRoot $PackageName))
if (-not $packageDir.StartsWith($distRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Package path must remain inside the distribution directory"
}
if (-not (Test-Path -LiteralPath (Join-Path $packageDir "WinSCPQt.exe"))) {
    throw "Run deploy.ps1 -PackageName $PackageName before building the installer"
}
foreach ($directory in @("data", "downloads")) {
    if (Test-Path -LiteralPath (Join-Path $packageDir $directory)) {
        throw "The package directory contains user data. Deploy a clean package first."
    }
}
if (-not $CompilerPath) {
    $compiler = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($compiler) { $CompilerPath = $compiler.Source }
    else {
        foreach ($folder in @("Inno Setup 7", "Inno Setup 6")) {
            foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, (Join-Path $env:LOCALAPPDATA "Programs"))) {
                $candidate = Join-Path $root "$folder\ISCC.exe"
                if (Test-Path -LiteralPath $candidate) { $CompilerPath = $candidate; break }
            }
            if ($CompilerPath) { break }
        }
    }
}
if (-not $CompilerPath -or -not (Test-Path -LiteralPath $CompilerPath)) {
    throw "Inno Setup 6.3+ is required. Set INNOSETUP_COMPILER or pass -CompilerPath."
}
$version = (Get-Item -LiteralPath (Join-Path $packageDir "WinSCPQt.exe")).VersionInfo.ProductVersion
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "Invalid executable product version" }
$setupName = "WinSCPQt-$version-win64-setup"
& $CompilerPath "/DPackageDir=$packageDir" "/DOutputDir=$distRoot" "/DSetupName=$setupName" `
    (Join-Path $PSScriptRoot "installer.iss")
if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed" }
$setupPath = Join-Path $distRoot "$setupName.exe"
$hash = (Get-FileHash -LiteralPath $setupPath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath ($setupPath + ".sha256") -Encoding ascii -Value "$hash  $setupName.exe"
Write-Host "Installer: $setupPath"
