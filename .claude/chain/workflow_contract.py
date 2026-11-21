"""Native OpenSpec reader. Planned v2 projection and exact materials are distinct."""
import datetime
import hashlib
import json
import os
import re
import stat
import subprocess
from pathlib import Path


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False).encode()).hexdigest()


def _no_links(path, missing=False):
    """Check lexical coordinates before resolve/read, including every parent."""
    path=Path(os.path.abspath(path))
    for item in reversed([path,*path.parents]):
        try: mode=item.lstat().st_mode
        except FileNotFoundError:
            if missing:continue
            raise ValueError(f'{item}: missing input') from None
        if stat.S_ISLNK(mode):raise ValueError(f'{item}: symlink input')
    return path


def _safe_path(root, name, missing=False):
    from admission import safe_path
    if not isinstance(name,str) or not safe_path(name):raise ValueError('unsafe input path')
    path=_no_links(root/name,missing)
    if not path.is_relative_to(root):raise ValueError('input outside component')
    return path


def _read(path):
    path=_no_links(path)
    if not stat.S_ISREG(path.lstat().st_mode):raise ValueError(f'{path}: not a regular file')
    return path.read_bytes().decode('utf-8')


def native(path):
    path=Path(path)
    if path.name!='proposal.md':return False
    path=_no_links(path)
    metadata=_no_links(path.parent/'.openspec.yaml',missing=True)
    return metadata.is_file()


def root_of(path):
    path=_no_links(path)
    result=subprocess.run(['git','-C',str(path if path.is_dir() else path.parent),'rev-parse','--show-toplevel'],capture_output=True,text=True)
    if result.returncode:raise ValueError(result.stderr.strip())
    return _no_links(Path(result.stdout.strip()))


def _visible(text):
    """One visible line per raw line; fenced/comment headings have no semantics."""
    out=[];fence=None;comment=False
    for line in text.splitlines():
        if fence:
            if re.fullmatch(r' {0,3}'+re.escape(fence[0])+'{'+str(fence[1])+r',}[ \t]*',line):fence=None
            out.append('');continue
        visible='';rest=line
        while rest:
            if comment:
                end=rest.find('-->')
                if end<0:rest='';break
                rest=rest[end+3:];comment=False
            else:
                start=rest.find('<!--')
                if start<0:visible+=rest;break
                visible+=rest[:start];rest=rest[start+4:];comment=True
        opening=re.match(r' {0,3}(`{3,}|~{3,})',visible)
        if opening:fence=(opening[1][0],len(opening[1]));visible=''
        out.append(visible)
    if fence or comment:raise ValueError('unclosed fence/comment in contract')
    return out


def _sections(text):
    lines=text.splitlines(keepends=True);visible=_visible(text);found={}
    for i,line in enumerate(visible):
        match=re.fullmatch(r' {0,3}##[ \t]+(.+?)(?:[ \t]+#+)?[ \t]*',line)
        if match:
            name=match[1].strip()
            if name in found:raise ValueError('duplicate section '+name)
            found[name]=[i+1,len(lines),i]
    starts=sorted(found.values())
    for current,nxt in zip(starts,starts[1:]):current[1]=nxt[2]
    return lines,visible,found


def _meaningful(text, where):
    body='\n'.join(_visible(text)).strip()
    if not body or re.fullmatch(r'(?:TODO|TBD|—|-|<[^>\n]*>|\{\{[^\n]*\}\})\.?',body,re.I):
        raise ValueError(where+': empty/placeholder field')
    return body


def _json(text):
    def pairs(items):
        result={}
        for key,value in items:
            if key in result:raise ValueError('duplicate JSON field')
            result[key]=value
        return result
    try:return json.loads(text,object_pairs_hook=pairs,parse_constant=lambda _:(_ for _ in ()).throw(ValueError('non-finite JSON')))
    except (ValueError,TypeError) as error:raise ValueError('invalid typed execution JSON') from error


