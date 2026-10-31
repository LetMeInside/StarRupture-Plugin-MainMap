[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateDirectory,
    [string]$UnrealEngine = 'C:\Program Files\Epic Games\UE_5.6'
)
$ErrorActionPreference = 'Stop'
& python -B (Join-Path $PSScriptRoot 'build_materials.py') --candidate-directory $CandidateDirectory --engine-root $UnrealEngine
if ($LASTEXITCODE -ne 0) {
    throw "Material cook/package failed (exit $LASTEXITCODE). Candidate is incomplete; accepted resources were not changed. Inspect candidate Project/Saved/Logs."
}
