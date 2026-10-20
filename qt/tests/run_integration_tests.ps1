[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [string]$QtRoot = $env:WINSCPQT_QT_ROOT,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$PackageName = "WinSCPQt-1.0.0-win64"
)

$ErrorActionPreference = "Stop"
$sourceRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $sourceRoot "build"
$exeDir = Join-Path $buildRoot $Configuration
$workRoot = Join-Path $buildRoot ("integration-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$homeDir = Join-Path $workRoot "home"
$roots = Join-Path $workRoot "roots"
$containerName = "winscpqt-tests-$PID"
if (-not $QtRoot) { $QtRoot = "C:\environment\Qt\6.10.3\msvc2022_64" }
if (-not $VcpkgRoot) { $VcpkgRoot = "C:\environment\vcpkg" }
$originalPath = $env:PATH
$originalHome = $env:WINSCPQT_HOME
$originalPlatform = $env:QT_QPA_PLATFORM
$originalDelete = $env:WINSCPQT_SMOKE_DELETE
$originalTestLog = $env:WINSCPQT_TEST_LOG
$originalFontDir = $env:QT_QPA_FONTDIR
$env:PATH = "$QtRoot\bin;$VcpkgRoot\installed\x64-windows\bin;$env:PATH"
$env:WINSCPQT_HOME = $homeDir
$env:QT_QPA_PLATFORM = "offscreen"
$env:QT_QPA_FONTDIR = Join-Path $env:SystemRoot "Fonts"
New-Item -ItemType Directory -Force $workRoot, $homeDir, $roots | Out-Null
Set-Content -LiteralPath (Join-Path $homeDir "settings.ini") -Encoding ascii -Value @"
[security]
askUnknownHostKey=false
savePasswords=false
"@
foreach ($name in @("sftp", "ftp", "ftps", "dav")) {
    $path = Join-Path $roots $name
    New-Item -ItemType Directory -Force $path | Out-Null
    Set-Content -LiteralPath (Join-Path $path "hello.txt") -Encoding ascii -Value "hello from $name"
}

function Wait-Port([int]$Port) {
    for ($attempt = 0; $attempt -lt 100; ++$attempt) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $client.Connect("127.0.0.1", $Port)
            if ($Port -in @(22222, 22223)) {
                $stream = $client.GetStream()
                $stream.ReadTimeout = 500
                $banner = New-Object byte[] 256
                $count = $stream.Read($banner, 0, $banner.Length)
                if (-not [Text.Encoding]::ASCII.GetString($banner, 0, $count).StartsWith("SSH-")) {
                    throw "SSH service is not ready"
                }
            }
            return
        } catch {
            Start-Sleep -Milliseconds 150
        } finally { $client.Dispose() }
    }
    throw "Test service on port $Port did not start"
}

function Invoke-Smoke([string]$Name, [string]$Executable, [string[]]$Arguments) {
    Write-Host "== $Name"
    $env:WINSCPQT_TEST_LOG = Join-Path $workRoot ($Name.Replace(" ", "-") + ".txt")
    & (Join-Path $exeDir $Executable) @Arguments
    if (Test-Path -LiteralPath $env:WINSCPQT_TEST_LOG) { Get-Content -LiteralPath $env:WINSCPQT_TEST_LOG }
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE" }
}

