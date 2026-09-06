param(
    [Parameter(Mandatory)][string]$EvidencePath,
    [Parameter(Mandatory)][string]$InstallerPath,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$SourceCommit
)
$ErrorActionPreference = 'Stop'
$record = Get-Content -LiteralPath $EvidencePath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($SourceCommit -notmatch '^[a-fA-F0-9]{40}$' -or $record.sourceCommit -cne $SourceCommit) { throw 'Evidence source commit mismatch.' }
if ($record.version -cne $Version) { throw 'Evidence version mismatch.' }
$hash = (Get-FileHash -LiteralPath $InstallerPath -Algorithm SHA256).Hash
if ($record.installerSha256 -notmatch '^[a-fA-F0-9]{64}$' -or $record.installerSha256 -ine $hash) { throw 'Evidence does not qualify these installer bytes.' }
$required = @('deterministic', 'production-ui', 'historical-upgrade', 'installer-recovery', 'fps-fixture', 'hardware-reference', 'resource-soak')
foreach ($name in $required) {
    $entries = @($record.tests | Where-Object { $_.name -ceq $name })
    if ($entries.Count -ne 1 -or $entries[0].status -cne 'passed' -or [string]::IsNullOrWhiteSpace($entries[0].evidence)) {
        throw "Required qualification missing or failed: $name"
    }
}
$soak = @($record.tests | Where-Object { $_.name -ceq 'resource-soak' })[0]
if ([int]$soak.durationSeconds -lt 300) { throw 'Candidate needs a five-minute measured resource-growth check.' }
if ([string]::IsNullOrWhiteSpace($record.machine) -or [string]::IsNullOrWhiteSpace($record.windowsVersion)) { throw 'Evidence needs machine and Windows identifiers.' }
Write-Output 'PASS: qualification record identifies these exact installer bytes and all required evidence entries.'
