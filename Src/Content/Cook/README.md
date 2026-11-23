# MainMap material authoring and candidate cooking

This directory contains the source authoring and candidate-cook tools. The BCT1 source and five-package material inventory are accepted. A newly cooked candidate remains separate from the accepted resource triplet until its three container files and pins are promoted together.

MainMap now uses only `M_BuildingComplexPresent` at runtime. The accepted baseline, authored legacy material sources, and embedded `MainMapMaterials` triplet still include `M_BuildingCompose`, `M_BuildingPresent`, `T_ComposeDefault`, and `T_PresentDefault`. Removing those packages from production requires a separately reviewed Unreal cook and IoStore repackaging pass, followed by updating the accepted resource inventory and embedded-resource digest. That packaging has not been performed; do not hand-edit the accepted hashes or substitute an improvised container.

## Verified toolchain and boundaries

The verified toolchain is stock Unreal Engine 5.6.1, CL 44394996 (compatible CL 43139311), installed at `C:\Program Files\Epic Games\UE_5.6`, plus Python 3.12.10 at `C:\Users\Karel\AppData\Local\Programs\Python\Python312\python.exe`. `build_materials.py` checks the exact UE Build.version against `material-baseline.json` and stops on mismatch. Run Unreal authoring and cooking from an unrestricted process-launch context; the prior sandboxed invocations exited silently without initializing the project. Do not weaken the hash gates to accommodate a mismatch.

The established operations use isolated directories under `C:\Modding\StarRupture\MainMap\Research`. Select a new, nonexistent directory for every attempt. Keep the accepted source inputs and resource containers outside those generated projects. The workflow does not update accepted manifests, hashes, or resources.

## Isolated Unreal authoring

The successful UE PythonScriptCommandlet workflow used an isolated content-only project. Prepare a new project by copying only the accepted `StarRupture.uproject` and `Config` directory; do not copy legacy content assets. The authoring script generates its deterministic texture import sources and saved graph assets in that project. The script refuses existing authored material assets.

Example PowerShell setup (choose a new name that does not already exist):

```powershell
$solution = 'C:\Modding\StarRupture\MainMap\Solution'
$authorRoot = 'C:\Modding\StarRupture\MainMap\Research\MainMap-BCB.12E-Phase3A-Authoring-<unique>'
$engineCmd = 'C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$authorLog = Join-Path $authorRoot 'Saved\Logs\BCT1.engine.log'
if (Test-Path -LiteralPath $authorRoot) { throw "Refusing existing authoring directory: $authorRoot" }
New-Item -ItemType Directory -Path $authorRoot | Out-Null
Copy-Item -LiteralPath "$solution\Src\Content\StarRupture.uproject" -Destination $authorRoot
Copy-Item -LiteralPath "$solution\Src\Content\Config" -Destination $authorRoot -Recurse
New-Item -ItemType Directory -Path (Join-Path $authorRoot 'Saved\Temp') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $authorRoot 'Saved\User') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $authorRoot 'Saved\Logs') -Force | Out-Null
$env:TEMP = Join-Path $authorRoot 'Saved\Temp'
$env:TMP = $env:TEMP
$authorArgs = @(
    "$authorRoot\StarRupture.uproject", '-unattended', '-nop4', '-nosplash',
    '-NoSound', '-NoSourceControl', '-nullrhi', "-UserDir=$authorRoot\Saved\User",
    '-run=pythonscript', "-script=$solution\Src\Content\Cook\author_materials.py",
    "-abslog=$authorLog"
)
$authorArgs | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $authorRoot 'Saved\Logs\author-command.json')
Push-Location $authorRoot
try {
    & $engineCmd @authorArgs 1> (Join-Path $authorRoot 'Saved\Logs\author.stdout.log') 2> (Join-Path $authorRoot 'Saved\Logs\author.stderr.log')
    $authorExit = $LASTEXITCODE
    Set-Content -LiteralPath (Join-Path $authorRoot 'Saved\Logs\author.exitcode.txt') -Value $authorExit
} finally { Pop-Location }
if ($authorExit -ne 0) { throw "Unreal authoring failed with exit code $authorExit; preserve the project and logs." }
```

The command and environment above reflect the successful unrestricted retry: working directory is the project root; TEMP/TMP and `-UserDir` point inside its `Saved` directory; the engine log is an explicit absolute path. The retained successful run was `MainMap-BCB.12E-Phase3A-Authoring-Retry-20261102-04`; its invocation was:

```powershell
& 'C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'C:\Modding\StarRupture\MainMap\Research\MainMap-BCB.12E-Phase3A-Authoring-Retry-20261102-04\StarRupture.uproject' -unattended -nop4 -nosplash -NoSound -NoSourceControl -nullrhi '-UserDir=C:\Modding\StarRupture\MainMap\Research\MainMap-BCB.12E-Phase3A-Authoring-Retry-20261102-04\Saved\User' -run=pythonscript '-script=C:\Modding\StarRupture\MainMap\Solution\Src\Content\Cook\author_materials.py' '-abslog=C:\Modding\StarRupture\MainMap\Research\MainMap-BCB.12E-Phase3A-Authoring-Retry-20261102-04\Saved\Logs\BCT1.engine.log'
```

