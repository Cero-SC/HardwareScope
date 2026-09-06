param([Parameter(Mandatory)][string]$InstallerPath, [string]$PreviousInstallerPath, [string]$ProductionUiProbePath)
$ErrorActionPreference = 'Stop'
# Destructive package tests are restricted to a disposable GitHub-hosted runner.
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'Run this test only on a disposable GitHub-hosted Windows runner.'
}
$installDirectory = Join-Path $env:ProgramFiles 'HardwareScope'
$pawnDirectory = Join-Path $env:ProgramFiles 'PawnIO'
if ((Test-Path -LiteralPath $installDirectory) -or (Test-Path -LiteralPath $pawnDirectory) -or
    (Get-Service HardwareScopeSensorService -ErrorAction SilentlyContinue)) {
    throw 'Clean-install test requires no existing HardwareScope or PawnIO.'
}
$prerequisite = Join-Path $PSScriptRoot '..\third_party\pawnio\PawnIO_setup.exe'
if ((Get-AuthenticodeSignature -LiteralPath $prerequisite).Status -ne 'Valid') {
    throw 'Official prerequisite signature is invalid.'
}
$installer = (Resolve-Path -LiteralPath $InstallerPath).Path
function Invoke-Package([string]$Path, [string[]]$Arguments) {
    $process = Start-Process -FilePath $Path -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(120000)) { throw "Package timed out: $Path" }
    if ($process.ExitCode -ne 0) { throw "Package failed: $Path (exit $($process.ExitCode))" }
}
$baselineInstaller = if ($PreviousInstallerPath) { (Resolve-Path -LiteralPath $PreviousInstallerPath).Path } else { $installer }
if ($PreviousInstallerPath) {
    $previousVersion = [version]((Get-Item -LiteralPath $baselineInstaller).VersionInfo.ProductVersion.Trim([char]0).Trim())
    $candidateVersion = [version]((Get-Item -LiteralPath $installer).VersionInfo.ProductVersion.Trim([char]0).Trim())
    if ($previousVersion -ge $candidateVersion) { throw 'Historical upgrade requires a strictly older baseline installer.' }
}
$secondScenario = if ($PreviousInstallerPath) { 'upgrade' } else { 'same-version-reinstall' }
foreach ($scenario in @('clean', $secondScenario)) {
    $log = Join-Path $env:RUNNER_TEMP "HardwareScope-$scenario.log"
    $scenarioInstaller = if ($scenario -eq 'clean') { $baselineInstaller } else { $installer }
    Invoke-Package $scenarioInstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/LOG="' + $log + '"'))
    $service = Get-Service HardwareScopeSensorService
    $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(20))
    if ($service.StartType -ne 'Automatic') { throw 'Sensor service is not automatic.' }
    $expectedVersion = [version]((Get-Item -LiteralPath $scenarioInstaller).VersionInfo.ProductVersion.Trim([char]0).Trim())
    foreach ($binary in @('HardwareScope.exe', 'HardwareScopeSensorService.exe', 'HardwareScopeUpdater.exe')) {
        $actualVersion = [version]((Get-Item -LiteralPath (Join-Path $installDirectory $binary)).VersionInfo.ProductVersion.Trim([char]0).Trim())
        if ($actualVersion -ne $expectedVersion) { throw "Wrong installed version for ${binary}: $actualVersion instead of $expectedVersion" }
    }
    Invoke-Package (Join-Path $installDirectory 'HardwareScopeSensorService.exe') @('--check-runtime')
    if (-not (Test-Path -LiteralPath (Join-Path $pawnDirectory 'PawnIOLib.dll'))) { throw 'PawnIO runtime missing.' }
    if ((Get-AuthenticodeSignature -LiteralPath (Join-Path $pawnDirectory 'PawnIOLib.dll')).Status -ne 'Valid') {
        throw 'Installed PawnIO library signature invalid.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $installDirectory 'PawnIO-Modules-COPYING.txt'))) { throw 'Module license missing.' }
    Write-Output "PASS $scenario install: signed prerequisite, driver opens, service running."
    if ($PreviousInstallerPath -and $scenario -eq 'clean') {
        $settingsDirectory = Join-Path $env:LOCALAPPDATA 'HardwareScope'
        New-Item -ItemType Directory -Path $settingsDirectory -Force | Out-Null
        $settingsPath = Join-Path $settingsDirectory 'settings-v2.ini'
        [IO.File]::WriteAllText($settingsPath, "schema_version=7`nrefresh_interval_ms=1250`ntext_color_rgb=52E0D4`nonboarding_completed=true`nautomatic_updates=false`nstart_with_windows=false`nstart_minimized=false`nshow_osd=false`n")
        $settingsHash = (Get-FileHash -LiteralPath $settingsPath -Algorithm SHA256).Hash
    } elseif ($PreviousInstallerPath -and $scenario -eq 'upgrade') {
        if ((Get-FileHash -LiteralPath $settingsPath -Algorithm SHA256).Hash -ne $settingsHash) {
            throw 'Historical upgrade changed or removed the existing user settings file.'
        }
        Write-Output 'PASS historical upgrade: application/service/updater versions advanced and existing user settings preserved.'
    }
}
if ($ProductionUiProbePath) {
    $probePath = (Resolve-Path -LiteralPath $ProductionUiProbePath).Path
    $app = $null
    $probe = $null
    try {
        # Only launch and clean up our own candidate process on this disposable runner.
        $app = Start-Process -FilePath (Join-Path $installDirectory 'HardwareScope.exe') -PassThru
        if (-not $app.WaitForInputIdle(15000)) { throw 'Packaged application did not become idle.' }
        $probe = Start-Process -FilePath $probePath -ArgumentList @('--expect-no-hooks', '--pid', $app.Id) -WindowStyle Hidden -PassThru
        if (-not $probe.WaitForExit(30000)) { throw 'Packaged UI boundary check timed out.' }
        if ($probe.ExitCode -ne 0) { throw "Packaged UI boundary check failed: $($probe.ExitCode)" }
        Write-Output 'PASS production UI: packaged main/settings windows expose no internal test messages; Settings opens and closes.'
    } finally {
        foreach ($ownedProcess in @($probe, $app)) {
            if ($ownedProcess -and -not $ownedProcess.HasExited) {
                if (-not $ownedProcess.CloseMainWindow() -or -not $ownedProcess.WaitForExit(5000)) { $ownedProcess.Kill(); $ownedProcess.WaitForExit() }
            }
            if ($ownedProcess) { $ownedProcess.Dispose() }
        }
        $probeLog = Join-Path $env:TEMP 'HardwareScopeNativeUiSmokeTests.log'
        if (Test-Path -LiteralPath $probeLog) { Copy-Item -LiteralPath $probeLog -Destination (Join-Path $env:RUNNER_TEMP 'HardwareScope-production-ui.log') }
    }
}
Invoke-Package (Join-Path $installDirectory 'unins000.exe') @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
if (Get-Service HardwareScopeSensorService -ErrorAction SilentlyContinue) { throw 'Service remains after uninstall.' }
if (-not (Test-Path -LiteralPath (Join-Path $pawnDirectory 'PawnIOLib.dll'))) { throw 'Uninstall removed the shared PawnIO dependency.' }
Write-Output 'PASS uninstall: service removed; shared PawnIO preserved.'
