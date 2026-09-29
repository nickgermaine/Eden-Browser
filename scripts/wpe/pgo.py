import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


IDENTITY_FIELDS = (
    'version',
    'sourceSHA256',
    'patches',
    'patchCatalogSHA256',
    'adoptedPatchSources',
    'canvasReplayPatch',
    'canvasReplaySources',
    'hugePageAlignment',
    'verifyPatches',
    'embeddedPatchGeneratorSHA256',
    'preparedManagedSourceSHA256',
    'compiler',
)
SHA256 = re.compile(r'^[0-9a-f]{64}$')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def profile_identity(manifest):
    if not isinstance(manifest, dict):
        raise RuntimeError('The WebKit build manifest is not an object')
    identity = {key: manifest.get(key) for key in IDENTITY_FIELDS}
    if (not isinstance(identity['version'], str) or not identity['version']
            or not isinstance(identity['compiler'], str) or not identity['compiler']):
        raise RuntimeError('The WebKit build manifest has incomplete PGO identity metadata')
    if any(not isinstance(identity[key], str) or not SHA256.fullmatch(identity[key])
           for key in ('sourceSHA256', 'patchCatalogSHA256', 'embeddedPatchGeneratorSHA256',
                       'preparedManagedSourceSHA256')):
        raise RuntimeError('The WebKit build manifest has invalid PGO source hashes')
    patches = identity['patches']
    if (not isinstance(patches, list) or not patches
            or any(not isinstance(name, str) or not name for name in patches)
            or len(patches) != len(set(patches))):
        raise RuntimeError('The WebKit build manifest has an invalid PGO patch list')
    for key in ('adoptedPatchSources', 'canvasReplaySources'):
        sources = identity[key]
        if (not isinstance(sources, dict)
                or any(not isinstance(name, str) or not name or not isinstance(value, str)
                       or not SHA256.fullmatch(value) for name, value in sources.items())):
            raise RuntimeError('The WebKit build manifest has invalid PGO patch source hashes')
    if (not isinstance(identity['canvasReplayPatch'], bool)
            or not isinstance(identity['hugePageAlignment'], bool)
            or not isinstance(identity['verifyPatches'], bool)):
        raise RuntimeError('The WebKit build manifest has invalid PGO patch selections')
    return identity


def training_manifest(training_build):
    cache = (training_build / 'CMakeCache.txt').read_text()
    install_prefix = re.search(r'^CMAKE_INSTALL_PREFIX:[^=]+=(.+)$', cache, re.MULTILINE)
    if not install_prefix:
        raise RuntimeError('The training build does not record its install prefix')
    path = Path(install_prefix[1]) / 'share' / 'eden-wpe-build.json'
    if not path.is_file():
        raise RuntimeError('The profile-generate runtime manifest is missing')
    manifest = json.loads(path.read_text())
    if manifest.get('mode') != 'profile-generate':
        raise RuntimeError('The PGO profile must come from a profile-generate runtime')
    return path, manifest


def record_training_paths(profile, training_build):
    profile = profile.resolve()
    training_build = training_build.resolve()
    cache = (training_build / 'CMakeCache.txt').read_text()
    if not re.search(r'^ENABLE_LLVM_PROFILE_GENERATION:BOOL=ON$', cache, re.MULTILINE):
        raise RuntimeError('The training build must enable LLVM profile generation')
    source = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$', cache, re.MULTILINE)
    if not source:
        raise RuntimeError('The training build does not record its source directory')
    manifest_path, manifest = training_manifest(training_build)
    metadata = {
        'formatVersion': 2,
        'profileSHA256': digest(profile),
        'buildRoot': str(training_build),
        'sourceRoot': str(Path(source[1]).resolve()),
        'trainingIdentity': profile_identity(manifest),
        'trainingManifest': str(manifest_path.resolve()),
        'trainingManifestSHA256': digest(manifest_path),
    }
    metadata_path = profile.with_suffix(profile.suffix + '.json')
    pending = metadata_path.with_name(metadata_path.name + '.tmp')
    try:
        pending.write_text(json.dumps(metadata, indent=2) + '\n')
        pending.replace(metadata_path)
    finally:
        pending.unlink(missing_ok=True)
    return metadata


def normalize_symbols(contents, mappings):
    lines = contents.splitlines(keepends=True)
    names = {}
    functions = 0
    references = 0
    for index, line in enumerate(lines):
        mapped = line
        for before, after in mappings:
            if line.startswith(before.rstrip('/') + '/'):
                mapped = after.rstrip('/') + line[len(before.rstrip('/')):]
                break
        if index + 1 < len(lines) and lines[index + 1].strip() == '# Func Hash:':
            name = mapped.rstrip('\n')
            original = line.rstrip('\n')
            if name in names and names[name] != original:
                raise RuntimeError('PGO path normalization would merge distinct function records')
            names[name] = original
            functions += int(mapped != line)
        else:
            references += int(mapped != line)
        lines[index] = mapped
    return ''.join(lines), {'remappedFunctions': functions, 'remappedReferences': references}


