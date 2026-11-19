"""REQ-79-1/6: native change inputs and archival navigation."""
import importlib
import json
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
        (self.change / 'design.md').write_text('# Design\n## Architecture\nA local runtime.\n## Rollback\nRestore previous entrypoint with audit retained.\n## Entity inventory\n| Runtime | type | controller |\n')
        (self.change / 'tasks.md').write_text('# Work\n## Autonomy Level\nR1 decisions remain Owner.\n## Stop-conditions\nMissing authority or stale input blocks.\n## Evidence\nOwn checks and raw logs.\n## Work\n- [ ] implement step\n## Scope\n```json\n["code.py"]\n```\n')
        spec = self.change / 'specs/demo/spec.md'; spec.parent.mkdir(parents=True)
        spec.write_text('# Demo\n## ADDED Requirements\n### Requirement: Example\nSystem SHALL preserve evidence.\nID: `REQ-79-1`\nИсточник: repo:openspec/changes/79-demo/proposal.md@' + self.revision + '#outcome\n#### Scenario: Example\n- **WHEN** invoked\n- **THEN** observable result\n- Test: `DemoTest.Example`\n')
        (self.root / 'code.py').write_text('value = 1\n')
        tests = self.root / 'tests'; tests.mkdir()
        (tests / 'DemoTest.cpp').write_text('// REQ-79-1\nTEST(DemoTest, Example) {}\n')
        chain = self.root / '.claude/chain'; chain.mkdir(parents=True)
        chain.joinpath('chain.env').write_text('HANDOFF_DIR=handoffs/current\nOPENSPEC_DIR=openspec\nCHAIN_MAX_DISPATCH=12\nCHAIN_MAX_ITER=5\n' +
            ''.join('MODEL_' + role.upper() + '=' + ('deepseek-flash' if role == 'explainer' else 'deepseek-v4-pro' if role in ('critic','reviewer','security','nfr') else 'gpt-6.1-sol') + '\n'
                    for role in ('critic','producer','reviewer','conformance','nfr','security','docwriter','explainer')))
        chain.joinpath('workflow-checks.json').write_text(json.dumps({'version':1,'default_profile':'harness','profiles':{'harness':{
            'code_paths':['code.py','tests/DemoTest.cpp'], 'gates':['reviewer','conformance','security'],
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

if __name__ == '__main__': unittest.main()
