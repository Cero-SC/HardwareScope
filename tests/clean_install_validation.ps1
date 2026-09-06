param([Parameter(Mandatory)][string]$InstallerPath)
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
foreach ($scenario in @('clean', 'upgrade')) {
    $log = Join-Path $env:RUNNER_TEMP "HardwareScope-$scenario.log"
    Invoke-Package $installer @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/LOG="' + $log + '"'))
    $service = Get-Service HardwareScopeSensorService
    $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(20))
    if ($service.StartType -ne 'Automatic') { throw 'Sensor service is not automatic.' }
    Invoke-Package (Join-Path $installDirectory 'HardwareScopeSensorService.exe') @('--check-runtime')
    if (-not (Test-Path -LiteralPath (Join-Path $pawnDirectory 'PawnIOLib.dll'))) { throw 'PawnIO runtime missing.' }
    if ((Get-AuthenticodeSignature -LiteralPath (Join-Path $pawnDirectory 'PawnIOLib.dll')).Status -ne 'Valid') {
        throw 'Installed PawnIO library signature invalid.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $installDirectory 'PawnIO-Modules-COPYING.txt'))) { throw 'Module license missing.' }
    Write-Output "PASS $scenario install: signed prerequisite, driver opens, service running."
}
Invoke-Package (Join-Path $installDirectory 'unins000.exe') @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
if (Get-Service HardwareScopeSensorService -ErrorAction SilentlyContinue) { throw 'Service remains after uninstall.' }
if (-not (Test-Path -LiteralPath (Join-Path $pawnDirectory 'PawnIOLib.dll'))) { throw 'Uninstall removed the shared PawnIO dependency.' }
Write-Output 'PASS uninstall: service removed; shared PawnIO preserved.'
