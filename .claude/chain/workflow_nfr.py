"""Recompute the C++ performance gate from the native check's raw samples."""
import hashlib
import json
import math
import re
import shutil
import statistics
import sys
from pathlib import Path

from workflow_contract import _no_links, _safe_path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tests'))
from nfr71 import HOT_PATHS, NANOSECONDS, PROFILES as DESTINATION_PROFILES, machine_lock
from nfr72 import DELIVERY_PATHS, PROFILES as DELIVERY_PROFILES

PATHS = (*HOT_PATHS, *DELIVERY_PATHS)
PROFILES = (*DESTINATION_PROFILES, *DELIVERY_PROFILES)
ORDER = (('baseline', 4), ('candidate', 4), ('candidate', 5), ('baseline', 5))


def file_hash(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def regular(path):
    path = _no_links(path)
    if not path.is_file() or path.stat().st_nlink != 1:
        raise ValueError(f'invalid NFR file: {path}')
    return path


def samples(path, names, repetitions):
    rows = {name: [] for name in names}
    for item in json.loads(regular(path).read_text())['benchmarks']:
        if item['run_type'] != 'iteration':
            continue
        run_name = item['run_name']
        if not isinstance(run_name, str) or not run_name.startswith('BenchmarkFixture/'):
            raise ValueError(f'unexpected NFR benchmark: {path}')
        name = run_name.removeprefix('BenchmarkFixture/')
        series = rows[name]
        if (item.get('error_occurred') or type(item.get('repetitions')) is not int
                or type(item.get('repetition_index')) is not int or item['repetitions'] != repetitions
                or item['repetition_index'] != len(series)):
            raise ValueError(f'invalid NFR repetition: {path}')
        batch = re.search(r'/(\d+)(?:/process_time)?$', name) if '_Batch_' in name else None
        deliveries = item.get('deliveries_per_iteration', int(batch[1]) if batch else 1)
        if not all(type(value) in (int, float) and math.isfinite(value) and value > 0
                   for value in (deliveries, item['cpu_time'], item['real_time'])):
            raise ValueError(f'invalid NFR measurement: {path}')
        scale = NANOSECONDS[item['time_unit']] / deliveries
        values = item['cpu_time'] * scale, item['real_time'] * scale
        if not all(math.isfinite(value) and value > 0 for value in values):
            raise ValueError(f'invalid NFR duration: {path}')
        series.append(values)
    if any(len(series) != repetitions for series in rows.values()):
        raise ValueError(f'incomplete NFR series: {path}')
    return rows


def abba(directory, name):
    grouped = {'baseline': [], 'candidate': []}
    files = []
    for index, (side, count) in enumerate(ORDER, 1):
        path = directory / f'{index}-{side}.json'
        grouped[side].extend(samples(path, (name,), count)[name])
        files.append(path)
    cpu, real, cv = [], [], []
    for measurements in grouped.values():
        cpu_times, real_times = zip(*measurements)
        cpu.append(statistics.median(cpu_times))
        real.append(statistics.median(real_times))
        cv.append(100 * statistics.stdev(cpu_times) / statistics.mean(cpu_times))
    status = 'noisy' if max(cv) > 5 else 'regression' if cpu[1] * 20 > cpu[0] * 21 else 'pass'
    return {'status': status, 'cpu_median': cpu, 'real_median': real, 'cpu_cv': cv,
            'delta_percent': 100 * (cpu[1] / cpu[0] - 1)}, files


def audit_series(raw):
    results, files = {}, []
    for name in PATHS:
        folder = raw / name.replace('/', '-')
        result, used = abba(folder, name)
        files.extend(used)
        retry = folder.with_name(folder.name + '-repeat')
        if result['status'] == 'noisy':
            result, used = abba(retry, name)
            files.extend(used)
        elif retry.exists() or retry.is_symlink():
            raise ValueError(f'unexpected NFR retry: {name}')
        results[name] = result
    profile_data = raw / 'profiles.json'
    samples(profile_data, PROFILES, 9)
    files.append(profile_data)
    return results, files


def retain(scratch, destination, base, candidate, exit_code):
    """Publish only expected regular files after checking the actual ABBA samples."""
    metadata_path = scratch / 'nfr-build.json'
    metadata = json.loads(regular(metadata_path).read_text())
    if (metadata['base'] != base or metadata['candidate_sources'] != candidate
            or metadata['baseline_sources'] == candidate
            or metadata['configuration'] != 'RelWithDebInfo'):
        raise ValueError('NFR source or build identity mismatch')
    if metadata['binary_sha256'] != {
        side: file_hash(regular(scratch / (side + '-build') / 'tiny_mq'))
        for side in ('baseline', 'candidate')
    }:
        raise ValueError('NFR binaries changed')
    results, files = audit_series(scratch / 'raw')
    statuses = {row['status'] for row in results.values()}
    if exit_code != (0 if statuses == {'pass'} else 1):
        raise ValueError('NFR process exit and raw samples disagree')
    verdict = 'rejected' if 'regression' in statuses else 'unobserved' if 'noisy' in statuses else 'passed'
    destination.mkdir(mode=0o700)
    manifest = {}
    for source in (*files, metadata_path):
        relative = source.relative_to(scratch)
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(regular(source), target)
        manifest[str(relative)] = file_hash(target)
    report = destination / 'verification.json'
    report.write_text(json.dumps({'files': manifest, 'paths': results, 'technical_verdict': verdict},
                                 indent=2, allow_nan=False) + '\n')
    return {'path': str(report), 'sha256': file_hash(report), 'base': base, 'candidate_sources': candidate,
            'findings': {name: row for name, row in results.items() if row['status'] != 'pass'},
            'max_cpu_delta_percent': max(row['delta_percent'] for row in results.values())}, verdict


def current(root, evidence):
    report = regular(_safe_path(root, evidence['path']))
    if file_hash(report) != evidence['sha256']:
        return False
    manifest = json.loads(report.read_text())['files']
    return all(file_hash(regular(_safe_path(report.parent, name))) == digest
               for name, digest in manifest.items())