That process returned 0, the engine log showed PythonScriptCommandlet and the script's final success message, and the graph report and assets were produced. Its working directory was the authoring project root and TEMP/TMP were set to its `Saved\Temp`. Treat a zero exit code without the required outputs and log evidence as failure.

Inspect the engine/console logs, `Artifacts\authored-graphs.json`, and `Artifacts\M_BuildingComplexPresent.t3d`. Confirm the asset exists and is nonempty, the report and T3D export exist, and graph validation reports texture parameter `TileTexture` and vector parameters `UVScale` and `UVBias`. Inspect the graph: UV0 is multiplied by UVScale's RG mask, then UVBias's RG mask is added; the mask expressions use the unnamed input pin (`''`). The transformed UV feeds TileTexture. Sampled RGB feeds Emissive; sampled alpha passes through OneMinus then Saturate to Opacity. Confirm successful material compilation and no authoring errors/warnings in the engine log before transferring anything.

## Transfer only a validated new source asset

The verified BCT1 source asset from the successful authoring run was 10,620 bytes with SHA-256 `DF930062C038FDB6F8C3CF240648513007DDD0E902221CFE2465B1FBDDAB68D8`. For a later approved transfer, validate the actual generated source against its recorded evidence, refuse an existing destination, copy only that `.uasset`, then compare destination size and SHA-256. Example:

```powershell
$generated = Join-Path $authorRoot 'Content\Mods\MainMap\Materials\M_BuildingComplexPresent.uasset'
$destination = 'C:\Modding\StarRupture\MainMap\Solution\Src\Content\Content\Mods\MainMap\Materials\M_BuildingComplexPresent.uasset'
$expectedHash = 'DF930062C038FDB6F8C3CF240648513007DDD0E902221CFE2465B1FBDDAB68D8'
if (-not (Test-Path -LiteralPath $generated -PathType Leaf)) { throw "Generated asset missing: $generated" }
if ((Get-Item -LiteralPath $generated).Length -ne 10620 -or (Get-FileHash -LiteralPath $generated -Algorithm SHA256).Hash -ne $expectedHash) { throw 'Generated asset differs from verified authoring output.' }
if (Test-Path -LiteralPath $destination) { throw "Refusing to overwrite source asset: $destination" }
Copy-Item -LiteralPath $generated -Destination $destination
if ((Get-Item -LiteralPath $destination).Length -ne 10620 -or (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $expectedHash) { throw 'Transferred asset verification failed.' }
```

This procedure does not transfer reports, textures, legacy assets, or temporary authoring files. The exact hash is evidence for the reported successful source asset; if a newly generated asset has a different hash, stop for review rather than editing the expected value.

## Candidate cook and IoStore packaging

Use Python 3 from this directory or call the PowerShell wrapper. Every candidate path must be new and outside `Solution`. The last successful invocation used:

```powershell
& 'C:\Users\Karel\AppData\Local\Programs\Python\Python312\python.exe' 'C:\Modding\StarRupture\MainMap\Solution\Src\Content\Cook\build_materials.py' --candidate-directory 'C:\Modding\StarRupture\MainMap\Research\MainMap-BCB.12E-Phase3A-Candidate-20261102-02'
```

That candidate completed WindowsClient cooking and packaging. For a future run, replace the candidate path with a new nonexistent directory. The script verifies the installed engine version, checks all eight accepted authored inputs, including BCT1, against exact size/SHA-256 pins before copying. It does not modify `DefaultEngine.ini` or accepted resources. TEMP/TMP and UserDir stay inside that candidate's project; the persistent DDC junction is described below.

The accepted inventory contains exactly five ExportBundleData package chunks (the four legacy packages plus `M_BuildingComplexPresent`) and one regenerated ContainerHeader in the existing `MainMapMaterials` container. Each candidate must match all five accepted package identities. It validates the IoStore chunk hashes and reads, container description, PAK list/test/verify, cooked package summaries, and source/cooked inventories. Inspect `Project\Saved\Logs`, `Packaging`, `Materials`, and `comparison.json`. A candidate comparison must report zero cooked-file differences, identical accepted authored inputs, identical five package summaries and package chunks, and exactly five ExportBundleData chunks plus one ContainerHeader. The regenerated container header is expected to have candidate identity.

`comparison.json`'s `accepted_resources_modified: false` is produced by the comparison script; it is not independent proof that accepted repository resources are unchanged. Independently compare the `resources` pins in both `material-baseline.json` and `Src\Build\rendering-inputs.json`, then check each accepted `.pak`, `.utoc`, and `.ucas` file's exact byte size and SHA-256 against those pins. The two manifest resource arrays must agree, including resource IDs. Do not edit either manifest or replace accepted resource files during candidate generation.

## Accepted BCT1 resource inventory

