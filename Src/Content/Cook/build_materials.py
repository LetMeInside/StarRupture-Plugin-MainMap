"""Explicit WindowsClient candidate cook/package, adapted from the verified A2.2 recipe.
Consumes retained saved assets; never authors graphs or replaces accepted resources.
"""
import json
import hashlib
import os
import subprocess
import struct
from pathlib import Path

import argparse
import shutil

parser = argparse.ArgumentParser(description='Cook retained materials into a NEW candidate directory; never accepts outputs.')
parser.add_argument('--candidate-directory', required=True, type=Path)
parser.add_argument('--engine-root', type=Path, default=Path(r'C:\Program Files\Epic Games\UE_5.6'))
args = parser.parse_args()
SOURCE = Path(__file__).resolve().parents[1]
BASELINE = json.loads((SOURCE / 'Cook/material-baseline.json').read_text(encoding='utf-8'))
CANDIDATE = args.candidate_directory.resolve()
ENGINE = args.engine_root.resolve() / 'Engine'
EDITOR = ENGINE / 'Binaries/Win64/UnrealEditor-Cmd.exe'
for tool in (EDITOR, ENGINE / 'Binaries/Win64/UnrealPak.exe'):
    if not tool.is_file(): raise SystemExit(f'Required installed UE tool missing: {tool}')
actual_build = json.loads((ENGINE / 'Build/Build.version').read_text())
if actual_build != BASELINE['engine_build']:
    raise SystemExit('Installed engine Build.version differs from accepted UE 5.6.1 CL 44394996 baseline')
if CANDIDATE.exists(): raise SystemExit(f'Refusing existing candidate directory: {CANDIDATE}')
if SOURCE.parent.parent == CANDIDATE or SOURCE.parent.parent in CANDIDATE.parents:
    raise SystemExit('Candidate outputs must be outside Solution; keep generated files out of repository source')
# Refuse paths inside the installed toolchain as well.
if ENGINE.parent == CANDIDATE or ENGINE.parent in CANDIDATE.parents:
    raise SystemExit('Candidate directory cannot be inside the installed engine')
CANDIDATE.mkdir(parents=True)
ROOT = CANDIDATE / 'Project'
ROOT.mkdir()
for entry in BASELINE['authored_inputs']:
    source = SOURCE / entry['file']
    if source.stat().st_size != entry['bytes'] or hashlib.sha256(source.read_bytes()).hexdigest() != entry['sha256']:
        raise SystemExit(f'Authoring input differs from accepted baseline: {source}; review changes before cooking')
    target = ROOT / entry['file']
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
PROJECT = ROOT / 'StarRupture.uproject'
LOGS = ROOT / 'Saved/Logs'
LOGS.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env['TEMP'] = env['TMP'] = str(ROOT / 'Saved/Temp')
Path(env['TEMP']).mkdir(parents=True, exist_ok=True)

