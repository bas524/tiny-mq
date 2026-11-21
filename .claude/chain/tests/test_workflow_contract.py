"""REQ-79-1/6: native change inputs and archival navigation."""
import importlib
import json
import shutil
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


class WorkflowFixture:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='aef-contract-')
        self.root = Path(self.tmp.name).resolve()
        (self.root / '.gitignore').write_text('/handoffs/\n')
        self.git('init', '-q'); self.git('config', 'user.name', 'Fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.change = self.root / 'openspec/changes/79-demo'; self.change.mkdir(parents=True)
        self.proposal = self.change / 'proposal.md'
        self.proposal.write_text('# Example\nOwner: Owner\nКомпонент: tiny-mq\n\n' +
            ''.join('## ' + key + '\nMeaningful value for ' + key + '.\n\n' for key in
                    ('Problem', 'Outcome', 'Constraints', 'Success criteria', 'Cancellation criteria', 'Expected effect', 'Journey')))
        self.git('add', '.'); self.git('commit', '-qm', 'pin intent')
        self.revision = self.git('rev-parse', 'HEAD').strip()
        (self.change / '.openspec.yaml').write_text('schema: aef\nversion: 0.1\nreview_date: 2026-11-19\ntask_ref: openspec/changes/79-demo/proposal.md\n')
        (self.change / 'design.md').write_text('# Design\n## Architecture\nA local runtime.\n## Rollback\nRestore previous entrypoint with audit retained.\n## Entity inventory\n| Сущность | Kind | Роль |\n|---|---|---|\n| Runtime | type | controller |\n')
        (self.change / 'tasks.md').write_text('# Work\n## Autonomy Level\nR1 decisions remain Owner.\n## Stop-conditions\nMissing authority or stale input blocks.\n## Evidence\nOwn checks and raw logs.\n## Work\n- [ ] implement step\n## Scope\n```json\n["code.py"]\n```\n')
        spec = self.change / 'specs/demo/spec.md'; spec.parent.mkdir(parents=True)
        spec.write_text('# Demo\n## ADDED Requirements\n### Requirement: Example\nSystem SHALL preserve evidence.\nID: `REQ-79-1`\nИсточник: repo:openspec/changes/79-demo/proposal.md@' + self.revision + '#outcome\n#### Scenario: Example\n- **WHEN** invoked\n- **THEN** observable result\n- Test: `DemoTest.Example`\n')
        (self.root / 'PROJECT.md').write_text('| Слот | Значение | Что это |\n|---|---|---|\n| `ENTITY_KINDS` | type · method · header · storage-field · config · test-suite · concept · frame · API · property | kinds |\n')
        task_file=self.change/'tasks.md'
        task_file.write_text(task_file.read_text()+'\n## Results\n```json\n'+json.dumps({'status':'draft','code_head':self.revision,'evidence':[]})+'\n```\n## Owner decisions\n```json\n[]\n```\n')
        (self.root / 'code.py').write_text('value = 1\n')
        tests = self.root / 'tests'; tests.mkdir()
        (tests / 'DemoTest.cpp').write_text('// REQ-79-1\nTEST(DemoTest, Example) {}\n')
        chain = self.root / '.claude/chain'; chain.mkdir(parents=True)
        chain.joinpath('chain.env').write_text('HANDOFF_DIR=handoffs/current\nOPENSPEC_DIR=openspec\nCHAIN_MAX_DISPATCH=12\nCHAIN_MAX_ITER=5\n' +
            ''.join('MODEL_' + role.upper() + '=' + ('deepseek-flash' if role == 'explainer' else 'deepseek-v4-pro' if role in ('critic','reviewer','security','nfr') else 'gpt-6.1-sol') + '\n'
                    for role in ('critic','producer','reviewer','conformance','nfr','security','docwriter','explainer')))
        chain.joinpath('workflow-checks.json').write_text(json.dumps({'version':1,'default_profile':'harness','profiles':{'harness':{
            'code_paths':['code.py','tests/DemoTest.cpp','.claude/chain'], 'gates':['reviewer','conformance','security'],
            'checks':[{'id':'fixture','argv':[sys.executable,'-c','print("FIXTURE_CHECK_OK")'],
                       'inputs':['code.py','tests/DemoTest.cpp'],'environment':[[sys.executable,'--version']]}]}}}))
        self.git('add', '.'); self.git('commit', '-qm', 'pin contract and criteria fixture')


    def git(self, *args):
        return subprocess.check_output(['git', '-c', 'core.hooksPath=/dev/null', *args], cwd=self.root, text=True, stderr=subprocess.DEVNULL)

    def close(self): self.tmp.cleanup()


class WorkflowContractTest(unittest.TestCase):
    def setUp(self):
        self.contract = importlib.import_module('workflow_contract')
        self.f = WorkflowFixture(); self.addCleanup(self.f.close)

    def test_required_fields(self):
        """REQ-79-1: all five INT and ten SDD meanings must be present."""
        result = self.contract.load_contract(self.f.proposal)
        self.assertEqual(set(result['intent']), {'goal', 'constraints', 'success_criteria', 'cancellation_criteria', 'expected_effect'})
        self.assertEqual(set(result['sdd']), {'Problem', 'Outcome', 'Constraints', 'Architecture', 'Acceptance Criteria', 'Rollback', 'Autonomy Level', 'Stop-conditions', 'Evidence', 'Owner'})
        self.assertIn('Meaningful value for Outcome', result['intent']['goal'])
        self.assertIn('A local runtime', result['sdd']['Architecture'])
        self.assertIn('Owner', result['sdd']['Owner'])
        for name, heading in [('proposal.md', 'Cancellation criteria'), ('design.md', 'Rollback'),
                              ('design.md', 'Entity inventory'), ('tasks.md', 'Stop-conditions')]:
            path = self.f.change / name; original = path.read_text()
            path.write_text(original.replace('## ' + heading, '## Unrelated', 1))
            with self.subTest(name=name, heading=heading):
                with self.assertRaisesRegex(ValueError, heading): self.contract.load_contract(self.f.proposal)
            path.write_text(original)
        self.assertFalse((self.f.change / 'SDD.md').exists())

    def test_sources_and_late_acceptance(self):
        """REQ-79-1: exact source revision; acceptance is a late output."""
        self.assertFalse((self.f.change / 'acceptance.md').exists())
        first = self.contract.load_contract(self.f.proposal)
        spec = self.f.change / 'specs/demo/spec.md'; original = spec.read_text()
        for body in (original.replace(self.f.revision, 'f' * 40), original.replace('#outcome', '#missing')):
            spec.write_text(body)
            with self.assertRaises(ValueError): self.contract.load_contract(self.f.proposal)
        spec.write_text(original)
        (self.f.change / 'acceptance.md').write_text('Explanation, not an approval.\n')
        self.assertEqual(first['contract_hash'], self.contract.load_contract(self.f.proposal)['contract_hash'])
        self.f.proposal.write_text(self.f.proposal.read_text() + '\nChanged meaning.\n')
        with self.assertRaises(ValueError): self.contract.load_contract(self.f.proposal)

    def test_archive_links_and_history(self):
        """REQ-79-6: relative/internal links survive a move; pinned sources remain."""
        target = self.f.root / 'docs/fact.md'; target.parent.mkdir(); target.write_text('Fact\n')
        text = '[fact](../../../docs/fact.md) [design](design.md)\nИсточник: repo:proposal.md@' + self.f.revision + '#outcome\n'
        self.f.proposal.write_text(text)
        destination = self.f.root / 'openspec/changes/archive/2026-11-19-79-demo'
        converted = self.contract.rebase_archive_links(self.f.change, destination)
        self.assertEqual((self.f.change / 'proposal.md').read_text(), text)
        self.assertIn('[fact](../../../../docs/fact.md)', converted['proposal.md'])
        self.assertIn('[design](design.md)', converted['proposal.md'])
        self.assertIn('Источник: repo:proposal.md@' + self.f.revision + '#outcome', converted['proposal.md'])
        self.assertEqual(self.contract.validate_links(self.f.change), [])
        self.f.proposal.write_text('[missing](does-not-exist.md)\n')
        self.assertTrue(self.contract.validate_links(self.f.change))


    def _reject_native(self, name, transform):
        """Exercise reader, actual trace CLI and non-injected prepare separately."""
        from workflow_runtime import Runtime
        f=WorkflowFixture(); self.addCleanup(f.close)
        path=f.change/name; path.write_text(transform(path.read_text()))
        chain=Path(__file__).resolve().parents[1]
        for source in chain.glob('*.py'): shutil.copyfile(source,f.root/'.claude/chain'/source.name)
        f.git('add','.'); f.git('commit','-qm','pin negative contract')
        with self.assertRaises(ValueError): self.contract.load_contract(f.proposal)
        for operation in ('sdd','input'):
            result=subprocess.run([sys.executable,'-B',str(f.root/'.claude/chain/trace.py'),operation,str(f.proposal)],
                cwd=f.root,capture_output=True,text=True,timeout=20)
            self.assertNotEqual(result.returncode,0,result.stdout+result.stderr)
        with self.assertRaises(ValueError): Runtime(f.root).prepare('79-demo')
        self.assertFalse((Runtime(f.root)._dir('79-demo')/'state.json').exists())

    def test_strict_logical_fields(self):
        """REQ-79-1 C1: invalid content is rejected, not merely missing headings."""
        cases=[('tasks.md',lambda s:s.replace('R1 decisions','R9 decisions')),
               ('design.md',lambda s:s.replace('A local runtime.','TODO')),
               ('design.md',lambda s:s.replace('A local runtime.','tBd.')),
               ('design.md',lambda s:s[:s.index('## Entity inventory')]+'## Entity inventory\narbitrary text\n'),
               ('design.md',lambda s:s.replace('| Runtime | type |','| Runtime | unknown |')),
               ('design.md',lambda s:s+'| Runtime | type | duplicate |\n'),
               ('design.md',lambda s:s+'\n## Architecture\nDuplicate.\n'),
               ('design.md',lambda s:s.replace('A local runtime.','<!-- no architecture -->\n```md\n## Architecture\nfake\n```')),
               ('tasks.md',lambda s:s+'\n## Architecture\nWrong file.\n')]
        for index,(name,transform) in enumerate(cases):
            with self.subTest(case=index): self._reject_native(name,transform)
        design=self.f.change/'design.md'; original=design.read_text()
        design.write_text(original.replace('A local runtime.','A local runtime. TODO is an optional note.\n```md\n## Architecture\nexample\n```'))
        self.contract.load_contract(self.f.proposal)
        project=self.f.root/'PROJECT.md'; project.write_text(project.read_text().replace('type · method','method'))
        with self.assertRaises(ValueError):self.contract.load_contract(self.f.proposal)

    def test_canonical_paths_and_shared_int(self):
        """REQ-79-1 C2: canonical/source symlinks and unconsumed deltas fail closed."""
        from workflow_runtime import Runtime
        for name in ('proposal.md','design.md','tasks.md','.openspec.yaml','specs/demo/spec.md'):
            with self.subTest(link=name):
                f=WorkflowFixture(); self.addCleanup(f.close)
                outside=tempfile.TemporaryDirectory(prefix='aef-outside-');self.addCleanup(outside.cleanup)
                path=f.change/name; target=Path(outside.name)/'input.md'; target.write_bytes(path.read_bytes())
                path.unlink();path.symlink_to(target)
                with self.assertRaises(ValueError):self.contract.load_contract(f.proposal)
                with self.assertRaises(ValueError):Runtime(f.root).prepare('79-demo')
        extra=self.f.change/'specs/demo/unconsumed.md';extra.write_text('Unconsumed delta\n')
        with self.assertRaises(ValueError):self.contract.load_contract(self.f.proposal)
        extra.unlink()
        shared=self.f.root/'requirements/intents/INT-01.md';shared.parent.mkdir(parents=True)
        proposal_before=self.f.proposal.read_bytes();shared.write_text('# Shared intent\n## Outcome\nGoal across multiple changes.\n');shared_before=shared.read_bytes();self.f.git('add','.');self.f.git('commit','-qm','shared INT')
        rev=self.f.git('rev-parse','HEAD').strip()
        spec=self.f.change/'specs/demo/spec.md';original=spec.read_text()
        spec.write_text(original.replace('repo:openspec/changes/79-demo/proposal.md@'+self.f.revision,
                                        'repo:requirements/intents/INT-01.md@'+rev))
        self.contract.load_contract(self.f.proposal)
        self.assertEqual(shared.read_bytes(),shared_before)
        self.assertEqual(self.f.proposal.read_bytes(),proposal_before)
        outside=tempfile.TemporaryDirectory(prefix='aef-int-outside-');self.addCleanup(outside.cleanup)
        copied=Path(outside.name)/'intent.md';copied.write_bytes(shared.read_bytes());shared.unlink();shared.symlink_to(copied)
        with self.assertRaises(ValueError):self.contract.load_contract(self.f.proposal)
        spec.write_text(original.replace('repo:openspec/changes/79-demo/proposal.md','repo:../outside.md'))
        with self.assertRaises(ValueError):self.contract.load_contract(self.f.proposal)

    def test_plan_and_material_projection(self):
        """REQ-79-5 C3: only explicitly typed execution content is non-planned."""
        first=self.contract.load_contract(self.f.proposal);tasks=self.f.change/'tasks.md';original=tasks.read_text()
        for marker in ('[x]','[X]'):
            tasks.write_text(original.replace('[ ]',marker))
            current=self.contract.load_contract(self.f.proposal)
            self.assertEqual(first['contract_hash'],current['contract_hash'])
            self.assertNotEqual(first['material_hash'],current['material_hash'])
            self.assertNotEqual(first['files'],current['files'])
        tasks.write_text(original.replace('"draft"','"verified"'))
        current=self.contract.load_contract(self.f.proposal)
        self.assertEqual(first['contract_hash'],current['contract_hash'])
        self.assertNotEqual(first['material_hash'],current['material_hash'])
        decisions=[{'kind':'architecture','status':'pending','source':None,'subject':'fixture only'}]
        tasks.write_text(original.replace('[]\n```\n',json.dumps(decisions)+'\n```\n'))
        self.assertEqual(first['contract_hash'],self.contract.load_contract(self.f.proposal)['contract_hash'])
        for text in (original.replace('implement step','implement different behavior'),
                     original.replace('R1 decisions','R2 decisions'),original+'\n## Other\nNew boundary.\n'):
            tasks.write_text(text)
            self.assertNotEqual(first['contract_hash'],self.contract.load_contract(self.f.proposal)['contract_hash'])
        for text in (original.replace('"status": "draft"','"Autonomy Level": "R2", "status": "draft"'),
                     original.replace('## Results\n','## Results\nRights granted.\n'),
                     original.replace('R1 decisions remain Owner.','').replace('"draft"','"R2"')):
            tasks.write_text(text)
            with self.assertRaises(ValueError):self.contract.load_contract(self.f.proposal)
        tasks.write_text(original)
        late=self.f.change/'acceptance.md';late.write_text('Late explanation\n')
        current=self.contract.load_contract(self.f.proposal)
        self.assertEqual(first['contract_hash'],current['contract_hash'])
        self.assertNotEqual(first['material_hash'],current['material_hash'])

    def test_historical_archive_is_not_active_admission(self):
        """REQ-79-6 C6: relocation is readable, never active authority."""
        from workflow_runtime import Runtime
        target=self.f.root/'docs/fact.md';target.parent.mkdir();target.write_text('Fact\n')
        tasks=self.f.change/'tasks.md';tasks.write_text(tasks.read_text()+'\n## Navigation\n[external](../../../docs/fact.md) [design](design.md)\n')
        original_business=self.f.proposal.read_text()
        before=self.contract.load_contract(self.f.proposal)
        destination=self.f.change.parent/'archive/2026-11-21-79-demo'
        converted=self.contract.rebase_archive_links(self.f.change,destination)
        # Use actual production relocation output; metadata must be included by its helper.
        self.assertIn('.openspec.yaml',converted)
        for name,text in converted.items():(self.f.change/name).write_text(text)
        destination.parent.mkdir();shutil.move(str(self.f.change),destination)
        self.assertEqual(self.contract.validate_links(destination),[])
        meta=destination/'.openspec.yaml';body=meta.read_text()
        self.assertIn('task_ref: '+str((destination/'proposal.md').relative_to(self.f.root)),body)
        self.assertIn('origin_task_ref: openspec/changes/79-demo/proposal.md',body)
        self.assertIn(before['material_hash'],body)
        with self.assertRaises(ValueError):self.contract.load_contract(destination/'proposal.md')
        history=self.contract.load_contract(destination/'proposal.md',historical=True)
        self.assertTrue(history['read_only'])
        self.assertTrue(history['sources'])
        self.assertEqual(self.f.git('show',self.f.revision+':openspec/changes/79-demo/proposal.md'),original_business)
        with self.assertRaises((ValueError,OSError)):Runtime(self.f.root).prepare('archive/2026-11-21-79-demo')
        for bad in (body.replace('origin_material_hash: '+before['material_hash'],'origin_material_hash: broken'),
                    body.replace('origin_task_ref: openspec/changes/79-demo/proposal.md','origin_task_ref: ../outside/proposal.md')):
            meta.write_text(bad)
            with self.assertRaises(ValueError):self.contract.load_contract(destination/'proposal.md',historical=True)
        meta.write_text(body)
        source=destination/'specs/demo/spec.md';source.write_text(source.read_text().replace(self.f.revision,'f'*40))
        with self.assertRaises(ValueError):self.contract.load_contract(destination/'proposal.md',historical=True)

if __name__ == '__main__': unittest.main()