$started = $false
try {
    docker build -t winscpqt-tests:local -f (Join-Path $PSScriptRoot "Dockerfile") $PSScriptRoot
    if ($LASTEXITCODE -ne 0) { throw "Test image build failed" }
    docker run -d --rm --name $containerName --mount "type=bind,source=$roots,target=/data" `
        -p 127.0.0.1:22222:22222 -p 127.0.0.1:22223:22223 `
        -p 127.0.0.1:21212:21212 -p 127.0.0.1:21213:21213 -p 127.0.0.1:18080:18080 `
        -p 127.0.0.1:50000-50019:50000-50019 winscpqt-tests:local python /tests/run_test_servers.py
    if ($LASTEXITCODE -ne 0) { throw "Test container failed to start" }
    $started = $true
    foreach ($port in @(22222, 22223, 21212, 21213, 18080)) { Wait-Port $port }

    $sftpUrl = "sftp://test:test@127.0.0.1:22222/"
    $scpUrl = "scp://test:test@127.0.0.1:22222/"
    $urls = @{
        SFTP = $sftpUrl
        SCP = $scpUrl
        FTP = "ftp://test:test@127.0.0.1:21212/"
        FTPS = "ftps://test:test@127.0.0.1:21213/?insecure=1"
        WebDAV = "webdav://127.0.0.1:18080/"
    }
    $uploadFile = Join-Path $workRoot "upload.txt"
    Set-Content -LiteralPath $uploadFile -Encoding ascii -Value "integration upload"
    $env:WINSCPQT_SMOKE_DELETE = "1"
    foreach ($name in @("SFTP", "SCP", "FTP", "FTPS", "WebDAV")) {
        Invoke-Smoke "backend $name" "WinSCPQtBackendSmoke.exe" @($urls[$name], $uploadFile, "/upload-$name.txt")
    }
    Invoke-Smoke "OpenSSH SFTP integrity" "WinSCPQtBackendSmoke.exe" @("sftp://test:test@127.0.0.1:22223/home/test")
    Invoke-Smoke "OpenSSH SCP integrity" "WinSCPQtBackendSmoke.exe" @("scp://test:test@127.0.0.1:22223/home/test")
    Invoke-Smoke "core regressions" "WinSCPQtCoreRegressionTest.exe" @("-o", "-,txt")
    Invoke-Smoke "runtime regressions" "WinSCPQtRuntimeTest.exe" @("-o", "-,txt")
    Invoke-Smoke "UI workflows" "WinSCPQtUiWorkflowTest.exe" @("-o", "-,txt")
    Invoke-Smoke "terminal screen" "WinSCPQtTerminalScreenTest.exe" @("-o", "-,txt")
    Invoke-Smoke "terminal keyboard" "WinSCPQtTerminalWidgetTest.exe" @("-o", "-,txt")
    Invoke-Smoke "shared SFTP connection" "WinSCPQtTerminalSmoke.exe" @($sftpUrl)
    Invoke-Smoke "shared SCP connection" "WinSCPQtTerminalSmoke.exe" @($scpUrl)
    Invoke-Smoke "OpenSSH PTY" "WinSCPQtTerminalSmoke.exe" @("sftp://test:test@127.0.0.1:22223/home/test", "--real")
    Invoke-Smoke "automatic terminal and session switching" "WinSCPQtWindowTest.exe" @("sftp://test:test@127.0.0.1:22223/home/test", $urls.FTP)
    Invoke-Smoke "sync" "WinSCPQtSyncSmoke.exe" @($sftpUrl, (Join-Path $roots "sftp"))
    foreach ($name in @("SFTP", "SCP", "FTP", "FTPS", "WebDAV")) {
        $rootName = switch ($name) { SFTP { "sftp" } SCP { "sftp" } WebDAV { "dav" } default { $name.ToLower() } }
        Invoke-Smoke "transfer $name" "WinSCPQtTransferSmoke.exe" @($urls[$name], (Join-Path $roots $rootName))
    }
    $packageDir = Join-Path $sourceRoot ("dist/" + $PackageName)
    if ($Configuration -eq "Release" -and (Test-Path -LiteralPath (Join-Path $packageDir "WinSCPQt.exe"))) {
        $developmentPath = $env:PATH
        $developmentPlatform = $env:QT_QPA_PLATFORM
        $developmentPluginPath = $env:QT_PLUGIN_PATH
        try {
            $env:PATH = "$packageDir;$env:SystemRoot\System32;$env:SystemRoot"
            $env:QT_PLUGIN_PATH = $packageDir
            Invoke-Smoke "packaged SFTP runtime" "WinSCPQtBackendSmoke.exe" @($sftpUrl)
            Invoke-Smoke "packaged FTPS runtime" "WinSCPQtBackendSmoke.exe" @($urls.FTPS)
            $env:QT_QPA_PLATFORM = "windows"
            $connectedScreenshot = Join-Path $workRoot "packaged-sftp.png"
            $packaged = Start-Process -FilePath (Join-Path $packageDir "WinSCPQt.exe") -WindowStyle Hidden `
                -ArgumentList @("--url", "sftp://test:test@127.0.0.1:22223/home/test", "--screenshot", "`"$connectedScreenshot`"") `
                -Wait -PassThru
            if ($packaged.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $connectedScreenshot)) {
                throw "Packaged SSH window failed"
            }
        } finally {
            $env:PATH = $developmentPath
            $env:QT_QPA_PLATFORM = $developmentPlatform
            $env:QT_PLUGIN_PATH = $developmentPluginPath
        }
    }
    $screenshot = Join-Path $workRoot "demo.png"
    $gui = Start-Process -FilePath (Join-Path $exeDir "WinSCPQt.exe") -WindowStyle Hidden `
        -ArgumentList @("--demo", "--screenshot", $screenshot) -Wait -PassThru
    if ($gui.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $screenshot)) { throw "GUI screenshot failed" }
    Write-Host "ALL INTEGRATION TESTS PASSED"
    Write-Host "Artifacts: $workRoot"
} finally {
    if ($started) {
        docker logs --tail 10000 $containerName *> (Join-Path $workRoot "servers.log")
        docker rm -f $containerName | Out-Null
    }
    $env:PATH = $originalPath
    $env:WINSCPQT_HOME = $originalHome
    $env:QT_QPA_PLATFORM = $originalPlatform
    $env:WINSCPQT_SMOKE_DELETE = $originalDelete
    $env:WINSCPQT_TEST_LOG = $originalTestLog
    $env:QT_QPA_FONTDIR = $originalFontDir
}
