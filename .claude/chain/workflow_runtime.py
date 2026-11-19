"""Trusted local AEF coordinator; model output has no control-plane authority."""
import contextlib
import datetime
import fcntl
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import uuid
from pathlib import Path
from workflow_contract import load_contract, digest, validate_links, rebase_archive_links
from workflow_policy import resolve_codex_source, valid_source, evaluate_gate

ROLES = ('critic','producer','reviewer','conformance','nfr','security','docwriter','explainer')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


class Runtime:
    def __init__(self, root, executor=None, source_reader=None, check_runner=None):
        self.root = Path(root).resolve()
        self.injected = any(x is not None for x in (executor, source_reader, check_runner))
        self.executor = executor or self._execute
        self.source_reader = source_reader or resolve_codex_source
        self.check_runner = check_runner
        self.config = {}
        for line in (self.root / '.claude/chain/chain.env').read_text().splitlines():
            if re.match(r'^[A-Z_]+=',line):
                key, value = line.split('=',1)
                parts = shlex.split(value, comments=True)
                if len(parts) != 1:
                    raise ValueError('non-scalar chain configuration: ' + key)
                self.config[key] = parts[0]
        common = Path(self.git('rev-parse','--git-common-dir').strip())
        self.common = (self.root / common).resolve() if not common.is_absolute() else common.resolve()
        self.control = self.common / 'aef' / hashlib.sha256(str(self.root).encode()).hexdigest()[:24]
        self.registry_path = self.root / '.claude/chain/workflow-checks.json'
        self.registry = json.loads(self.registry_path.read_text())
        if self.registry.get('version') != 1:
            raise ValueError('unsupported check registry')
        self.profile = self.registry['profiles'][self.registry['default_profile']]

    def git(self, *args):
        result = subprocess.run(['git','-c','core.hooksPath=/dev/null',*args],cwd=self.root,capture_output=True,text=True,
                                env={**os.environ,'GIT_OPTIONAL_LOCKS':'0','GIT_CONFIG_GLOBAL':'/dev/null','GIT_CONFIG_NOSYSTEM':'1'})
        if result.returncode:
            raise ValueError(result.stderr.strip())
        return result.stdout

    def _name(self, change):
        if not re.fullmatch(r'[0-9]+-[a-z0-9-]+',change):
            raise ValueError('invalid change id')
        return change

    def _dir(self, change):
        return self.control / self._name(change)

    def _proposal(self, change):
        return self.root / 'openspec/changes' / self._name(change) / 'proposal.md'

    def _read(self, change):
        return json.loads((self._dir(change) / 'state.json').read_text())

    def _save(self, change, state):
        directory = self._dir(change)
        # This directory is never writable by a stage. Atomic publication under OS lock.
        tmp = directory / ('state-' + uuid.uuid4().hex)
        with tmp.open('x') as stream:
            json.dump(state,stream,ensure_ascii=False,sort_keys=True); stream.flush(); os.fsync(stream.fileno())
        os.replace(tmp,directory/'state.json')

    @contextlib.contextmanager
    def _lock(self, change, blocking=True):
        directory = self._dir(change); directory.mkdir(parents=True,exist_ok=True,mode=0o700)
        fd = os.open(directory/'lock',os.O_CREAT|os.O_RDWR,0o600)
        locked = False
        try:
            try:
                fcntl.flock(fd,fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB)); locked=True
            except BlockingIOError:
                pass
            yield locked
        finally:
            if locked:
                fcntl.flock(fd,fcntl.LOCK_UN)
            os.close(fd)

    def _pending(self, state, kind):
        card = {'kind':kind,'contract_hash':state['contract_hash'], 'action':{'business':'critic','architecture':'implement','acceptance':'accept','archive':'archive'}.get(kind,kind),
                'requirement_ids':state['requirement_ids'], 'acceptance_hash':state.get('acceptance_hash'), 'code_subject':self._subject(), 'nonce':uuid.uuid4().hex}
        return {'kind':kind,'subject':digest(card),'card':card,
                'question':json.dumps(card,sort_keys=True,ensure_ascii=False)}

    def _event(self, state, kind, event, **data):
        state['events'].append({'time':now(),'kind':kind,'event':event,**data})

    def _safe_target(self, value):
        path = Path(value)
        if path.is_absolute() or '..' in path.parts or not path.parts:
            raise ValueError('target outside scope: ' + value)
        if path.parts[0] in ('.git','openspec','requirements','handoffs') or 'tests' in path.parts or path.name in ('chain.env','AGENTS.md','PROJECT.md'):
            raise ValueError('protected target: ' + value)
        target = self.root / path
        if target.is_dir():raise ValueError('target must name an exact file')
        if any(p.is_symlink() for p in [target,*target.parents] if p.is_relative_to(self.root)):
            raise ValueError('symlink target: ' + value)
        if not target.resolve().is_relative_to(self.root):
            raise ValueError('target escapes root')
        return str(target)

    def prepare(self, change, targets=None):
        contract = load_contract(self._proposal(change))
        if targets is None:
            scope = re.search(r'## Scope\s*```json\s*(.*?)```',(self._proposal(change).parent/'tasks.md').read_text(),re.S)
            if scope:
                targets = json.loads(scope[1])
            else:
                scope_path = self.root / 'handoffs/current' / change.split('-')[0] / 'implementation-targets.json'
                targets = json.loads(scope_path.read_text())['target_files']
        safe_targets = [self._safe_target(t) for t in targets]
        if not safe_targets or len(set(safe_targets)) != len(safe_targets):
            raise ValueError('empty/duplicate targets')
        with self._lock(change):
            if (self._dir(change)/'state.json').exists():
                state = self._read(change)
                if state['contract_hash'] != contract['contract_hash'] or state['targets'] != safe_targets:
                    raise ValueError('prepare would replace pinned checkpoint')
                return self._public(state)
            # Real repositories must have a clean Git base. Fixture injection is never admission.
            if not self.injected and self.git('status','--porcelain','--untracked-files=all').strip():
                raise ValueError('prepare requires clean Git base')
            output = self.root/'handoffs/current'/change.split('-')[0]/'runtime-output'
            output.mkdir(parents=True,exist_ok=True)
            state = {'change':change,'state':'waiting','contract_hash':contract['contract_hash'],'contract_files':contract['files'],
                     'requirement_ids':[r['id'] for r in contract['requirements']], 'targets':safe_targets,
                     'base':self.git('rev-parse','HEAD').strip(), 'registry_hash':sha(self.registry_path),
                     'models':{r:self.config.get('MODEL_'+r.upper()) for r in ROLES},
                     'attempts':[], 'events':[], 'decisions':[], 'receipts':[], 'probes':[],
                     'dispatches':0,'rounds':1,'stage':'critic','checkpoint':None,'wait_reason':'owner',
                     'pending_decision':None,'acceptance_verified':False,'transition_kinds':['substantive','infrastructure','mechanical'],
                     'unknown_cost':True,'gpt_units':'credits','output':str(output),'audit_location':str(self.common/'aef-audit'/change),
                     'test_only':self.injected}
            state['pending_decision'] = self._pending(state,'business')
            self._event(state,'mechanical','prepared')
            self._save(change,state)
            return self._public(state)

    def _public(self, state):
        result={k:v for k,v in state.items() if k not in ('decisions',)}
        result['owner_interventions']=len(state['decisions'])
        result['technical_verdict']=state.get('technical_verdict','unobserved')
        result['transition']='owner_required' if state.get('pending_decision') else 'allowed' if state['state'] in ('ready','archive_ready','complete') else 'blocked'
        result['gate_results']=state.get('gate_results',{})
        return result

    def status(self, change):
        with self._lock(change,blocking=False) as acquired:
            state = self._read(change)
            if state['state'] == 'running' and acquired:
                state['state'] = 'interrupted'
                self._event(state,'infrastructure','observer_lost')
                self._save(change,state)
            # Status is read-only except recording observed interruption. It never polls a model.
            return self._public(state)

    def _fresh(self, state):
        contract = load_contract(self._proposal(state['change']))
        if contract['contract_hash'] != state['contract_hash'] or sha(self.registry_path) != state['registry_hash']:
            raise ValueError('pinned contract/check policy changed')
        if state['models'] != {r:self.config.get('MODEL_'+r.upper()) for r in ROLES}:
            raise ValueError('pinned model policy changed')

    def record_decision(self, change, decision):
        with self._lock(change,blocking=False) as acquired:
            if not acquired:
                raise ValueError('active execution cannot register decisions')
            state = self._read(change); self._fresh(state)
            if state['state'] in ('running','interrupted'):
                raise ValueError('execution requires reconciliation')
            pending = state.get('pending_decision')
            if (not pending or decision.get('kind') != pending['kind'] or decision.get('subject') != pending['subject']
                    or decision.get('owner') != load_contract(self._proposal(change))['sdd']['Owner']):
                raise ValueError('decision does not bind pending Owner card')
            if pending['card'].get('code_subject')!=self._subject():
                raise ValueError('pending decision code subject changed')
            source = self.source_reader(decision.get('source',{}))
            if not valid_source(source,decision['source']):
                raise ValueError('unverified Owner source')
            if not self.injected:
                # Structured UI answer includes the exact question shown to the Owner.
                text = source['text']
                match = re.fullmatch(r'\s*<send_user_message_question_reply>\s*(.*?)\s*</send_user_message_question_reply>\s*',text,re.S)
                if not match:
                    raise ValueError('need Owner response to exact pending decision card')
                answers = json.loads(match[1])
                if (len(answers)!=1 or answers[0].get('question') != pending['question']
                        or answers[0].get('answer') != 'Approve ' + pending['subject']):
                    raise ValueError('Owner response does not authorize exact card')
            if decision['kind']=='exception':
                card=pending['card']
                receipt=next((r for r in state['receipts'] if r['receipt_hash']==card['receipt_hash']),None)
                bound={**decision,**{k:card[k] for k in ('contract_hash','gate','receipt_hash','requirement_ids','debt')},'actions':[card['action']]}
                result=evaluate_gate({'gate':'nfr','technical_verdict':'rejected','receipt_hash':card['receipt_hash'],'requirement_ids':card['requirement_ids']},
                    {**card,'exception_gates':['nfr']},bound,source_reader=self.source_reader,debt_reader=self._debt)
                if not receipt or not self.receipt_current(change,receipt) or result['transition']!='allowed':
                    raise ValueError('NFR exception no longer applicable')
                state['nfr_exception']={'decision':bound,'result':result,'subject':self._subject()}
                state['technical_verdict']='rejected'
                state.setdefault('gate_results',{})['nfr']=result
                state['stage']='conformance' if 'conformance' in self.profile['gates'] else 'security' if 'security' in self.profile['gates'] else 'docwriter'
            state['decisions'].append({**decision,'verified_source':source,'card':pending['card'],'registered_at':now()})
            state['pending_decision']=None; state['state']='ready'; state['wait_reason']=None
            if decision['kind']=='archive':
                state['state']='archive_ready'
            if decision['kind']=='acceptance':
                if not self._acceptance_current(state) or not self._release_ready(state):
                    raise ValueError('acceptance checks incomplete')
                state['accepted_subject']=self._subject()
                state['pending_decision']=self._pending(state,'archive'); state['state']='waiting'
            self._event(state,'mechanical','owner_decision',decision_kind=decision['kind'],subject=decision['subject'])
            self._save(change,state)
            return self._public(state)

    def _debt(self, ref):
        name=ref.get('path','')
        if not name.startswith('tasks/') or Path(name).is_absolute() or '..' in Path(name).parts:
            raise ValueError('debt must be a local maintenance task')
        path=self.root/name
        if path.is_symlink() or not path.resolve().is_relative_to(self.root):
            raise ValueError('invalid debt path')
        text=path.read_text()
        if not re.search(r'^## Spec ref\s*$',text,re.M):
            raise ValueError('debt lacks Spec ref')
        status=re.search(r'^(?:Status|Состояние|Статус):\s*(.+)$',text,re.M)
        return {'status':'open' if status and status[1].strip().lower() in ('open','открыта','открыто') else 'closed',
                'revision':sha(path),'requirement_ids':sorted(set(re.findall(r'REQ-[0-9]+-[0-9]+',text)))}

    def request_exception(self, change, binding):
        """Trusted coordinator proposes a narrow card; never accepts model-made waivers."""
        with self._lock(change,blocking=False) as acquired:
            if not acquired:raise ValueError('active execution')
            state=self._read(change);self._fresh(state)
            receipt=next((r for r in reversed(state['receipts']) if r['receipt_hash']==binding.get('receipt_hash')),None)
            if (not receipt or receipt['actor']!='nfr' or receipt['technical_verdict']!='rejected'
                    or not self.receipt_current(change,receipt) or receipt['test_only']
                    or binding.get('actions')!=['continue-after-nfr']):
                raise ValueError('only a current observed NFR rejection can be excepted')
            debt=self._debt(binding['debt'])
            requirements=receipt['requirement_ids']
            if debt['status']!='open' or debt['revision']!=binding['debt']['revision'] or set(debt['requirement_ids'])!=set(requirements):
                raise ValueError('exception debt binding mismatch')
            card={'kind':'exception','contract_hash':state['contract_hash'],'action':'continue-after-nfr',
                  'requirement_ids':requirements,'gate':'nfr','receipt_hash':receipt['receipt_hash'],
                  'debt':binding['debt'],'code_subject':self._subject(),'nonce':uuid.uuid4().hex}
            state['pending_decision']={'kind':'exception','subject':digest(card),'card':card,'question':json.dumps(card,sort_keys=True,ensure_ascii=False)}
            state.update(state='waiting',wait_reason='owner');self._save(change,state)
            return self._public(state)

    def wait(self, change, reason):
        if reason not in ('owner','rate_limited','no_progress','budget_limit','environment'):
            raise ValueError('unknown wait reason')
        with self._lock(change):
            state=self._read(change); state.update(state='waiting',wait_reason=reason)
            state['checkpoint']=state.get('checkpoint') or {'subject':self._subject(),'reason':reason,'time':now()}
            self._event(state,'mechanical','wait',reason=reason); self._save(change,state)
            return self._public(state)

    def resume(self, change, reason):
        if not reason.strip():
            raise ValueError('reconciliation reason required')
        with self._lock(change,blocking=False) as acquired:
            if not acquired:
                raise ValueError('observer still owns execution lock')
            state=self._read(change); self._fresh(state)
            checkpoint=state.get('checkpoint')
            if checkpoint and checkpoint['subject'] != self._subject():
                recovered_path=self._dir(change)/'observations'/(str(checkpoint.get('execution'))+'.json')
                if checkpoint.get('stage')!='producer' or not recovered_path.is_file():
                    raise ValueError('checkpoint inputs changed')
                after=json.loads(recovered_path.read_text())
                if after.get('after_subject')!=self._subject():raise ValueError('completed producer output changed')
                self._check_source_scope(state)
            if state['state'] in ('interrupted','running'):
                # Only the independent native exit observer can reconcile a lost coordinator.
                execution=checkpoint.get('execution') if checkpoint else None
                if not execution or not re.fullmatch(r'[a-f0-9]{32}',execution):
                    raise ValueError('missing execution identity')
                observation=self._dir(change)/'observations'/(execution+'.json')
                if not observation.is_file():
                    raise ValueError('orphan execution needs trusted exit observation; PID alone insufficient')
                observed=json.loads(observation.read_text())
                if observed.get('execution')!=execution or observed.get('subject')!=checkpoint['subject']:
                    raise ValueError('exit observer binding mismatch')
                state['recovered_result']=self._observed_result(observed)
                self._event(state,'infrastructure','execution_reconciled',execution=execution,reason=reason)
            if state.get('wait_reason') == 'budget_limit' and (state['dispatches']>=12 or state['rounds']>5):
                raise ValueError('budget checkpoint cannot be reset by resume')
            state.update(state='waiting' if state['pending_decision'] else 'ready',wait_reason='owner' if state['pending_decision'] else None)
            self._event(state,'mechanical','resumed',reason=reason); self._save(change,state)
            return self._public(state)

    def next(self, change):
        with self._lock(change,blocking=False) as acquired:
            if not acquired:
                observed=self._public(self._read(change))
                if observed['state']=='ready':
                    observed.update(state='running',phase='coordinator_preparing')
                return observed
            state=self._read(change)
            if state['state']=='running':
                state['state']='interrupted'; self._event(state,'infrastructure','observer_lost'); self._save(change,state)
            if state['state'] in ('waiting','interrupted','complete') or state.get('pending_decision'):
                return self._public(state)
            self._fresh(state)
            while state['state']=='ready':
                if state['dispatches']>=12 or state['rounds']>5:
                    state.update(state='waiting',wait_reason='budget_limit'); break
                recovered=state.pop('recovered_result',None)
                stage=state['stage']; execution=state['checkpoint']['execution'] if recovered else uuid.uuid4().hex
                state.update(state='running',checkpoint={'subject':self._subject(),'execution':execution,'stage':stage,'time':now()})
                if recovered:
                    attempt=next(a for a in state['attempts'] if a.get('execution')==execution)
                else:
                    attempt={'execution':execution,'stage':stage,'started_at':now(),'cost':None,'cost_unit':'unknown', 'kind':'substantive'}
                    state['attempts'].append(attempt); state['dispatches']+=1
                self._save(change,state)
                request={'change':change,'execution':execution,'policy':self.policy(change,stage),'contract':str(self._proposal(change)), 'output':str(Path(state['output'])/execution)}
                try:
                    if recovered:
                        result=recovered
                    else:
                        with self._execution_streams(state,execution):
                            result=self.executor(stage,request)
                    if not isinstance(result,dict):
                        raise ValueError('unobserved execution result')
                    attempt.update(finished_at=now(),result=result)
                    self._fresh(state)
                    if stage=='producer':self._check_source_scope(state)
                    if stage!='producer' and state['checkpoint']['subject']!=self._subject():
                        raise ValueError('read-only stage changed source scope')
                    if result.get('wait_reason'):
                        state.update(state='waiting',wait_reason=result['wait_reason']); attempt['kind']='infrastructure'; break
                    verdict=result.get('technical_verdict','unobserved')
                    if result.get('questions'):
                        verdict='rejected'
                    receipt=None
                    if stage in ('reviewer','conformance','nfr','security'):
                        receipt=self._verify(change,stage,state)
                        if receipt['technical_verdict']=='rejected':verdict='rejected'
                    state['technical_verdict']=verdict
                    state.setdefault('gate_results',{})[stage]={'technical_verdict':verdict,'execution':execution,'receipt_hash':receipt['receipt_hash'] if receipt else None}
                    permitted_na=(verdict=='not_applicable' and self.profile.get('gate_applicability',{}).get(stage)=='not_applicable')
                    if verdict!='passed' and not permitted_na:
                        state.update(state='waiting',wait_reason='no_progress')
                        if verdict=='rejected':
                            self._event(state,'substantive','rejected',stage=stage); state['rounds']+=1
                            if stage!='critic':
                                state['stage']='producer';state['findings']=result
                        break
                    state['checkpoint']=None; state['state']='ready'
                    if stage=='critic':
                        state['stage']='producer';state['pending_decision']=self._pending(state,'architecture');state['state']='waiting'; break
                    if stage=='explainer':
                        if not self.injected:
                            explanation=Path(self.policy(change,'explainer')['output'])/'acceptance.md'
                            if explanation.is_symlink() or not explanation.is_file() or not explanation.read_text().strip():
                                raise ValueError('independent acceptance.md output missing')
                            canonical=self._proposal(change).parent/'acceptance.md'
                            canonical.write_bytes(explanation.read_bytes())
                            state['acceptance_hash']=sha(canonical)
                        state['acceptance_verified']=self._acceptance_current(state)
                        state['pending_decision']=self._pending(state,'acceptance');state['state']='waiting';break
                    sequence=['producer','reviewer']+[g for g in self.profile['gates'] if g!='reviewer']+['docwriter','explainer']
                    state['stage']=sequence[sequence.index(stage)+1]
                    self._event(state,'mechanical','stage_completed',stage=stage)
                    self._save(change,state)
                except InterruptedError as error:
                    state['state']='interrupted';attempt.update(error=str(error),kind='infrastructure');break
                except (OSError,ValueError,subprocess.SubprocessError) as error:
                    state.update(state='waiting',wait_reason='environment');attempt.update(error=str(error),kind='infrastructure');break
            self._save(change,state)
            return self._public(state)

    @contextlib.contextmanager
    def _execution_streams(self, state, execution):
        # Children must not retain the operator CLI pipe when its observer dies.
        # Production subprocesses also get explicit files in _execute.
        saved=[]; streams=[]
        try:
            for fd, suffix in ((1,'stdout'),(2,'stderr')):
                saved.append((fd,os.dup(fd)))
                stream=open(Path(state['output'])/(execution+'-executor.'+suffix),'ab',buffering=0)
                streams.append(stream);os.dup2(stream.fileno(),fd)
            yield
        finally:
            for fd, duplicate in saved:
                os.dup2(duplicate,fd);os.close(duplicate)
            for stream in streams:stream.close()

    def _check_source_scope(self, state):
        allowed={str(Path(p).relative_to(self.root)) for p in state['targets']}
        changed=set(self.git('diff','--name-only','-z',state['base']).split('\0'))
        changed.update(self.git('ls-files','--others','--exclude-standard','-z').split('\0'))
        changed.discard('')
        if changed-allowed:
            raise ValueError('source writes outside role scope: '+str(sorted(changed-allowed)))
        for name in allowed:self._safe_target(name)

    def _subject(self):
        # Include tracked and untracked content, modes and symlink text; exclude only output and late explanation.
        files=self.git('ls-files','-co','--exclude-standard','-z').split('\0')
        data={}
        for name in sorted(set(files)-{''}):
            if name.startswith('handoffs/') or name.endswith('/acceptance.md'):
                continue
            p=self.root/name
            data[name]=[p.lstat().st_mode,os.readlink(p) if p.is_symlink() else sha(p)] if p.exists() or p.is_symlink() else None
        return digest({'base':self.git('rev-parse','HEAD').strip(),'files':data})

    def _input_hash(self):
        names=set(self.profile.get('code_paths',[]))
        for check in self._checks():names.update(check.get('inputs',[]))
        content={}
        for name in sorted(names):
            path=self.root/name
            if Path(name).is_absolute() or '..' in Path(name).parts or not path.resolve().is_relative_to(self.root):
                raise ValueError('registered dependency escapes root')
            paths=sorted(path.rglob('*')) if path.is_dir() else [path]
            for item in paths:
                if item.is_symlink():
                    if not item.resolve().is_relative_to(self.root) or not item.resolve().is_file():
                        raise ValueError('unresolved symlink dependency: '+str(item))
                    content[str(item.relative_to(self.root))]=[os.readlink(item),sha(item.resolve())]
                    continue
                if item.is_file():
                    content[str(item.relative_to(self.root))]=sha(item)
                elif not item.exists():
                    raise ValueError('missing registered dependency: '+str(item))
        return digest(content)

    def _environment(self):
        binaries={}
        for argv in [c['argv'] for c in self._checks()]+[a for c in self._checks() for a in c.get('environment',[])]:
            executable=shutil.which(argv[0])
            if not executable:raise ValueError('missing check tool: '+argv[0])
            binaries[argv[0]]=sha(Path(executable).resolve())
        data={'python':sys.version,'platform':list(os.uname()),'inputs':self._input_hash(),'binaries':binaries,'registry':sha(self.registry_path), 'env':{k:os.environ.get(k) for k in ('PATH','LANG','LC_ALL','CXX','CC','CMAKE_BUILD_TYPE')}}
        return digest(data)

    def _checks(self):
        checks=self.profile['checks']
        seen=set()
        for check in checks:
            if check['id'] in seen or not check.get('argv') or not all(isinstance(x,str) for x in check['argv']):
                raise ValueError('invalid registered check')
            seen.add(check['id'])
        return checks

    def verify(self, change, actor='reviewer'):
        with self._lock(change):
            state=self._read(change);self._fresh(state)
            receipt=self._verify(change,actor,state);self._save(change,state)
            return receipt

    def _verify(self, change, actor, state):
        if actor not in ROLES:
            raise ValueError('unknown checker')
        subject=self._subject(); environment=self._environment(); results=[]
        log=Path(state['output'])/('check-'+uuid.uuid4().hex+'.json')
        for check in self._checks():
            for argv in check.get('environment',[])+[check['argv']]:
                if self.check_runner:
                    result=self.check_runner(argv,cwd=self.root)
                else:
                    command=self.sandbox_command(change,actor,argv)
                    result=subprocess.run(command,cwd=self.root,env=self._tool_env(state),capture_output=True,text=True,timeout=600)
                results.append({'argv':argv,'exit':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
        log.write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
        receipt={'actor':actor,'subject':subject,'environment_hash':environment,'log_path':str(log),'log_sha256':sha(log),
                 'technical_verdict':'passed' if results and all(r['exit']==0 for r in results) and subject==self._subject() and environment==self._environment() else 'rejected',
                 'observed_at':now(),'requirement_ids':state['requirement_ids'],'test_only':self.injected or state['test_only']}
        if receipt['technical_verdict']=='passed' and self.profile.get('gate_applicability',{}).get(actor)=='not_applicable':
            receipt['technical_verdict']='not_applicable'
        receipt['receipt_hash']=digest(receipt); state['receipts'].append(receipt)
        self._event(state,'mechanical','verified',actor=actor,receipt=receipt['receipt_hash'])
        return receipt

    def receipt_current(self, change, receipt):
        try:
            state=self._read(change);self._fresh(state)
            return (receipt in state['receipts'] and receipt['subject']==self._subject()
                    and receipt['environment_hash']==self._environment() and sha(receipt['log_path'])==receipt['log_sha256'])
        except (ValueError,OSError):
            return False

    def policy(self, change, actor):
        if actor not in ROLES:
            raise ValueError('unknown role')
        state=self._read(change)
        model=state['models'].get(actor)
        if not model:
            raise ValueError('missing role model')
        provider='deepseek' if model.startswith('deepseek-') else 'openai' if model.startswith('gpt-') else None
        if provider is None:
            raise ValueError('unapproved model provider')
        producer=state['models']['producer']; reviewer=state['models']['reviewer']
        if producer.startswith('deepseek')==reviewer.startswith('deepseek'):
            raise ValueError('producer/reviewer provider independence violated')
        output=Path(state['output'])/actor
        writes=state['targets'] if actor=='producer' else []
        return {'actor':actor,'model':model,'provider':provider,'base_url':'https://api.deepseek.com/' if provider=='deepseek' else 'https://api.openai.com/v1/',
                'profile':'deepseek-pro' if provider=='deepseek' and model!='deepseek-flash' else 'deepseek' if provider=='deepseek' else None,
                'write_paths':writes+[str(output)], 'write_source_paths':writes,'output':str(output),
                'fresh_session':True,'network_enabled':False,'mcp_enabled':False,'proxy_enabled':False,'approval_escalation':False}

    def _permissions(self, change, actor):
        policy=self.policy(change,actor)
        rights={':minimal':'read',str(self.root):'read',str(self.common):'read',
                '/Library/Developer/CommandLineTools':'read','/opt/homebrew':'read',
                str(self.common/'aef'):'deny',str(self.common/'aef-audit'):'deny',
                str(Path.home()/'.codex'):'deny',str(Path.home()/'.ssh'):'deny',str(Path.home()/'.config/gh'):'deny'}
        for path in policy['write_paths']:
            rights[path]='write'
        return 'permissions.aef_stage={filesystem={'+','.join(json.dumps(k)+'='+json.dumps(v) for k,v in rights.items())+'},network={enabled=false}}'

    def _tool_env(self, state, actor='reviewer'):
        output=Path(state['output'])/actor;output.mkdir(parents=True,exist_ok=True)
        tmp=output/'tmp';tmp.mkdir(exist_ok=True)
        env={k:v for k,v in os.environ.items() if k in ('PATH','HOME','LANG','LC_ALL','SHELL','USER','LOGNAME','CODEX_HOME')}
        env.update(TMPDIR=str(tmp),TEMP=str(tmp),TMP=str(tmp),PYTHONDONTWRITEBYTECODE='1',GIT_CONFIG_GLOBAL='/dev/null',GIT_CONFIG_NOSYSTEM='1')
        return env

    def sandbox_command(self, change, actor, argv):
        """Real native Codex sandbox, with managed policy included. No model call."""
        if not argv or not all(isinstance(a,str) for a in argv):
            raise ValueError('argv array required')
        binary=shutil.which('codex')
        if not binary:
            raise ValueError('codex unavailable; sandbox cannot be widened')
        return [binary,'sandbox','--include-managed-config','-P','aef_stage','-c',self._permissions(change,actor),'-C',str(self.root),'--',*argv]

    def _codex_command(self, change, actor, last_message):
        binary=shutil.which('codex')
        if not binary:
            raise ValueError('codex unavailable')
        version=subprocess.run([binary,'--version'],capture_output=True,text=True)
        if version.returncode or '0.170.0' not in version.stdout:
            raise ValueError('native permissions require validated Codex 0.170.0')
        policy=self.policy(change,actor)
        command=[binary,'exec','--ignore-user-config','--ignore-rules','--json','--model',policy['model'],'-C',str(self.root),'-o',str(last_message)]
        # No -p file is inherited: direct profiles are fully specified here.
        settings=['default_permissions="aef_stage"', self._permissions(change,actor), 'approval_policy="never"',
                  'features.hooks=false','web_search="disabled"','mcp_servers={}','shell_environment_policy.inherit="none"',
                  'model_provider='+json.dumps(policy['provider'])]
        if policy['provider']=='deepseek':
            # Provider auth stays in the parent/native provider layer; model tools cannot read it.
            settings+=['model_providers.deepseek={name="Direct DeepSeek",base_url="https://api.deepseek.com/",env_key="DEEPSEEK_API_KEY",wire_api="chat"}']
        tool_env=self._tool_env(self._read(change),actor)
        # Never propagate provider credentials to the tool subprocess environment.
        settings+=['shell_environment_policy.set={'+','.join(json.dumps(k)+'='+json.dumps(v) for k,v in tool_env.items() if k!='CODEX_HOME')+'}']
        for setting in settings:
            command+=['-c',setting]
        return command+['-']

    def _execute(self, actor, request):
        change=request['change']; state=self._read(change);policy=self.policy(change,actor)
        output=Path(policy['output']);output.mkdir(parents=True,exist_ok=True)
        # Native sandbox denial checks are prerequisites to any business-data dispatch.
        if not self._smokes_current(state):
            raise ValueError('real native and both direct provider receipts required before business dispatch')
        role={'critic':'spec-critic','docwriter':'doc-writer','conformance':'specialist-conformance','security':'specialist-security','nfr':'specialist-nfr'}.get(actor,actor)
        role_path=self.root/'.claude/agents'/(role+'.md')
        if not role_path.is_file():
            raise ValueError('missing role instruction: '+str(role_path))
        contract=load_contract(self._proposal(change))
        # Reviewer receives canonical inputs and own checks, never author narrative.
        prompt=role_path.read_text()+'\nCanonical contract:\n'+contract['text']+'\nSource coordinates:\n'+json.dumps(contract['line_map'])
        if actor=='producer' and state.get('findings'):
            prompt+='\nIndependent findings to correct:\n'+json.dumps(state['findings'],ensure_ascii=False)
        if actor=='explainer':
            prompt+='\nWrite the independent explanation to '+str(output/'acceptance.md')+'. Checks and observed results: '+json.dumps(state['receipts'],ensure_ascii=False)
        prompt+='\nRegistered gate applicability: '+json.dumps(self.profile.get('gate_applicability',{}))
        prompt+='\nOnly write your role outputs under '+str(output)+'. Return JSON with technical_verdict, questions, summary. Runtime controls transitions and receipts.'
        observer=Path(state['output'])/'observer';observer.mkdir(exist_ok=True)
        last=observer/(request['execution']+'-last.json'); raw=observer/(request['execution']+'.jsonl')
        command=self._codex_command(change,actor,last)
        env=self._tool_env(state,actor)
        # Native provider transport can authenticate separately from tools. Never read secret bytes here.
        if policy['provider']=='deepseek' and os.environ.get('DEEPSEEK_API_KEY'):
            env['DEEPSEEK_API_KEY']=os.environ['DEEPSEEK_API_KEY']
        observations=self._dir(change)/'observations';observations.mkdir(exist_ok=True)
        plan={'execution':request['execution'],'subject':state['checkpoint']['subject'],'argv':command,
              'binary_sha256':sha(Path(command[0]).resolve()),'cwd':str(self.root),'prompt':prompt,
              'raw_log':str(raw),'stderr':str(observer/(request['execution']+'.stderr')),'last':str(last),
              'provider':policy['provider'],'outcome':str(observations/(request['execution']+'.json'))}
        plan_path=observations/(request['execution']+'-plan.json');plan_path.write_text(json.dumps(plan))
        # A separate trusted waiter survives loss of this coordinator and records actual process exit.
        waiter=subprocess.Popen([sys.executable,'-B',str(Path(__file__).resolve()),'observe',str(plan_path)],
                                cwd=self.root,env=env,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL,start_new_session=True)
        waiter.wait()
        if waiter.returncode or not Path(plan['outcome']).is_file():
            raise InterruptedError('native exit observer did not publish a completion')
        return self._observed_result(json.loads(Path(plan['outcome']).read_text()))

    def _observed_result(self, observed):
        if (observed.get('binary_sha256')!=sha(Path(observed['argv'][0]).resolve())
                or sha(observed['raw_log'])!=observed['log_sha256']):
            raise ValueError('native observation changed')
        events=[json.loads(line) for line in Path(observed['raw_log']).read_text().splitlines() if line.strip()]
        actual={k:observed[k] for k in ('argv','exit_code','raw_log','log_sha256','binary_sha256','provider')}
        actual.update(fresh_session=any(e.get('type')=='thread.started' for e in events),
                      usage=[e.get('usage') for e in events if e.get('type')=='turn.completed'])
        if observed['exit_code']:
            return {**actual,'technical_verdict':'unobserved','wait_reason':'rate_limited' if any('429' in json.dumps(e) for e in events) else 'environment'}
        if not actual['fresh_session'] or not any(e.get('type')=='turn.completed' for e in events):
            raise ValueError('missing fresh native session/completion observation')
        if sha(observed['last'])!=observed['last_sha256']:
            raise ValueError('native final claim changed')
        claim=json.loads(Path(observed['last']).read_text())
        return {**claim,**actual}

    def _guard_script(self, change, actor, marker):
        state=self._read(change);output=Path(self.policy(change,actor)['output'])
        # Fixed QA operations, not document-supplied shell. All potential secret accesses are open/close only.
        protected=[str(self._proposal(change)),str(self._dir(change)/'forbidden-new'),str(self.root/'unexpected-aef-probe.py')]
        credentials=[str(Path.home()/p) for p in ('.codex/auth.json','.codex/config.toml','.ssh/id_rsa','.config/gh/hosts.yml')]
        return '''import os,json,pathlib
out=pathlib.Path('''+repr(str(output))+''')
for name in '''+repr(credentials)+''':
 try: fd=os.open(name,os.O_RDONLY)
 except PermissionError: continue
 except FileNotFoundError: continue
 else: os.close(fd);raise SystemExit('credential open allowed')
for name in '''+repr(protected)+''':
 try: fd=os.open(name,os.O_WRONLY|os.O_CREAT,0o600)
 except PermissionError: continue
 else: os.close(fd);raise SystemExit('protected write allowed')
for op in ('symlink','replace'):
 local=out/('guard-'+op)
 try:
  if local.is_symlink() or local.exists():local.unlink()
  if op=='symlink':
   local.symlink_to('''+repr(protected[0])+''')
   fd=os.open(local,os.O_WRONLY);os.close(fd)
  else:
   local.write_text('synthetic')
   os.replace(local,'''+repr(protected[0])+''')
 except PermissionError:pass
 else:raise SystemExit('protected '+op+' allowed')
 finally:
  if local.is_symlink() or local.exists():local.unlink()
print('NATIVE_DENIALS_VERIFIED '''+marker+''')
'''

    @staticmethod
    def _exact_command(command, argv):
        try:
            parts=shlex.split(command)
            if parts==argv:return True
            if len(parts)==3 and Path(parts[0]).name in ('sh','bash','zsh') and parts[1] in ('-c','-lc'):
                return shlex.split(parts[2])==argv
        except ValueError:pass
        return False

    def provider_probe(self, change, actor):
        """Explicit paid opt-in API. Synthetic guard only; never opens business gates by model claim."""
        if self.injected:
            raise ValueError('fixture probes cannot certify production')
        with self._lock(change,blocking=False) as acquired:
            if not acquired:
                raise ValueError('execution active')
            state=self._read(change);self._fresh(state)
            if state['state'] in ('running','interrupted'):
                raise ValueError('probe requires reconciled checkpoint')
            prior_state=state['state']
            probe_attempt={'stage':'provider_probe','actor':actor,'kind':'infrastructure','started_at':now(),'cost':None,'cost_unit':'unknown'}
            state['attempts'].append(probe_attempt)
            state.update(state='running',checkpoint={'subject':self._subject(),'execution':uuid.uuid4().hex,'stage':'provider_probe','time':now()})
            self._save(change,state)
            policy=self.policy(change,actor);output=Path(policy['output']);output.mkdir(parents=True,exist_ok=True)
            observer=Path(state['output'])/'observer';observer.mkdir(exist_ok=True)
            marker=uuid.uuid4().hex;guard=observer/('guard-'+marker+'.py');guard.write_text(self._guard_script(change,actor,marker))
            argv=[sys.executable,'-B',str(guard)]
            # Own OS observation precedes model smoke; refusal cannot fall back to unconfined execution.
            native=subprocess.run(self.sandbox_command(change,actor,argv),env=self._tool_env(state,actor),cwd=self.root,capture_output=True,text=True,timeout=30)
            native_log=observer/(marker+'-native.json');native_log.write_text(json.dumps({'argv':argv,'exit':native.returncode,'stdout':native.stdout,'stderr':native.stderr}))
            if native.returncode or 'NATIVE_DENIALS_VERIFIED '+marker not in native.stdout:
                raise ValueError('native denial probe failed: '+str(native_log))
            raw=observer/(marker+'.jsonl');last=observer/(marker+'-last.json');command=self._codex_command(change,actor,last)
            prompt='Synthetic QA only. Execute exactly this fixed guard command with the terminal tool, then report its exit: '+shlex.join(argv)+'. Do not read other files or invoke any service.'
            env=self._tool_env(state,actor)
            if policy['provider']=='deepseek' and os.environ.get('DEEPSEEK_API_KEY'):
                env['DEEPSEEK_API_KEY']=os.environ['DEEPSEEK_API_KEY']
            with raw.open('x') as stdout,(observer/(marker+'.stderr')).open('x') as stderr:
                result=subprocess.run(command,input=prompt,text=True,env=env,cwd=self.root,stdout=stdout,stderr=stderr,timeout=180)
            events=[json.loads(line) for line in raw.read_text().splitlines() if line.strip()]
            observed=False
            for event in events:
                item=event.get('item',{})
                if (event.get('type')=='item.completed' and item.get('type')=='command_execution'
                        and item.get('exit_code')==0 and self._exact_command(item.get('command',''),argv)
                        and 'NATIVE_DENIALS_VERIFIED '+marker in item.get('aggregated_output','')):
                    observed=True
            receipt={'provider':policy['provider'],'fresh_session':any(e.get('type')=='thread.started' for e in events),
                     'guard_command_observed':observed,'native_denials_verified':True,'exit_code':result.returncode,
                     'raw_log':str(raw),'log_sha256':sha(raw),'binary_sha256':sha(Path(command[0]).resolve()),
                     'argv':command,'native_log':str(native_log),'native_log_sha256':sha(native_log),
                     'policy_hash':digest(self._permissions(change,actor)), 'actor':actor,'observed_at':now(),'test_only':False}
            if result.returncode or not observed or not receipt['fresh_session'] or not any(e.get('type')=='turn.completed' for e in events):
                raise ValueError('provider execution not confirmed by JSONL exit observer: '+str(raw))
            probe_attempt.update(finished_at=now(),exit_code=result.returncode,receipt=receipt)
            state.update(state=prior_state,checkpoint=None)
            state['probes'].append(receipt); self._event(state,'infrastructure','provider_probe',provider=policy['provider']);self._save(change,state)
            return receipt

    def _smokes_current(self, state):
        providers=set()
        for receipt in state['probes']:
            try:
                if (not receipt['test_only'] and receipt['exit_code']==0 and receipt['guard_command_observed']
                        and receipt['native_denials_verified'] and receipt['fresh_session']
                        and receipt['policy_hash']==digest(self._permissions(state['change'],receipt['actor']))
                        and receipt['binary_sha256']==sha(Path(shutil.which('codex')).resolve())
                        and sha(receipt['raw_log'])==receipt['log_sha256'] and sha(receipt['native_log'])==receipt['native_log_sha256']):
                    providers.add(receipt['provider'])
            except (OSError,TypeError,KeyError):
                pass
        return providers=={'openai','deepseek'}

    def _acceptance_current(self, state):
        path=self._proposal(state['change']).parent/'acceptance.md'
        return path.is_file() and not path.is_symlink() and sha(path)==state.get('acceptance_hash') and any(a.get('stage')=='explainer' and a.get('result',{}).get('exit_code')==0 for a in state['attempts'])

    def _release_ready(self, state):
        if state['test_only'] or self.injected or not self._smokes_current(state):
            return False
        for gate in self.profile['gates']:
            if not any(r['actor']==gate and not r['test_only'] and r['technical_verdict'] in ('passed','not_applicable') and self.receipt_current(state['change'],r) for r in state['receipts']):
                return False
        return True

    def audit(self, change):
        directory=self.common/'aef-audit'/self._name(change)
        manifest=json.loads((directory/'manifest.json').read_text())
        for name, expected in manifest.items():
            path=directory/name
            if Path(name).is_absolute() or '..' in Path(name).parts or not path.resolve().is_relative_to(directory.resolve()) or sha(path)!=expected:
                raise ValueError('audit manifest mismatch: '+name)
        actual={str(p.relative_to(directory)) for p in directory.rglob('*') if p.is_file() and p.name!='manifest.json'}
        if actual!=set(manifest):raise ValueError('audit manifest file set mismatch')
        return {'technical_verdict':'passed','manifest':str(directory/'manifest.json'),'files':len(manifest)}

    def archive(self, change):
        with self._lock(change,blocking=False) as acquired:
            if not acquired:
                raise ValueError('execution active')
            state=self._read(change);self._fresh(state)
            if (not state.get('accepted_subject') or state['accepted_subject']!=self._subject()
                    or not self._release_ready(state) or not self._acceptance_current(state)
                    or not any(d['kind']=='archive' and d['card']['code_subject']==self._subject() for d in state['decisions'])):
                raise ValueError('separate current acceptance, audit, archive decision and independent checks required')
            source=self._proposal(change).parent
            if validate_links(source):
                raise ValueError('unresolved links before archive')
            destination=source.parent/'archive'/(datetime.date.today().isoformat()+'-'+change)
            if destination.exists():
                raise ValueError('archive destination exists')
            converted=rebase_archive_links(source,destination)
            # Stable audit lies in git-common-dir, outside this temporary worktree; never just a symlink.
            audit=Path(state['audit_location']);audit.mkdir(parents=True,exist_ok=False)
            if any(p.is_symlink() for p in Path(state['output']).rglob('*')):
                raise ValueError('audit refuses model-created symlinks')
            shutil.copytree(state['output'],audit/'logs',symlinks=False)
            snapshot=audit/'snapshot'
            for name in sorted(set(self.git('ls-files','-co','--exclude-standard','-z').split('\0'))-{''}):
                if name.startswith('handoffs/'):continue
                original=self.root/name
                if original.is_symlink():
                    if not original.resolve().is_relative_to(self.root):
                        raise ValueError('external audit link: '+name)
                    target=snapshot/name;target.parent.mkdir(parents=True,exist_ok=True)
                    target.symlink_to(os.path.relpath(snapshot/original.resolve().relative_to(self.root),target.parent))
                    continue
                if original.is_file():
                    target=snapshot/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(original,target)
            (audit/'state.json').write_text(json.dumps(state,ensure_ascii=False,indent=2))
            manifest={str(p.relative_to(audit)):sha(p) for p in audit.rglob('*') if p.is_file()}
            (audit/'manifest.json').write_text(json.dumps(manifest,sort_keys=True,indent=2))
            self.audit(change)
            from openspec import validate, merged_text, parse_delta
            if validate(change):
                raise ValueError('OpenSpec validation failed')
            updates={}
            for delta in source.glob('specs/*/spec.md'):
                target=self.root/'openspec/specs'/delta.parent.name/'spec.md'
                ops,_=parse_delta(delta);updates[target]=merged_text(target,ops,delta.parent.name)[0]
            for target,text in updates.items():
                target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text)
            for relative,text in converted.items():
                (source/relative).write_text(text)
            destination.parent.mkdir(parents=True,exist_ok=True);shutil.move(str(source),str(destination))
            if validate_links(destination):
                raise ValueError('archive incomplete: unresolved links')
            state.update(state='complete',archive=str(destination));self._event(state,'mechanical','archived',manifest=str(audit/'manifest.json'));self._save(change,state)
            return self._public(state)


def observe(plan_path):
    """Trusted exit observer entry; the plan and completion live outside stage write scope."""
    plan_path=Path(plan_path).resolve()
    plan=json.loads(plan_path.read_text())
    if plan_path.parent!=Path(plan['outcome']).resolve().parent or plan_path.parent.name!='observations':
        raise ValueError('invalid observer control directory')
    if sha(Path(plan['argv'][0]).resolve())!=plan['binary_sha256']:
        raise ValueError('native binary changed before dispatch')
    with Path(plan['raw_log']).open('x') as stdout,Path(plan['stderr']).open('x') as stderr:
        result=subprocess.run(plan['argv'],input=plan['prompt'],text=True,cwd=plan['cwd'],stdout=stdout,stderr=stderr)
    record={k:v for k,v in plan.items() if k!='prompt'}
    record.update(after_subject=Runtime(plan['cwd'])._subject(),exit_code=result.returncode,completed_at=now(),log_sha256=sha(plan['raw_log']),
                  last_sha256=sha(plan['last']) if Path(plan['last']).is_file() else None)
    temporary=Path(plan['outcome']+'.tmp')
    with temporary.open('x') as stream:
        json.dump(record,stream);stream.flush();os.fsync(stream.fileno())
    os.replace(temporary,plan['outcome'])


if __name__=='__main__':
    if len(sys.argv)!=3 or sys.argv[1]!='observe':
        raise SystemExit('trusted observer expects a protected launch plan')
    observe(sys.argv[2])