def _execution(body, name, root):
    match=re.fullmatch(r'\s*```json[ \t]*\r?\n(.*?)\r?\n```[ \t]*\s*',body,re.S)
    if not match:raise ValueError(name+': expected only one fenced json, no prose')
    value=_json(match[1])
    def safe(value):
        _safe_path(root,value,missing=True)
    if name=='Results':
        if (not isinstance(value,dict) or set(value)!={'status','code_head','evidence'}
                or value['status'] not in ('draft','blocked','in_progress','verified')
                or not isinstance(value['code_head'],str) or not re.fullmatch('[0-9a-fA-F]{40}',value['code_head'])
                or not isinstance(value['evidence'],list)):
            raise ValueError('Results: invalid schema')
        check=subprocess.run(['git','-C',str(root),'cat-file','-e',value['code_head']+'^{commit}'],capture_output=True)
        if check.returncode:raise ValueError('Results: unknown code_head')
        for path in value['evidence']:safe(path)
    else:
        if not isinstance(value,list):raise ValueError('Owner decisions: expected array')
        for item in value:
            if (not isinstance(item,dict) or set(item)!={'kind','status','source','subject'}
                    or item['kind'] not in ('business','architecture','acceptance','archive')
                    or item['status'] not in ('approved','pending')
                    or not isinstance(item['subject'],str)):
                raise ValueError('Owner decisions: invalid schema')
            _meaningful(item['subject'],'Owner decisions subject')
            if item['source'] is None:
                if item['status']!='pending':raise ValueError('approved informational decision lacks source')
            else:safe(item['source'])
    return value


def _project_tasks(text, root):
    lines,visible,sections=_sections(text)
    for name in ('Results','Owner decisions'):
        if name not in sections:raise ValueError('missing '+name)
        start,end,_=sections[name];_execution(''.join(lines[start:end]),name,root)
    remove=set()
    for name in ('Results','Owner decisions'):
        start,end,_=sections[name];remove.update(range(start,end))
    result=[]
    work=sections.get('Work')
    for i,line in enumerate(lines):
        if i in remove:continue
        if work and work[0]<=i<work[1] and visible[i].strip():
            line=re.sub(r'^([ \t]*(?:[-+*]|[0-9]+[.)])[ \t]+)\[(?: |x|X)\](?=[ \t]|$)',r'\1[ ]',line)
        result.append(line)
    return ''.join(result)


def _metadata(text):
    values={}
    for line in text.splitlines():
        if not line.strip() or line.lstrip().startswith('#'):continue
        match=re.fullmatch(r'([a-z_]+):[ \t]*(\S.*?)[ \t]*',line)
        if not match or match[1] in values:raise ValueError('invalid/duplicate metadata field')
        key,value=match.groups()
        if key not in ('schema','task_ref','status','version','review_date','origin_task_ref','origin_material_hash'):
            raise ValueError('unknown metadata field '+key)
        if value.startswith('"'):
            try:value=json.loads(value)
            except ValueError:raise ValueError('invalid metadata scalar') from None
        elif value.startswith("'") and value.endswith("'"):value=value[1:-1]
        elif any(c in value for c in '{}[]') or value in ('null','true','false','~'):
            raise ValueError('metadata must be a string scalar')
        if not isinstance(value,str):raise ValueError('metadata must be a string scalar')
        _meaningful(value,'metadata '+key);values[key]=value
    if not {'schema','task_ref','version','review_date'}<=set(values):raise ValueError('missing required metadata')
    return values


