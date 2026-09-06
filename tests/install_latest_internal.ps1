param(
    [Parameter(Mandatory = $true)]
    [string]$InstallerPath,

    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [Parameter(Mandatory = $true)]
    [string]$ResultPath
)

$ErrorActionPreference = 'Stop'
$installDirectory = Join-Path $env:ProgramFiles 'HardwareScope'
$appPath = Join-Path $installDirectory 'HardwareScope.exe'
$updaterPath = Join-Path $installDirectory 'HardwareScopeUpdater.exe'
$servicePath = Join-Path $installDirectory 'HardwareScopeSensorService.exe'
$presentMonPath = Join-Path $installDirectory 'PresentMon.exe'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'HardwareScope installation must run as administrator.'
}

$installer = Get-Item -LiteralPath $InstallerPath
$appSource = Get-Item -LiteralPath (Join-Path $BuildDirectory 'HardwareScopeNative.exe')
$updaterSource = Get-Item -LiteralPath (Join-Path $BuildDirectory 'HardwareScopeNativeUpdater.exe')
$serviceSource = Get-Item -LiteralPath (Join-Path $BuildDirectory 'HardwareScopeNativeSensorService.exe')
$presentMonSource = Get-Item -LiteralPath (Join-Path $BuildDirectory 'PresentMon.exe')

$installerArguments = @(
    '/VERYSILENT'
    '/SUPPRESSMSGBOXES'
    '/NORESTART'
    '/CLOSEAPPLICATIONS'
)
$setup = Start-Process -FilePath $installer.FullName -ArgumentList $installerArguments -Wait -PassThru
if ($setup.ExitCode -ne 0) {
    throw "HardwareScope Setup failed with exit code $($setup.ExitCode)."
}

Get-Process HardwareScope, HardwareScopeNative, HardwareScopeNativeInstrumented -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

if (Test-Path -LiteralPath $servicePath) {
    & $servicePath --uninstall
    if ($LASTEXITCODE -ne 0) {
        throw "The existing sensor service could not be removed (exit code $LASTEXITCODE)."
    }
}

Copy-Item -LiteralPath $appSource.FullName -Destination $appPath -Force
Copy-Item -LiteralPath $updaterSource.FullName -Destination $updaterPath -Force
Copy-Item -LiteralPath $serviceSource.FullName -Destination $servicePath -Force
Copy-Item -LiteralPath $presentMonSource.FullName -Destination $presentMonPath -Force

& $servicePath --install
if ($LASTEXITCODE -ne 0) {
    throw "The updated sensor service could not be installed (exit code $LASTEXITCODE)."
}

# Match the elevated installer's {autodesktop}; a personal shortcut appears
# alongside this shared shortcut and creates a duplicate on the user's desktop.
$desktop = [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonDesktopDirectory)
$shortcutPath = Join-Path $desktop 'HardwareScope.lnk'
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $appPath
$shortcut.WorkingDirectory = $installDirectory
$shortcut.IconLocation = "$(Join-Path $installDirectory 'HardwareScope.ico'),0"
$shortcut.Description = 'HardwareScope 2.0 native hardware monitoring'
$shortcut.Save()
[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) | Out-Null
[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) | Out-Null

$service = Get-Service -Name 'HardwareScopeSensorService'
$results = @(
    "Installed app: $appPath"
    "Installed updater: $updaterPath"
    "Installed service: $servicePath"
    "Installed FPS runtime: $presentMonPath"
    "Service status: $($service.Status)"
    "Service start type: $($service.StartType)"
    "Desktop shortcut: $shortcutPath"
    "App SHA-256: $((Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash)"
    "Build SHA-256: $((Get-FileHash -LiteralPath $appSource.FullName -Algorithm SHA256).Hash)"
)
$resultDirectory = Split-Path -Parent $ResultPath
New-Item -ItemType Directory -Path $resultDirectory -Force | Out-Null
$results | Set-Content -LiteralPath $ResultPath -Encoding utf8
$results
