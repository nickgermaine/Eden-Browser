import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def record_training_paths(profile, training_build):
    profile = profile.resolve()
    training_build = training_build.resolve()
    cache = (training_build / 'CMakeCache.txt').read_text()
    if not re.search(r'^ENABLE_LLVM_PROFILE_GENERATION:BOOL=ON$', cache, re.MULTILINE):
        raise RuntimeError('The training build must enable LLVM profile generation')
    source = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$', cache, re.MULTILINE)
    if not source:
        raise RuntimeError('The training build does not record its source directory')
    metadata = {
        'formatVersion': 1,
        'profileSHA256': digest(profile),
        'buildRoot': str(training_build),
        'sourceRoot': source[1],
    }
    profile.with_suffix(profile.suffix + '.json').write_text(json.dumps(metadata, indent=2) + '\n')
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


def prepare_profile(profile, source, build, affinity, environment):
    metadata_path = profile.with_suffix(profile.suffix + '.json')
    if not metadata_path.is_file():
        return profile, {}
    metadata = json.loads(metadata_path.read_text())
    if metadata.get('formatVersion') != 1 or metadata.get('profileSHA256') != digest(profile):
        raise RuntimeError('The PGO training metadata does not match the input profile')
    mappings = [(metadata['buildRoot'], str(build.resolve())), (metadata['sourceRoot'], str(source.resolve()))]
    if any(not Path(before).is_absolute() or before == '/' for before, _ in mappings):
        raise RuntimeError('The PGO training roots must be absolute project directories')
    tool = shutil.which('llvm-profdata', path=environment['PATH'])
    if not tool:
        raise RuntimeError('PGO path normalization requires llvm-profdata')
    build.mkdir(parents=True, exist_ok=True)
    prepared = build / 'eden-pgo.profdata'
    record = build / 'eden-pgo-paths.json'
    identity = {
        'inputSHA256': metadata['profileSHA256'],
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