def _inventory(body, root):
    rows=[]
    for line in _visible(_read(root/'PROJECT.md')):
        cells=[cell.strip().strip('`') for cell in line.strip().strip('|').split('|')]
        if cells and cells[0]=='ENTITY_KINDS':rows.append(cells)
    if len(rows)!=1 or len(rows[0])!=3:raise ValueError('PROJECT ENTITY_KINDS missing/duplicate/invalid')
    kinds=[kind.strip() for kind in rows[0][1].split('·')]
    if not all(kinds) or len(set(kinds))!=len(kinds):raise ValueError('PROJECT ENTITY_KINDS invalid vocabulary')
    lines=[line.strip() for line in _visible(body) if line.strip()]
    cells=lambda line:[cell.strip() for cell in line.strip('|').split('|')]
    if len(lines)<3 or cells(lines[0])!=['Сущность','Kind','Роль']:
        raise ValueError('Entity inventory: invalid header/empty table')
    if len(cells(lines[1]))!=3 or any(not re.fullmatch(':?-{3,}:?',cell) for cell in cells(lines[1])):
        raise ValueError('Entity inventory: invalid separator')
    seen=set()
    for line in lines[2:]:
        values=cells(line)
        if len(values)!=3 or any(not value for value in values):raise ValueError('Entity inventory: invalid row')
        if values[0] in seen or values[1] not in kinds:raise ValueError('Entity inventory: duplicate entity/unknown Kind')
        seen.add(values[0])


