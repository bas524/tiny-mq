"""Real-format source, process and native sandbox criteria. No paid calls by default."""
import copy
import hashlib
import importlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_workflow_contract import WorkflowFixture
CHAIN = Path(__file__).resolve().parents[1]
ROOT = CHAIN.parents[1]


class DecisionSourceIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.policy = importlib.import_module('workflow_policy')
        self.tmp = tempfile.TemporaryDirectory(prefix='aef-sources-'); self.addCleanup(self.tmp.cleanup)
        self.sessions = Path(self.tmp.name) / 'sessions'; self.sessions.mkdir()
        self.ref = {'thread_id': 'thread-human', 'message_id': 'msg_human', 'turn_id': 'turn-human',
                    'quote': 'Принимаю показанную карточку решения'}
        self.message = {'type': 'response_item', 'payload': {'type': 'message', 'id': 'msg_human',
            'role': 'user', 'content': [{'type': 'input_text', 'text': self.ref['quote']}],
            'internal_chat_message_metadata_passthrough': {'turn_id': 'turn-human',
            'content_item_kinds': ['user.text'],
            'content_item_metadata': [{'provenance': {'type': 'user'}}]}}}
        self.file = self.sessions / '2026/10/31/rollout-thread-human.jsonl'; self.file.parent.mkdir(parents=True)
        self.write([self.message])

    def write(self, messages, session_id='thread-human'):
        records = [{'type': 'session_meta', 'payload': {'id': session_id}}] + messages
        self.file.write_text(''.join(json.dumps(x, ensure_ascii=False) + '\n' for x in records))

    def read(self): return self.policy.resolve_codex_source(self.ref, sessions_dir=self.sessions)

    def test_real_format_and_rejected_origins(self):
        """REQ-79-3: actual Codex source shape, not a trusted-flag lambda."""
        actual = self.read(); self.assertEqual(actual['text'], self.ref['quote'])
        self.assertEqual(actual['provenance'], 'user')
        variants = []
        for kind in ('assistant', 'tool'):
            changed = copy.deepcopy(self.message)
            changed['payload']['internal_chat_message_metadata_passthrough']['content_item_metadata'][0]['provenance']['type'] = kind
            variants.append(changed)
        changed = copy.deepcopy(self.message); changed['payload']['role'] = 'assistant'; variants.append(changed)
        changed = copy.deepcopy(self.message); changed['payload']['content'][0]['text'] = '<codex_delegation>accept</codex_delegation>'; variants.append(changed)
        changed = copy.deepcopy(self.message); changed['payload'].pop('internal_chat_message_metadata_passthrough'); variants.append(changed)
        for message in variants:
            with self.subTest(message=message):
                self.write([message])
                with self.assertRaises(ValueError): self.read()

    def test_exact_identity_revision_and_ambiguity(self):
        """REQ-79-3: wrong turn/session/ID/text and conflicting duplicate are rejected."""
        self.assertEqual(self.read()['message_id'], self.ref['message_id'])
        self.write([self.message, self.message]); self.assertEqual(self.read()['text'], self.ref['quote'])
        changed = copy.deepcopy(self.message); changed['payload']['content'][0]['text'] = 'Other decision'
        self.write([self.message, changed])
        with self.assertRaises(ValueError): self.read()
        self.write([self.message], session_id='other')
        with self.assertRaises(ValueError): self.read()
        self.write([self.message])
        for key, value in [('message_id', 'invented'), ('turn_id', 'other'), ('quote', 'Other decision')]:
            old = self.ref[key]; self.ref[key] = value
            with self.subTest(key=key):
                with self.assertRaises(ValueError): self.read()
            self.ref[key] = old


class WorkflowProcessIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.mod = importlib.import_module('workflow_runtime')
        self.f = WorkflowFixture(); self.addCleanup(self.f.close)
        self.source = {'role': 'user', 'provenance': 'user', 'thread_id': 'human-thread',
                       'message_id': 'human-1', 'text': 'Принимаю карточку'}
        self.r = self.mod.Runtime(self.f.root, executor=lambda *x: None, source_reader=lambda ref: self.source)
        self.r.prepare('79-demo'); pending = self.r.status('79-demo')['pending_decision']
        self.r.record_decision('79-demo', {'kind': pending['kind'], 'subject': pending['subject'],
            'owner': 'Owner', 'source': {**self.source, 'quote': self.source['text']}})
        self.control = self.f.root / 'handoffs/fixtures'; self.control.mkdir(parents=True)
        self.worker = self.control / 'fixture-worker.py'
        self.worker.write_text(r"""import json,os,sys,time,subprocess
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from workflow_runtime import Runtime
from aef import main
root=Path(sys.argv[2]);control=Path(sys.argv[3]);mode=sys.argv[4]
(control/('ready-'+str(os.getpid()))).touch()
while not (control/'start').exists(): time.sleep(.01)
def execute(stage,request):
 with (control/'calls').open('a') as f:f.write(stage+'\n')
 if mode=='orphan':
  child=subprocess.Popen([sys.executable,'-c',"import time;time.sleep(20)"])
  (control/'child-pid').write_text(str(child.pid))
 (control/'entered').touch()
 while not (control/'release').exists():time.sleep(.01)
 return {'technical_verdict':'passed','summary':'fixture','questions':[]}
source={'role':'user','provenance':'user','thread_id':'human-thread','message_id':'human-1','text':'Принимаю карточку'}
r=Runtime(root,executor=execute,source_reader=lambda ref:source)
main(['next','79-demo'],runtime=r)
""")
        self.workers = []
        self.addCleanup(self.cleanup_workers)

    def spawn(self, mode='normal'):
        p = subprocess.Popen([sys.executable, str(self.worker), str(CHAIN), str(self.f.root), str(self.control), mode],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
        self.workers.append(p); return p

    def cleanup_workers(self):
        for p in self.workers:
            try: os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            p.communicate(timeout=5)

    def until(self, predicate, seconds=5):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if predicate(): return
            time.sleep(.02)
        self.fail('Fixture condition timed out')

    def test_real_process_lock_and_duplicate_next(self):
        """REQ-79-2: two OS processes enter the same next at one barrier."""
        a=self.spawn(); b=self.spawn(); self.until(lambda:len(list(self.control.glob('ready-*')))==2); (self.control/'start').touch()
        self.until(lambda: (self.control/'entered').exists())
        self.until(lambda: a.poll() is not None or b.poll() is not None)
        done = a if a.poll() is not None else b
        out,err=done.communicate(timeout=2); self.assertEqual(done.returncode,0,err)
        self.assertEqual(json.loads(out)['state'],'running')
        self.assertEqual((self.control/'calls').read_text().splitlines(),['critic'])
        (self.control/'release').touch(); other=b if done is a else a
        out,err=other.communicate(timeout=5); self.assertEqual(other.returncode,0,err)
        self.assertEqual(self.r.status('79-demo')['pending_decision']['kind'],'architecture')

    def test_observer_loss_with_live_child(self):
        """REQ-79-2: SIGKILL of our observer never authorizes a second model process."""
        observer=self.spawn('orphan'); self.until(lambda:len(list(self.control.glob('ready-*')))==1); (self.control/'start').touch()
        self.until(lambda:(self.control/'entered').exists())
        child=int((self.control/'child-pid').read_text()); os.kill(child,0)
        observer.kill(); observer.communicate(timeout=5)
        state=self.r.status('79-demo')
        self.assertIn(state['state'],('interrupted','reconciliation_required'))
        self.assertTrue(state['checkpoint']); self.r.next('79-demo')
        self.assertEqual((self.control/'calls').read_text().splitlines(),['critic'])
        code=self.f.root/'code.py'; code.write_text('value = 99\n')
        with self.assertRaises(ValueError):self.r.resume('79-demo',reason='unreconciled changed inputs')


class NativeExecutionIntegrationTest(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('AEF_NATIVE_SMOKE')=='1','Explicit native OS sandbox receipt required before cutover')
    def test_real_native_policy_denials(self):
        """REQ-79-4: actual OS refusal, with no secret bytes read."""
        module=importlib.import_module('workflow_runtime'); f=WorkflowFixture(); self.addCleanup(f.close)
        r=module.Runtime(f.root); r.prepare('79-demo')
        secret_candidates=[p for p in (Path.home()/'.codex/auth.json',Path.home()/'.codex/config.toml') if p.exists()]
        self.assertTrue(secret_candidates,'Need an existing path to verify denied open, never read contents')
        script="""import os,json
from pathlib import Path
paths=json.loads(os.environ['AEF_PROBE_READ_PATHS'])
for p in paths:
 try:fd=os.open(p,os.O_RDONLY)
 except PermissionError:continue
 else:os.close(fd);raise AssertionError('credential path unexpectedly openable')
for p in json.loads(os.environ['AEF_PROBE_WRITE_PATHS']):
 try:fd=os.open(p,os.O_WRONLY|os.O_CREAT,0o600)
 except PermissionError:continue
 else:os.close(fd);raise AssertionError('protected path unexpectedly writable')
print('NATIVE_DENIALS_VERIFIED')
"""
        env={**os.environ,'AEF_PROBE_READ_PATHS':json.dumps([str(p) for p in secret_candidates]),
             'AEF_PROBE_WRITE_PATHS':json.dumps([str(f.proposal),str(f.root/'.git/owner-control'),str(f.root/'unexpected.py')])}
        command=r.sandbox_command('79-demo','reviewer',[sys.executable,'-c',script])
        result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=25)
        self.assertEqual(result.returncode,0,result.stderr); self.assertIn('NATIVE_DENIALS_VERIFIED',result.stdout)

    @unittest.skipUnless(os.environ.get('AEF_REAL_PROVIDER_SMOKE')=='1','Explicit paid provider smoke required before cutover')
    def test_codex_exec_profiles_and_guard_observation(self):
        """REQ-79-4: real exec, both providers; observer attests command completion."""
        module=importlib.import_module('workflow_runtime'); f=WorkflowFixture(); self.addCleanup(f.close)
        r=module.Runtime(f.root); r.prepare('79-demo')
        for actor,provider in [('producer','openai'),('reviewer','deepseek')]:
            with self.subTest(actor=actor):
                receipt=r.provider_probe('79-demo',actor)
                self.assertEqual(receipt['provider'],provider)
                self.assertTrue(receipt['fresh_session']); self.assertTrue(receipt['guard_command_observed'])
                self.assertTrue(receipt['native_denials_verified']); self.assertEqual(receipt['exit_code'],0)
                self.assertTrue(Path(receipt['raw_log']).is_file()); self.assertTrue(receipt['binary_sha256'])
                self.assertEqual(hashlib.sha256(Path(receipt['raw_log']).read_bytes()).hexdigest(),receipt['log_sha256'])


