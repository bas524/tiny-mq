"""Local C++ checks. Dependencies are provisioned before the sandboxed run."""

import hashlib
import json
import re
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

from workflow_contract import _no_links, _safe_path, digest

VCPKG_CI_COMMIT = 'cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3'  # 2026.06.24


def sdk_paths(root):
    coordinate = Path(os.environ.get('AEF_CPP_SDK', root / 'handoffs/toolchains/cpp'))
    sdk = _no_links(coordinate if coordinate.is_absolute() else root / coordinate)
    if not sdk.is_relative_to(root / 'handoffs'):
        raise ValueError('C++ SDK must be inside this worktree handoffs directory')
    toolchain = _no_links(sdk / 'vcpkg/scripts/buildsystems/vcpkg.cmake')
    status = _no_links(sdk / 'installed/vcpkg/status')
    if not toolchain.is_file() or not status.is_file() or not status.stat().st_size:
        raise ValueError('C++ SDK must contain pinned vcpkg and preinstalled dependencies')
    return sdk / 'vcpkg', sdk / 'installed'


def toolchain_identity(root):
    """Bind runtime receipts to the pinned checkout and installed package ABIs."""
    vcpkg, installed = sdk_paths(root)
    def sha(path):
        with path.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest()

    git = ['git', '-c', 'core.hooksPath=/dev/null', '-C', str(vcpkg)]
    rev = subprocess.check_output([*git, 'rev-parse', 'HEAD'], text=True).strip()
    if rev != VCPKG_CI_COMMIT:
        raise ValueError('C++ SDK vcpkg must match CI tag 2026.06.24')
    dirty = subprocess.check_output([*git, 'status', '--porcelain', '--untracked-files=all'],
                                    env={**os.environ, 'GIT_OPTIONAL_LOCKS': '0'})
    if dirty:
        raise ValueError('C++ SDK vcpkg tracked files changed')
    tools = {}
    for name in ('cmake', 'ninja', 'cc', 'c++'):
        path = shutil.which(name)
        if path is None:
            raise ValueError('C++ check tool missing: ' + name)
        tools[name] = {'path': path, 'sha256': sha(Path(path).resolve())}
    tidy = shutil.which('clang-tidy')
    if tidy is None:
        tidy = next((str(path) for path in
                     (Path('/opt/homebrew/opt/llvm/bin/clang-tidy'), Path('/usr/local/opt/llvm/bin/clang-tidy'))
                     if path.is_file()), None)
    if tidy is None:
        raise ValueError('C++ check tool missing: clang-tidy')
    tools['clang-tidy'] = {'path': tidy, 'sha256': sha(Path(tidy).resolve())}
    abis = {str(p.relative_to(installed)): sha(p)
            for p in sorted(installed.glob('*/share/*/vcpkg_abi_info.txt'))}
    if not abis:
        raise ValueError('C++ SDK package ABI records missing')
    return {'vcpkg': rev, 'installed': str(installed),
            'package_status_sha256': sha(installed / 'vcpkg/status'), 'package_abis': abis, 'tools': tools}


def cmake_environment(root):
    vcpkg, installed = sdk_paths(root)
    env = {**os.environ, 'VCPKG_ROOT': str(vcpkg)}
    flags = [f'-DCMAKE_TOOLCHAIN_FILE={vcpkg}/scripts/buildsystems/vcpkg.cmake',
             f'-DVCPKG_INSTALLED_DIR={installed}', '-DVCPKG_MANIFEST_INSTALL=OFF']
    return env, flags


def run_checks(root, output, mode):
    if (root / 'CMakeUserPresets.json').exists() or (root / 'CMakeUserPresets.json').is_symlink():
        raise ValueError('C++ verification requires a worktree without CMakeUserPresets.json')
    if mode not in ('reviewer', 'conformance', 'security'):
        raise ValueError('unknown C++ check mode')
    env, flags = cmake_environment(root)
    jobs = str(min(os.cpu_count() or 1, 6))

    def run(argv, cwd=root, extra_env=None):
        print('argv:', repr(argv), 'cwd:', cwd, flush=True)
        subprocess.run(argv, cwd=cwd, env={**env, **(extra_env or {})}, check=True)

    def build(preset, *, asan=False):
        binary = output / ('asan' if asan else preset)
        if asan:
            configure = ['cmake', '-S', str(root), '-B', str(binary), '-G', 'Ninja',
                         '-DCMAKE_BUILD_TYPE=Debug',
                         '-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer -g',
                         '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address']
        else:
            configure = ['cmake', '--preset', preset, '-B', str(binary)]
        run(configure + flags)
        run(['cmake', '--build', str(binary), '--parallel', jobs])
        return binary

    if mode == 'security':
        build_dir = build('debug', asan=True)
        run([str(build_dir / 'tiny_mq'), '--gtest_filter=-*Bench*'], cwd=build_dir,
            extra_env={'ASAN_OPTIONS': 'detect_stack_use_after_return=1:detect_leaks=0'})
    else:
        build_dir = build('debug')
        if mode == 'reviewer':
            run(['cmake', '--build', str(build_dir), '--target', 'tiny_mq_test_clang_tidy'])
        run([str(build_dir / 'tiny_mq'), '--gtest_filter=-*Bench*'], cwd=build_dir)
        if mode == 'reviewer':
            build('release')