def load_contract(path, historical=False):
    from admission import text_lines, requirement_blocks, block_id, SCN_HDR
    from trace import resolve_source, KbError, block_scenarios
    path=_no_links(path)
    if path.is_dir():path=_no_links(path/'proposal.md')
    root=root_of(path)
    archived=path.is_relative_to(root/'openspec/changes/archive')
    if archived!=historical:raise ValueError('historical archive requires explicit read_only context; active admission forbidden')
    files=[path,path.parent/'design.md',path.parent/'tasks.md',path.parent/'.openspec.yaml']
    specdir=_no_links(path.parent/'specs')
    specs=[]
    for item in sorted(specdir.rglob('*')):
        _no_links(item)
        if item.is_file() and item.suffix=='.md':
            if len(item.relative_to(specdir).parts)!=2 or item.name!='spec.md':raise ValueError('unconsumed spec delta '+str(item))
            specs.append(item)
    if not specs:raise ValueError('missing Acceptance Criteria specs')
    files+=specs
    texts={p:_read(p) for p in files}
    proposal,design,tasks,meta=files[:4]
    sections={p:_sections(texts[p]) for p in files if p!=meta}
    fields={proposal:('Problem','Outcome','Constraints','Success criteria','Cancellation criteria','Expected effect','Owner'),
            design:('Architecture','Rollback','Entity inventory'),tasks:('Autonomy Level','Stop-conditions','Evidence')}
    logical={name:p for p,names in fields.items() for name in names}
    logical['Acceptance Criteria']=specs[0]
    for p,(_,visible,_) in sections.items():
        for line in visible:
            heading=re.fullmatch(r' {0,3}(#{1,6})[ \t]+(.+?)(?:[ \t]+#+)?[ \t]*',line)
            if heading and heading[2] in logical and (logical[heading[2]]!=p or heading[1]!='##'):
                raise ValueError(f'{p}: misplaced logical section {heading[2]}')
    def field(p,heading):
        lines,_,found=sections[p]
        if heading not in found:raise ValueError(f'{p}: missing {heading}')
        start,end,_=found[heading];raw=''.join(lines[start:end])
        _meaningful(raw,str(p)+': '+heading)
        return raw.strip(),start+1,raw
    values={}
    for p,names in fields.items():
        for key in names:
            if key!='Owner':values[key]=field(p,key)
    if not re.match(r'^R[123]\b',_meaningful(values['Autonomy Level'][0],'Autonomy Level')):raise ValueError('Autonomy Level must start with R1/R2/R3')
    _inventory(values['Entity inventory'][2],root)
    visible='\n'.join(sections[proposal][1])
    owners=re.findall(r'^Owner:[ \t]*(.+?)(?:\. Дата:.*)?$',visible,re.M)
    components=re.findall(r'(?:^|\. )Компонент:[ \t]*([^\.\n]+)',visible,re.M)
    if len(owners)!=1 or len(components)!=1:raise ValueError('explicit unique Owner and Компонент required')
    _meaningful(owners[0],'Owner');_meaningful(components[0],'Компонент')
    metadata=_metadata(texts[meta])
    try:
        date=datetime.date.fromisoformat(metadata['review_date'])
        if not historical and not 0<=(datetime.date.today()-date).days<=90:raise ValueError()
    except ValueError:raise ValueError('invalid/stale review_date') from None
    if metadata['schema']!='aef' or metadata['task_ref']!=str(proposal.relative_to(root)):raise ValueError('invalid schema/task_ref')
    _safe_path(root,metadata['task_ref'])
    if historical:
        match=re.fullmatch(r'[0-9]{4}-[0-9]{2}-[0-9]{2}-(.+)',path.parent.name)
        origin=metadata.get('origin_task_ref')
        if not match or origin!='openspec/changes/'+match[1]+'/proposal.md':raise ValueError('invalid archive origin_task_ref')
        _safe_path(root,origin,missing=True)
        if not re.fullmatch('[0-9a-f]{64}',metadata.get('origin_material_hash','')):raise ValueError('invalid archive origin_material_hash')
    elif set(metadata)&{'origin_task_ref','origin_material_hash'}:raise ValueError('archive origin metadata forbidden on active input')
    records=[];sources=[]
    for spec in specs:
        lines=text_lines(texts[spec])
        for name,start,end in requirement_blocks(lines):
            body=lines[start+1:end];joined='\n'.join(body)
            ids=re.findall(r'^ID:\s*`?(REQ-[0-9]{2}-[0-9]+)`?\s*$',joined,re.M)
            rid=block_id(body)
            refs=re.findall(r'^Источник:[ \t]*(\S+)[ \t]*$',joined,re.M)
            tests=re.findall(r'^\s*-?\s*Test:\s*`([^`]+)`',joined,re.M)
            scenarios=block_scenarios(body)
            if len(ids)!=1 or not rid or not refs or len(set(refs))!=len(refs) or not tests or not scenarios or any(not test or not meaningful for _,test,meaningful in scenarios):
                raise ValueError(f'{spec}:{start+1}: requirement needs unique ID/source/scenario/Test')
            if not re.search(r'\b(SHALL|MUST)\b',joined):raise ValueError('missing SHALL/MUST')
            for ref in refs:
                if ref.startswith('repo:'):
                    match=re.fullmatch(r'repo:([^@#]+)@([0-9a-fA-F]{7,40})#([^\s#]+)',ref)
                    if not match:raise ValueError('unsafe repo source')
                    source=_safe_path(root,match[1],missing=historical)
                try:resolved=resolve_source(path,ref)
                except KbError as error:raise ValueError(f'{spec}:{start+1}: {error}') from error
                sources.append(resolved)
                if ref.startswith('repo:') and not historical:
                    pinned=subprocess.run(['git','-C',str(root),'show',match[2]+':'+resolved['path']],capture_output=True)
                    if pinned.returncode or _read(source).encode()!=pinned.stdout:raise ValueError(f'{source}: differs from pinned business source')
            records.append({'id':rid,'name':name,'tests':tests,'sources':refs,'path':str(spec.relative_to(root)),'line':start+1})
    if not records or len({r['id'] for r in records})!=len(records):raise ValueError('missing or duplicate requirement IDs')
    intent={key:values[value][0] for key,value in {'goal':'Outcome','constraints':'Constraints','success_criteria':'Success criteria','cancellation_criteria':'Cancellation criteria','expected_effect':'Expected effect'}.items()}
    criteria='\n\n'.join(re.sub(r'^## .*Requirements\s*$','',texts[p],flags=re.M) for p in specs)
    sdd={key:values[key][0] for key in ('Problem','Outcome','Constraints','Architecture','Rollback','Autonomy Level','Stop-conditions','Evidence')}
    sdd['Outcome']+='\n'+values['Success criteria'][0]
    sdd.update({'Acceptance Criteria':criteria,'Owner':owners[0]})
    virtual=[f'# {path.parent.name}',f'Version: {metadata["version"]}',f'Review Date: {metadata["review_date"]}',''];coordinates={}
    for key,body in sdd.items():
        virtual.append('## '+key)
        if key=='Problem':virtual+=['Источник: '+records[0]['sources'][0],'Компонент: '+components[0]]
        start=len(virtual)+1;virtual+=body.splitlines()+['']
        origin=design if key in ('Architecture','Rollback') else tasks if key in ('Autonomy Level','Stop-conditions','Evidence') else specs[0] if key=='Acceptance Criteria' else proposal
        coordinates[str(start)]={'path':str(origin.relative_to(root)),'line':values.get(key,('',1))[1],'field':key}
    virtual+=['## Entity inventory',values['Entity inventory'][0]]
    projected=dict(texts);projected[tasks]=_project_tasks(texts[tasks],root)
    hashes={str(p.relative_to(root)):hashlib.sha256(texts[p].encode()).hexdigest() for p in files}
    projected_hashes={str(p.relative_to(root)):hashlib.sha256(projected[p].encode()).hexdigest() for p in files}
    acceptance=_no_links(path.parent/'acceptance.md',missing=True)
    acceptance_hash=hashlib.sha256(_read(acceptance).encode()).hexdigest() if acceptance.exists() else None
    return {'version':2,'intent':intent,'sdd':sdd,'requirements':records,'sources':sources,
            'contract_hash':digest({'version':2,'files':projected_hashes}),
            'material_hash':digest({'version':2,'files':hashes,'acceptance':acceptance_hash}),
            'files':hashes,'projected_files':projected_hashes,'text':'\n'.join(virtual)+'\n','line_map':coordinates,
            'metadata':metadata,'read_only':historical}