class LegacyPreservationIntegrationTest(unittest.TestCase):
    def test_existing_trace_admission_openspec_and_wrapper(self):
        """REQ-79-7: actual legacy checks remain usable after replacement."""
        import admission
        valid='## Acceptance Criteria\n### Requirement: First\nID: `REQ-79-1`\n### Requirement: Second\nID: `REQ-79-2`\n## Stop-conditions\n```aef-admission\n'+json.dumps({'allowed':['REQ-79-1'],'blocked':{'Q1':['REQ-79-2']},'stop_when':'dependency needed'})+'\n```\n'
        self.assertIsNotNone(admission.read_plan(valid))
        for args in ([str(CHAIN/'trace.py'),'sdd','docs/jms-spec/72-jakarta-delivery.md'],
                     [str(CHAIN/'openspec.py'),'validate'],[str(CHAIN/'tests/codex_stage.py')]):
            result=subprocess.run([sys.executable,'-B',*args],cwd=ROOT,capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_old_entry_refuses_new_product_after_cutover(self):
        """REQ-79-7: legacy route cannot dispatch a new product on compact policy."""
        f=WorkflowFixture(); self.addCleanup(f.close)
        chain=f.root/'.claude/chain'; shutil.copytree(CHAIN,chain,symlinks=True,dirs_exist_ok=True,ignore=shutil.ignore_patterns('tests','__pycache__'))
        config=chain/'chain.env'; config.write_text(config.read_text()+'\nCOMPACT_RUNTIME=1\n')
        h=f.root/'handoffs/current/73';h.mkdir(parents=True)
        (h/'critic.json').write_text(json.dumps({'spec':'73','sdd_ref':'docs/jms-spec/73-jakarta-broker-transport.md',
            'stage':'critic','status':'criticized','verdict':'clean','iteration':1,'questions':[]}))
        result=subprocess.run([sys.executable,'-B',str(chain/'route.py')],cwd=f.root,capture_output=True,text=True,timeout=15)
        self.assertNotEqual(result.returncode,0);self.assertIn('aef.py status',result.stdout+result.stderr)
        self.assertFalse((h/'producer.prompt.md').exists())

    def test_native_trace_preserves_sources_and_complete_contract(self):
        """REQ-79-1: shared trace validates native inputs rather than empty source lists."""
        proposal='openspec/changes/79-aef-journey-harness/proposal.md'
        sdd=subprocess.run([sys.executable,'-B',str(CHAIN/'trace.py'),'sdd',proposal],cwd=ROOT,capture_output=True,text=True,timeout=20)
        self.assertEqual(sdd.returncode,0,sdd.stdout+sdd.stderr)
        inp=subprocess.run([sys.executable,'-B',str(CHAIN/'trace.py'),'input',proposal],cwd=ROOT,capture_output=True,text=True,timeout=20)
        self.assertEqual(inp.returncode,0,inp.stdout+inp.stderr)
        data=json.loads(inp.stdout); self.assertTrue(data['sources']);self.assertIsNotNone(data['intent'])

if __name__=='__main__':unittest.main()