BUILD_INPUTS = ('CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json', 'external', 'server', 'tests')
GIT = ('git', '-c', 'core.hooksPath=/dev/null')


def source_files(root):
    paths = subprocess.check_output(
        [*GIT, 'ls-files', '--cached', '--others', '--exclude-standard', '-z', '--', *BUILD_INPUTS],
        cwd=root, text=True,
    ).split('\0')
    for name in sorted(set(paths) - {''}):
        path = _safe_path(root, name, missing=True)
        if path.exists():
            if not path.is_file():
                raise ValueError(f'invalid C++ build input: {path}')
            yield path


def source_identity(paths, root):
    from workflow_nfr import file_hash
    return digest({str(path.relative_to(root)): file_hash(_no_links(path)) for path in paths})


def extract_baseline(archive, output):
    with tarfile.open(archive) as sources:
        if callable(getattr(tarfile, 'data_filter', None)):
            sources.extractall(output, filter='data')
            return
        # Older 3.11 releases lack the data filter. Git supplies regular
        # tracked inputs. Reject links and special files in the fallback.
        for member in sources:
            name = member.name.removesuffix('/') if member.isdir() else member.name
            path = _safe_path(output, name, missing=True)
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
            elif member.isreg():
                path.parent.mkdir(parents=True, exist_ok=True)
                with sources.extractfile(member) as data, path.open('xb') as stream:
                    shutil.copyfileobj(data, stream)
                path.chmod(member.mode & 0o755 | 0o600)
            else:
                raise ValueError(f'invalid C++ baseline archive member: {name}')


def build_nfr_pair(root, output, base):
    from workflow_nfr import file_hash

    if not re.fullmatch(r'[0-9a-f]{40}', base):
        raise ValueError('NFR requires a pinned Git baseline')
    baseline = output / 'baseline-src'
    candidate = output / 'candidate-src'
    baseline.mkdir()
    candidate.mkdir()
    archive = output / 'baseline.tar'
    subprocess.run([*GIT, 'archive', '--format=tar', '-o', str(archive), base, '--', *BUILD_INPUTS],
                   cwd=root, check=True)
    extract_baseline(archive, baseline)
    archive.unlink()
    for path in source_files(root):
        target = candidate / path.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    baseline_id, candidate_id = (
        source_identity((path for path in src.rglob('*') if path.is_file()), src)
        for src in (baseline, candidate)
    )
    if baseline_id == candidate_id:
        raise ValueError('NFR requires distinct product sources')

    env, flags = cmake_environment(root)
    jobs = str(min(os.cpu_count() or 1, 6))
    binaries = {}
    for side, sources in (('baseline', baseline), ('candidate', candidate)):
        build = output / (side + '-build')
        subprocess.run(['cmake', '--preset', 'release', '-B', str(build), *flags, '-DENABLE_TESTS=ON'],
                       cwd=sources, env=env, check=True)
        subprocess.run(['cmake', '--build', str(build), '--parallel', jobs],
                       cwd=sources, env=env, check=True)
        binaries[side] = build / 'tiny_mq'
    metadata = {'base': base, 'baseline_sources': baseline_id, 'candidate_sources': candidate_id,
                'binary_sha256': {side: file_hash(path) for side, path in binaries.items()},
                'configuration': 'RelWithDebInfo'}
    (output / 'nfr-build.json').write_text(json.dumps(metadata, indent=2) + '\n')



if __name__ == '__main__':
    project = Path(__file__).resolve().parents[2]
    out = _no_links(Path(os.environ['AEF_CHECK_OUTPUT']))
    if not out.is_dir() or not out.is_relative_to(project / 'handoffs'):
        raise SystemExit('invalid C++ check scratch directory')
    mode = sys.argv[1]
    if mode == 'nfr-build':
        build_nfr_pair(project, out, os.environ['AEF_NFR_BASE'])
    else:
        run_checks(project, out, mode)