def contract_view(path):
    return load_contract(path)['text'] if native(path) else Path(path).read_text(encoding='utf-8')


LINK=re.compile(r'(!?\[[^\]]*\]\()([^\s)]+)(\))')


def validate_links(directory):
    directory=_no_links(directory);errors=[]
    for p in directory.rglob('*.md'):
        for match in LINK.finditer(_read(p)):
            target=match[2].split('#')[0]
            if not target or re.match(r'[a-zA-Z][\w+.-]*:',target):continue
            if not (p.parent/target).exists():errors.append(f'{p}: missing {target}')
    return errors


def rebase_archive_links(source,destination):
    source=_no_links(source);destination=Path(os.path.abspath(destination));out={}
    # This remains a pure relocation calculator, including intentionally partial text.
    for p in source.rglob('*.md'):
        _no_links(p)
        def replace(match):
            target,sep,fragment=match[2].partition('#')
            if not target or re.match(r'[a-zA-Z][\w+.-]*:',target):return match[0]
            resolved=(p.parent/target).resolve()
            if resolved.is_relative_to(source):resolved=destination/resolved.relative_to(source)
            new=os.path.relpath(resolved,(destination/p.relative_to(source)).parent)
            return match[1]+new+(sep+fragment if sep else '')+match[3]
        out[str(p.relative_to(source))]=LINK.sub(replace,_read(p))
    try:contract=load_contract(source/'proposal.md')
    except ValueError:return out
    root=root_of(source)
    meta=source/'.openspec.yaml'
    body=_read(meta)
    body=re.sub(r'^task_ref:.*$', 'task_ref: '+str((destination/'proposal.md').relative_to(root)),body,flags=re.M)
    out['.openspec.yaml']=body.rstrip('\n')+'\norigin_task_ref: '+str((source/'proposal.md').relative_to(root))+'\norigin_material_hash: '+contract['material_hash']+'\n'
    return out