def prepare_profile(profile, source, build, affinity, environment, expected_identity):
    profile = profile.resolve()
    metadata_path = profile.with_suffix(profile.suffix + '.json')
    if not metadata_path.is_file():
        raise RuntimeError('The PGO profile is missing its profile-generate provenance metadata')
    metadata = json.loads(metadata_path.read_text())
    if (metadata.get('formatVersion') != 2 or metadata.get('profileSHA256') != digest(profile)
            or not isinstance(metadata.get('trainingIdentity'), dict)
            or metadata.get('trainingIdentity') != profile_identity(expected_identity)
            or not isinstance(metadata.get('trainingManifest'), str)
            or not Path(metadata['trainingManifest']).is_absolute()
            or not isinstance(metadata.get('trainingManifestSHA256'), str)
            or not SHA256.fullmatch(metadata['trainingManifestSHA256'])):
        raise RuntimeError('The PGO training metadata does not match the profile or prepared source identity')
    training_manifest_path = Path(metadata['trainingManifest'])
    if (not training_manifest_path.is_file()
            or digest(training_manifest_path) != metadata['trainingManifestSHA256']):
        raise RuntimeError('The PGO training runtime manifest changed after provenance was recorded')
    training_manifest = json.loads(training_manifest_path.read_text())
    if (not isinstance(training_manifest, dict)
            or training_manifest.get('mode') != 'profile-generate'
            or profile_identity(training_manifest) != metadata['trainingIdentity']):
        raise RuntimeError('The PGO training runtime manifest does not match its recorded source identity')
    roots = {key: metadata.get(key) for key in ('buildRoot', 'sourceRoot')}
    if any(not isinstance(value, str) or not Path(value).is_absolute() or value == '/' for value in roots.values()):
        raise RuntimeError('The PGO training roots must be absolute project directories')
    mappings = [(roots['buildRoot'], str(build.resolve())), (roots['sourceRoot'], str(source.resolve()))]
    tool = shutil.which('llvm-profdata', path=environment['PATH'])
    if not tool:
        raise RuntimeError('PGO path normalization requires llvm-profdata')
    build.mkdir(parents=True, exist_ok=True)
    prepared = build / 'eden-pgo.profdata'
    record = build / 'eden-pgo-paths.json'
    identity = {
        'inputSHA256': metadata['profileSHA256'],
        'trainingIdentity': metadata['trainingIdentity'],
        'trainingManifest': metadata['trainingManifest'],
        'trainingManifestSHA256': metadata['trainingManifestSHA256'],
        'mappings': [list(pair) for pair in mappings],
        'normalizerSHA256': digest(Path(__file__)),
    }
    if prepared.is_file() and record.is_file():
        saved = json.loads(record.read_text())
        if all(saved.get(key) == value for key, value in identity.items()) and saved.get('outputSHA256') == digest(prepared):
            return prepared, saved
    def run(arguments):
        return subprocess.run(affinity + [tool, *arguments], env=environment, text=True, capture_output=True, check=True).stdout
    with tempfile.TemporaryDirectory(prefix='eden-pgo-paths-') as temporary:
        temporary = Path(temporary)
        text_profile = temporary / 'profile.txt'
        run(['merge', '--num-threads=1', '--text', str(profile.resolve()), '-o', str(text_profile)])
        normalized, counts = normalize_symbols(text_profile.read_text(), mappings)
        text_profile.write_text(normalized)
        merged = temporary / 'profile.profdata'
        run(['merge', '--num-threads=1', '--failure-mode=any', str(text_profile), '-o', str(merged)])
        if run(['show', str(profile.resolve())]) != run(['show', str(merged)]):
            raise RuntimeError('PGO path normalization changed the profile counter summary')
        if not prepared.is_file() or digest(prepared) != digest(merged):
            pending = prepared.with_suffix('.tmp')
            shutil.copyfile(merged, pending)
            pending.replace(prepared)
    result = identity | counts | {'outputSHA256': digest(prepared)}
    record.write_text(json.dumps(result, indent=2) + '\n')
    print(f"Matched PGO paths for {counts['remappedFunctions']} functions and {counts['remappedReferences']} references", flush=True)
    return prepared, result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--training-build', type=Path, required=True)
    arguments = parser.parse_args()
    print(json.dumps(record_training_paths(arguments.profile, arguments.training_build), indent=2))