def run(args, label):
    # Preserve attempts; never silently replace previous logs.
    i = 1
    while (LOGS / f'{label}-{i}.console.log').exists():
        i += 1
    args = [str(x) for x in args]
    args.append('-abslog=' + str(LOGS / f'{label}-{i}.engine.log'))
    (LOGS / f'{label}-{i}.command.json').write_text(json.dumps(args, indent=2))
    with (LOGS / f'{label}-{i}.console.log').open('w') as log:
        result = subprocess.run(args, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    print(f'{label}: exit={result.returncode}, log={LOGS / f"{label}-{i}.console.log"}', flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
    return LOGS / f'{label}-{i}.console.log'

common = [EDITOR, PROJECT, '-unattended', '-nop4', '-nosplash', '-NoSound',
          '-NoSourceControl', f'-UserDir={ROOT / "Saved/User"}']
def cook():
    cook_log = run(common + ['-nullrhi', '-run=cook', '-TargetPlatform=WindowsClient',
                  '-CookDir=' + str(ROOT / 'Content/Mods'), '-NoDefaultMaps',
                  '-stdout', '-FullStdOutLogOutput'], 'cook')
    text = cook_log.read_text(errors='replace')
    assert not any(marker in text for marker in ['Failed to compile Material',
        "doesn't have a valid ShaderMap", 'with an invalid ShaderMap']), 'Material shader failure; inspect cook log'
def package():
    cooked = ROOT / 'Saved/Cooked/WindowsClient'
    metadata = cooked / 'StarRupture/Metadata/packagestore.manifest'
    assert metadata.is_file(), 'Cook package-store manifest missing'
    assert 'cookinprogress=true' not in (metadata.parent / 'CookedSettings.txt').read_text().lower(), 'Cook incomplete'
    out = CANDIDATE / 'Materials'
    out.mkdir(exist_ok=True)
    base = out / 'MainMapMaterials'
    assert not any(base.with_suffix(ext).exists() for ext in ['.pak','.utoc','.ucas']), 'Refusing to replace previous triplet'
    work = CANDIDATE / 'Packaging'
    work.mkdir(exist_ok=True)
    packages = ['Mods/MiniMap/Materials/M_BuildingCompose', 'Mods/MainMap/Materials/M_BuildingPresent',
                'Mods/MiniMap/Materials/T_ComposeDefault', 'Mods/MainMap/Materials/T_PresentDefault']
    entries, inventory, summaries = [], [], []
    for package in packages:
        stem = cooked / 'StarRupture/Content' / package
        assert stem.with_suffix('.uasset').is_file() and stem.with_suffix('.uexp').is_file(), package
        fields = struct.unpack('<Iiiiii', stem.with_suffix('.uasset').read_bytes()[:24])
        assert fields[0] == 0x9e2a83c1 and fields[1] == -9, 'Unexpected UE package summary layout'
        summaries.append(dict(zip(['tag','legacy_file_version','legacy_ue3_version','file_version_ue4',
                                    'file_version_ue5','licensee_version'], fields), package='/Game/' + package))
        for source in sorted(stem.parent.glob(stem.name + '.*')):
            assert source.suffix in ['.uasset','.uexp','.ubulk'], source
            dest = '../../../StarRupture/Content/' + source.relative_to(cooked / 'StarRupture/Content').as_posix()
            entries.append(f'"{source.as_posix()}" "{dest}"')
            inventory.append({'source':str(source),'mount_path':dest,'bytes':source.stat().st_size,
                              'sha256':hashlib.sha256(source.read_bytes()).hexdigest()})
    response = work / 'IoStoreResponse.txt'
    response.write_text('\n'.join(entries) + '\n')
    stage = work / 'staging-manifest.json'
    # Record exact source/cooked identities; candidate outputs are not accepted build inputs.
    inputs = [PROJECT] + sorted((ROOT / 'Config').glob('*.ini')) + sorted((ROOT / 'Content').rglob('*.uasset'))
    cook_command = max(LOGS.glob('cook-*.command.json'), key=lambda p: p.stat().st_mtime)
    assert all(p.stat().st_mtime <= cook_command.stat().st_mtime for p in inputs), 'Authoring inputs changed after cook began; recook required'
    source_manifest = [{'file':p.relative_to(ROOT).as_posix(), 'bytes':p.stat().st_size,
                        'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in inputs]
    stage.write_text(json.dumps({'platform':'WindowsClient','container':'MainMapMaterials',
                                'shader_formats':['PCD3D_SM5','PCD3D_SM6'],
                                'shared_material_shader_code':False,'files':inventory,
                                'cooked_package_summaries':summaries,
                                'authored_inputs':source_manifest,
                                'cook_command':json.loads(cook_command.read_text()),
                                'engine_build':json.loads((ENGINE / 'Build/Build.version').read_text())}, indent=2))
    commands = work / 'IoStoreCommands.txt'
    commands.write_text(f'-Output="{base.as_posix()}" -ContainerName=MainMapMaterials -ResponseFile="{response.as_posix()}"\n')
    pak = ENGINE / 'Binaries/Win64/UnrealPak.exe'
    run([pak, f'-CreateGlobalContainer={work / "global"}', f'-CookedDirectory={cooked}',
         f'-PackageStoreManifest={metadata}', f'-ScriptObjects={metadata.parent / "scriptobjects.bin"}',
         f'-Commands={commands}', '-TargetPlatform=WindowsClient', '-validateChunkHashes'], 'iostore')
    pakresponse = work / 'PakResponse.txt'
    pakresponse.write_text(f'"{stage.as_posix()}" "../../../StarRupture/Content/Mods/MainMap/MaterialProofManifest.json"\n')
    run([pak, base.with_suffix('.pak'), f'-Create={pakresponse}'], 'pak')
    run([pak, 'IoStore', f'-List={base.with_suffix(".utoc")}', f'-Csv={work / "iostore-inventory.csv"}'], 'iostore-list')
    run([pak, 'IoStore', '-ProfileReadSpeed', f'-Container={base.with_suffix(".utoc")}', '-ReadType=Read',
         '-MaxJobCount=1', '-Validate'], 'iostore-validate')
    # Describe needs the authoring global metadata and target container adjacent.
    # Inspection hardlinks preserve the isolated delivery directory and avoid duplicate bytes.
    inspect = work / 'Inspect'
    inspect.mkdir(exist_ok=True)
    for item in [work / 'global.utoc', work / 'global.ucas', base.with_suffix('.utoc'), base.with_suffix('.ucas')]:
        os.link(item, inspect / item.name)
    run([pak, 'IoStore', f'-Describe={inspect / "global.utoc"}',
         f'-DumpToFile={work / "package-description.txt"}', '-IncludeExportHashes'], 'iostore-describe')
    run([pak, base.with_suffix('.pak'), '-List'], 'pak-list')
    run([pak, base.with_suffix('.pak'), '-Test'], 'pak-test')
    run([pak, base.with_suffix('.pak'), '-Verify'], 'pak-verify')
    hashes = []
    for ext in ['.pak','.utoc','.ucas']:
        file = base.with_suffix(ext)
        assert file.is_file() and file.stat().st_size > 0, file
        hashes.append({'file':str(file),'bytes':file.stat().st_size,'sha256':hashlib.sha256(file.read_bytes()).hexdigest()})
    (out / 'SHA256.json').write_text(json.dumps(hashes, indent=2))
    # Match this loader implementation exactly, including its nonstandard seed.
    def loader_hash(data):
        value = 1469598103934665603
        for byte in data:
            value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
        return value
    expected = ''.join(f'{ext[1:]}={loader_hash(base.with_suffix(ext).read_bytes()):016x}:{base.with_suffix(ext).stat().st_size}\n'
                       for ext in ['.pak','.utoc','.ucas'])
    (out / 'expected-loader-cache.pakhash').write_bytes(expected.encode('ascii'))
    print(json.dumps(hashes, indent=2), flush=True)


def compare():
    import csv
    stage = json.loads((CANDIDATE / 'Packaging/staging-manifest.json').read_text())
    differences = []
    for expected in BASELINE['files']:
        source = ROOT / 'Saved/Cooked/WindowsClient' / expected['file']
        current = dict(bytes=source.stat().st_size, sha256=hashlib.sha256(source.read_bytes()).hexdigest())
        if current != {k: expected[k] for k in ('bytes','sha256')}:
            differences.append(dict(file=expected['file'], baseline=expected, candidate=current))
    with (CANDIDATE / 'Packaging/iostore-inventory.csv').open() as f:
        chunks = [{k.strip():v.strip() for k,v in row.items()} for row in csv.DictReader(f)]
    names = sorted(row['Filename'] for row in chunks if row['ChunkType'] == 'ExportBundleData')
    wanted = sorted(row['Filename'] for row in BASELINE['chunks'] if row['ChunkType'] == 'ExportBundleData')
    if names != wanted or len(chunks) != 5:
        raise RuntimeError('Candidate container inventory does not contain exactly the four accepted packages and header')
    chunk_differences = []
    expected_chunks = {row['Filename']:row for row in BASELINE['chunks']}
    for row in chunks:
        before = expected_chunks[row['Filename']]
        # Container header changes with the new container name/ID; package data must be inspected separately.
        if any(row[k] != before[k] for k in ('Hash','Size','PackageId')):
            chunk_differences.append(dict(file=row['Filename'],baseline=before,candidate=row))
    result = dict(cook_completed=True, packaging_completed=True,
                  authored_inputs_identical=stage['authored_inputs'] == BASELINE['authored_inputs'],
                  shader_formats=stage['shader_formats'],shared_material_shader_code=stage['shared_material_shader_code'],
                  cooked_file_differences=differences,chunk_differences=chunk_differences,
                  package_summaries_identical=stage['cooked_package_summaries'] == BASELINE['cooked_package_summaries'],
                  accepted_resources_modified=False)
    (CANDIDATE / 'comparison.json').write_text(json.dumps(result,indent=2))
    print(f'VALID CANDIDATE: {CANDIDATE}; not accepted or installed. Cooked differences: {len(differences)}; chunk differences: {len(chunk_differences)}',flush=True)

cook()
package()
compare()