`M_BuildingComplexPresent.uasset` is now accepted as an authored input at 10,620 bytes, SHA-256 `df930062c038fdb6f8c3cf240648513007ddd0e902221cfe2465b1fbddab68d8`. Its accepted cooked files are `M_BuildingComplexPresent.uasset` (3,131 bytes, SHA-256 `8ffe6d3fbadb9ec71d134b85a5233d4e5afa711965a875483a20f5567e02e872`) and `.uexp` (105,794 bytes, SHA-256 `eb6b2e7b5c85dc5c1274b4db41091d338eb0eb6082aae69b5a6d3061b0bdbc30`). The accepted package chunk is `0x4B6BCAE3880D204D`, 108,685 bytes, hash `0xe1b3efa9fdefbba9df07efd74f2d501080017335`.

The accepted resource files retain IDs 30101/30102/30103: `MainMapMaterials.pak` is 9,211 bytes / `9a478cf32c831de10816aca6757bb97e8122320bed92cb114805ede7c48d00b0`; `MainMapMaterials.utoc` is 919 bytes / `a18238fa366c31d0e84262b949fd37f752c6a269ae31b29ed1422691de6230a7`; and `MainMapMaterials.ucas` is 435,040 bytes / `ca1fd19a24c04298fa397cfa89087cce465619370dcc95df62bc163ade5f1dfc`. Both resource manifests must carry these same three entries. These accepted package changes do not implement BCT1 runtime material lookup or Phase 3B widget presentation.

## Persistent Derived Data Cache

The accepted project config routes the local filesystem DDC to `%GAMEDIR%DerivedDataCache`. Each isolated candidate therefore ordinarily resolves this path to its own `Project\DerivedDataCache`; previous logs showed different candidate-local paths and very low cache-hit rates. The cook script now creates a directory junction at that candidate path pointing to the stable Research directory:

`C:\Modding\StarRupture\MainMap\Research\MainMap-MaterialCook-DDC`

The candidate projects, TEMP/TMP, UserDir, logs and outputs remain separate. Only DDC storage is shared. The script records `candidate_ddc_path`, `persistent_ddc_path`, `resolved_ddc_path` and junction verification in `Project\Saved\Logs\ddc-location.json`, and logs the resolved destination. The persistent directory must be a plain directory directly under MainMap Research, not under `Solution`, the engine install, or any candidate. The candidate's `Project\DerivedDataCache` must not already exist. Conflicting paths, reparse points, or junction creation/resolution failures stop the cook before Unreal starts; no existing cache is deleted or replaced. Keep the persistent Research cache when disposing of a candidate. If removing a candidate is later authorized, verify and remove its junction itself first; do not recursively traverse a junction as if it were candidate-owned cache data.

The completed Phase 3A candidate's populated cache at `MainMap-BCB.12E-Phase3A-Candidate-20261102-02\Project\DerivedDataCache` can seed the persistent cache by copying it to a unique staging directory and publishing that directory only when the stable target does not exist. The source candidate cache is never modified; using it as a copy source is intentional. The script rejects a seed path that is itself a reparse point, a seed tree containing reparse points, a missing `Buckets` directory, or overlap with the persistent cache path. To seed on the first future run, after checking the stable target is absent, supply:

```powershell
--seed-ddc-from 'C:\Modding\StarRupture\MainMap\Research\MainMap-BCB.12E-Phase3A-Candidate-20261102-02\Project\DerivedDataCache'
```

Append that option and path to the `build_materials.py` invocation. The script refuses seeding if the persistent cache already exists; subsequent runs omit `--seed-ddc-from`. A partial seed copy is preserved for inspection and must not be silently deleted or overwritten. No cache was copied as part of the source/documentation change.

DDC entries are keyed by engine, platform and shader inputs; incompatible or missing entries still compile normally. Legitimate shader compilation is not disabled. The new junction and persistent-cache reuse have not yet been runtime-validated by a cook. On the next authorized candidate cook, verify the junction record/resolved DDC path, successful shader compilation, and the engine's DDC/shader cache-hit and miss summary. Do not claim reduced compilation until those logs demonstrate it. A shared cache can accumulate derived entries, but it does not alter accepted source hashes or resource pins.

## Stop and acceptance rules

Preserve failed candidates, stdout/stderr, command records, engine logs, generated reports and partial outputs. Stop on any hash mismatch, missing/empty asset, material compile/graph failure, cook or packaging command failure, missing package/chunk, unexpected legacy difference, resource pin mismatch, or unsafe DDC path. Do not retry in the same candidate or change settings to make a check pass; diagnose and use a new candidate only after review. Do not change accepted hashes/manifests, replace the accepted `.pak`/`.utoc`/`.ucas`, launch the game, or begin Phase 3B without separate authorization. The DLL build separately requires Visual Studio 2022 C++ v143 and the Windows SDK; it does not invoke Unreal. Candidate packaging's inspection hardlinks require same-volume support (for example NTFS); all inspection hardlinks stay within the candidate.

