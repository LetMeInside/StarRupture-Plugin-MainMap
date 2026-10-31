# MainMap material inputs and explicit candidate cooking

The retained project, two INI files, two parent materials and two default textures are the accepted A2.2 authoring inputs. Their bytes and Unreal object paths are unchanged. The cook consumes these saved assets directly; it never reauthors them. `material-baseline.json` records their provenance, accepted cooked identities, package inventory, engine settings and delivery identities. Absolute provenance strings inside the accepted binary PAK and imported texture metadata are historical data, not filesystem inputs.

Required installed tools: Python 3 on PATH and stock Unreal Engine **5.6.1 CL 44394996**, compatible CL 43139311 (the recorded Build.version). The workflow checks Build.version and refuses a different toolchain. The DLL build separately requires Visual Studio 2022 C++ v143 and the Windows SDK; it does not invoke Unreal.

From this directory, choose a **new, nonexistent absolute directory outside Solution**:

```powershell
Unblock-File .\BuildMaterials.ps1
 .\BuildMaterials.ps1 -CandidateDirectory 'C:\MaterialCandidates\MainMap-001'
```

Use `-UnrealEngine` to select an equivalent installed UE root. The Python entry point accepts the same options as `--candidate-directory` and `--engine-root`. All paths are derived from the script, not the caller's working directory.

The script copies only the seven accepted project/config/asset inputs to the new candidate's `Project`. It redirects TEMP/TMP, UserDir and the configured local filesystem DDC there. It performs a fresh WindowsClient cook for SM5 and SM6 with NullRHI, versioned packages and inline material shader code. It uses the established A2.2 UnrealPak IoStore recipe, cook-generated package-store manifest and script objects, dynamically generated response files with the original mount paths, and a loose staging manifest. It validates IoStore chunk hashes and reads, describes exports/imports, lists and tests the PAK, and runs payload hash verification. All commands/logs and generated global containers remain in candidate staging; global containers are never DLL inputs.

Successful output is `Materials/MainMapMaterials.{pak,utoc,ucas}`, `Materials/SHA256.json`, and `comparison.json`. `Packaging` contains staging/response manifests, inventory and export description. Failure leaves the candidate and logs for diagnosis; retry with a new directory. Existing directories and the accepted files in `Src/Resources/Materials` are never overwritten. Normal DLL builds consume only the accepted package and never cook.

Candidate generation is separate from acceptance. Review comparison.json, shader/cook logs, the exact four-package inventory, material exports/imports and every differing cooked file/chunk. A valid candidate is not automatically equivalent to the shipping-tested baseline. Only after explicit approval of new material inputs may a maintainer replace all three accepted files together and update both `Src/Build/rendering-inputs.json` and this directory's matching baseline resource entries/provenance. Failed/incomplete attempts never update accepted files or pins. Future runtime verification requires a fresh game launch; preserve old loader caches.

`author_materials.py` preserves the original graph-construction recipe for authoring in an empty content-only project with these configs. Invoke it through the installed editor PythonScript commandlet, using an isolated project copy and a script path to this file. It refuses existing materials. It generates the two deterministic one-pixel PNG import sources itself; those PNGs are not required to cook the four retained assets. It is not invoked by BuildMaterials.ps1. No one-time graph correction, editor probe, old test fixture, Research record or external authoring workspace is needed.

For an explicitly intended fresh authoring operation, copy only StarRupture.uproject and Config into a new empty project directory, then run this from Cook (adjust the two installed/staging paths):

```powershell
$materialAuthorRoot = 'C:\MaterialCandidates\FreshAuthoring'
$materialEditor = 'C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
& $materialEditor "$materialAuthorRoot\StarRupture.uproject" -unattended -nop4 -nosplash -NoSound -NoSourceControl -nullrhi "-UserDir=$materialAuthorRoot\Saved\User" -run=pythonscript "-script=$((Resolve-Path .\author_materials.py).Path)"
```

The accepted saved assets are the authoritative inputs for the current package. Fresh authoring does not approve a replacement and is unnecessary when relocating or recooking those assets. Candidate packaging's inspection hardlinks require a staging filesystem that supports same-volume hardlinks (such as NTFS); every hardlink stays within the chosen candidate directory.
