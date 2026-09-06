$ErrorActionPreference = 'Stop'
$directory = Join-Path ([IO.Path]::GetTempPath()) ('HardwareScope-evidence-fixture-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $directory | Out-Null
$installer = Join-Path $directory 'fixture.bin'
$evidence = Join-Path $directory 'fixture.json'
$validator = Join-Path $PSScriptRoot 'validate_release_evidence.ps1'
$commit = 'a' * 40
$failed = 0
try {
    [IO.File]::WriteAllText($installer, 'fixture, not an installer')
    $record = @{
        version = '0.0.1'; sourceCommit = $commit; installerSha256 = (Get-FileHash $installer -Algorithm SHA256).Hash
        machine = 'test fixture'; windowsVersion = 'test fixture'
        tests = @('deterministic','production-ui','historical-upgrade','installer-recovery','fps-fixture','hardware-reference','resource-soak') | ForEach-Object {
            @{name=$_; status='passed'; evidence='synthetic validator fixture, not product qualification'; durationSeconds=300}
        }
    }
    [IO.File]::WriteAllText($evidence, ($record | ConvertTo-Json -Depth 6))
    & $validator -EvidencePath $evidence -InstallerPath $installer -Version '0.0.1' -SourceCommit $commit
    foreach ($scenario in @('wrong-version','wrong-commit','wrong-hash','missing-test','short-soak')) {
        $bad = ($record | ConvertTo-Json -Depth 6) | ConvertFrom-Json
        switch ($scenario) {
            'wrong-version' { $bad.version = '0.0.2' }
            'wrong-commit' { $bad.sourceCommit = 'b' * 40 }
            'wrong-hash' { $bad.installerSha256 = '0' * 64 }
            'missing-test' { $bad.tests = @($bad.tests | Where-Object name -ne 'hardware-reference') }
            'short-soak' { ($bad.tests | Where-Object name -eq 'resource-soak').durationSeconds = 60 }
        }
        [IO.File]::WriteAllText($evidence, ($bad | ConvertTo-Json -Depth 6))
        $rejected = $false
        try { & $validator -EvidencePath $evidence -InstallerPath $installer -Version '0.0.1' -SourceCommit $commit | Out-Null }
        catch { $rejected = $true }
        if (-not $rejected) { Write-Error "Validator accepted $scenario" }
    }
    Write-Output 'PASS: release-evidence validator accepts only matching, complete qualification records (synthetic fixtures).'
} finally {
    Remove-Item -LiteralPath $installer,$evidence -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $directory -ErrorAction SilentlyContinue
}
