"""Native OpenSpec contract accessor. No generated SDD is persisted."""
import datetime
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False).encode()).hexdigest()


def native(path):
    path = Path(path)
    return path.name == 'proposal.md' and (path.parent / '.openspec.yaml').is_file()


def root_of(path):
    result = subprocess.run(['git', '-C', str(Path(path).resolve().parent), 'rev-parse', '--show-toplevel'], capture_output=True, text=True)
    if result.returncode:
        raise ValueError(result.stderr.strip())
    return Path(result.stdout.strip()).resolve()


def load_contract(path):
    from admission import text_lines, section, requirement_blocks, block_id, SCN_HDR
    from trace import resolve_source, KbError
    path = Path(path).resolve()
    if path.is_dir():
        path = path / 'proposal.md'
    root = root_of(path)
    files = [path, path.parent / 'design.md', path.parent / 'tasks.md', path.parent / '.openspec.yaml']
    specs = sorted(path.parent.glob('specs/*/spec.md'))
    if not specs:
        raise ValueError(f'{path.parent}/specs: missing Acceptance Criteria')
    files += specs
    texts = {p: p.read_text(encoding='utf-8') for p in files}
    def field(p, heading):
        lines = texts[p].splitlines()
        matches = [i for i, line in enumerate(lines) if line == '## ' + heading]
        if len(matches) != 1:
            raise ValueError(f'{p}: expected one {heading}')
        start = matches[0] + 1
        end = next((i for i in range(start, len(lines)) if lines[i].startswith('## ')), len(lines))
        body = '\n'.join(lines[start:end]).strip()
        if not body or '{{' in body:
            raise ValueError(f'{p}:{start + 1}: empty {heading}')
        return body, start + 1
    proposal, design, tasks, meta = files[:4]
    values = {}
    for key in ('Problem', 'Outcome', 'Constraints', 'Success criteria', 'Cancellation criteria', 'Expected effect'):
        values[key] = field(proposal, key)
    for key in ('Architecture', 'Rollback', 'Entity inventory'):
        values[key] = field(design, key)
    for key in ('Autonomy Level', 'Stop-conditions', 'Evidence'):
        values[key] = field(tasks, key)
    owners = re.findall(r'^Owner:\s*(.+?)(?:\. Дата:.*)?$', texts[proposal], re.M)
    components = re.findall(r'(?:^|\. )Компонент:\s*([^\.\n]+)', texts[proposal], re.M)
    if len(owners) != 1 or len(components) != 1:
        raise ValueError(f'{proposal}: explicit Owner and Компонент required')
    metadata = {}
    for key in ('schema', 'version', 'review_date', 'task_ref'):
        found = re.findall(r'^' + key + r':\s*(\S.*?)\s*$', texts[meta], re.M)
        if len(found) != 1:
            raise ValueError(f'{meta}: missing/duplicate {key}')
        metadata[key] = found[0].strip('\"\'')
    try:
        age=(datetime.date.today()-datetime.date.fromisoformat(metadata['review_date'])).days
        if not 0<=age<=90:raise ValueError('stale review_date')
    except ValueError as error:
        raise ValueError(f'{meta}: invalid/stale review_date') from error
    if metadata['schema'] != 'aef' or metadata['task_ref'] != str(proposal.relative_to(root)):
        raise ValueError(f'{meta}: invalid schema/task_ref')
    records, sources = [], []
    for spec in specs:
        lines = text_lines(texts[spec])
        for name, start, end in requirement_blocks(lines):
            body = lines[start + 1:end]
            rid = block_id(body)
            refs = re.findall(r'^Источник:\s*(\S+)', '\n'.join(body), re.M)
            tests = re.findall(r'^\s*-?\s*Test:\s*`([^`]+)`', '\n'.join(body), re.M)
            from trace import block_scenarios
            scenarios=block_scenarios(body)
            if not rid or not refs or not tests or not scenarios or any(not test or not meaningful for _,test,meaningful in scenarios):
                raise ValueError(f'{spec}:{start+1}: requirement needs ID/source/scenario/Test')
            if not re.search(r'\b(SHALL|MUST)\b', '\n'.join(body)):
                raise ValueError(f'{spec}:{start+1}: missing SHALL/MUST')
            for ref in refs:
                try:
                    resolved = resolve_source(path, ref)
                except KbError as error:
                    raise ValueError(f'{spec}:{start+1}: {error}') from error
                sources.append(resolved)
                # A native intent is pinned before implementation; source drift fails closed.
                if ref.startswith('repo:'):
                    current = root / resolved['path']
                    pinned = subprocess.run(['git', '-C', str(root), 'show', ref.split('@')[1].split('#')[0] + ':' + resolved['path']], capture_output=True)
                    if not current.is_file() or current.read_bytes() != pinned.stdout:
                        raise ValueError(f'{current}: differs from pinned business source')
            records.append({'id': rid, 'name': name, 'tests': tests, 'sources': refs, 'path': str(spec.relative_to(root)), 'line': start+1})
    if not records or len({r['id'] for r in records}) != len(records):
        raise ValueError('missing or duplicate requirement IDs')
    intent = {k: values[v][0] for k, v in {'goal':'Outcome', 'constraints':'Constraints', 'success_criteria':'Success criteria', 'cancellation_criteria':'Cancellation criteria', 'expected_effect':'Expected effect'}.items()}
    criteria = '\n\n'.join(re.sub(r'^## .*Requirements\s*$', '', texts[p], flags=re.M) for p in specs)
    sdd = {k: values[k][0] for k in ('Problem','Outcome','Constraints','Architecture','Rollback','Autonomy Level','Stop-conditions','Evidence')}
    sdd['Outcome'] += '\n' + values['Success criteria'][0]
    sdd.update({'Acceptance Criteria':criteria, 'Owner':owners[0]})
    virtual = [f'# {path.parent.name}', f'Version: {metadata["version"]}', f'Review Date: {metadata["review_date"]}', '']
    coordinates = {}
    for key, body in sdd.items():
        virtual.append('## ' + key)
        if key == 'Problem':
            virtual.extend(['Источник: ' + records[0]['sources'][0], 'Компонент: ' + components[0]])
        start = len(virtual) + 1
        virtual.extend(body.splitlines()); virtual.append('')
        origin = design if key in ('Architecture','Rollback') else tasks if key in ('Autonomy Level','Stop-conditions','Evidence') else specs[0] if key == 'Acceptance Criteria' else proposal
        coordinates[str(start)] = {'path': str(origin.relative_to(root)), 'line': values.get(key, ('',1))[1], 'field':key}
    virtual.extend(['## Entity inventory', values['Entity inventory'][0]])
    file_hashes = {str(p.relative_to(root)):hashlib.sha256(texts[p].encode()).hexdigest() for p in files}
    return {'intent':intent, 'sdd':sdd, 'requirements':records, 'sources':sources, 'contract_hash':digest(file_hashes), 'files':file_hashes, 'text':'\n'.join(virtual)+'\n', 'line_map':coordinates, 'metadata':metadata}


def contract_view(path):
    return load_contract(path)['text'] if native(path) else Path(path).read_text(encoding='utf-8')


LINK = re.compile(r'(!?\[[^\]]*\]\()([^\s)]+)(\))')


def validate_links(directory):
    errors = []
    for p in Path(directory).rglob('*.md'):
        for match in LINK.finditer(p.read_text()):
            target = match[2].split('#')[0]
            if not target or re.match(r'[a-zA-Z][\w+.-]*:', target):
                continue
            if not (p.parent / target).exists():
                errors.append(f'{p}: missing {target}')
    return errors


def rebase_archive_links(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    out = {}
    for p in source.rglob('*.md'):
        def replace(match):
            target, sep, fragment = match[2].partition('#')
            if not target or re.match(r'[a-zA-Z][\w+.-]*:', target):
                return match[0]
            resolved = (p.parent / target).resolve()
            if resolved.is_relative_to(source):
                resolved = destination / resolved.relative_to(source)
            new = os.path.relpath(resolved, (destination / p.relative_to(source)).parent)
            return match[1] + new + (sep + fragment if sep else '') + match[3]
        out[str(p.relative_to(source))] = LINK.sub(replace, p.read_text())
    return out
