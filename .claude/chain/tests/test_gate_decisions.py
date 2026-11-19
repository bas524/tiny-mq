"""REQ-79-3: observable authorization semantics, independent of router internals."""
import copy
import importlib
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


class GateDecisionTest(unittest.TestCase):
    def setUp(self):
        self.policy = importlib.import_module('workflow_policy')
        self.result = {'gate': 'nfr', 'technical_verdict': 'rejected',
                       'receipt_hash': 'measurement-v1', 'requirement_ids': ['REQ-72-9']}
        self.subject = {'contract_hash': 'contract-v1', 'action': 'continue-after-nfr',
                        'requirement_ids': ['REQ-72-9'], 'exception_gates': ['nfr']}
        self.decision = {'kind': 'exception', 'owner': 'Owner', 'contract_hash': 'contract-v1',
                         'gate': 'nfr', 'receipt_hash': 'measurement-v1',
                         'requirement_ids': ['REQ-72-9'], 'actions': ['continue-after-nfr'],
                         'debt': {'path': 'tasks/M-05.md', 'revision': 'debt-v1'},
                         'source': {'thread_id': 'real-thread', 'message_id': 'real-message',
                                    'quote': 'Разрешаю этот переход с долгом M-05'}}
        self.source = {'provenance': 'user', 'role': 'user', 'thread_id': 'real-thread', 'message_id': 'real-message',
                       'text': 'Разрешаю этот переход с долгом M-05'}
        self.debt = {'status': 'open', 'revision': 'debt-v1', 'requirement_ids': ['REQ-72-9']}

    def evaluate(self, decision=None, result=None, subject=None):
        return self.policy.evaluate_gate(result or self.result, subject or self.subject, decision,
            source_reader=lambda ref: self.source, debt_reader=lambda ref: self.debt)

    def test_rejected_is_not_pass(self):
        """REQ-79-3: legacy approval cannot turn a failed measurement into pass."""
        for extra in ({}, {'status': 'approved'}, {'status': 'approved', 'performance_verdict': 'rejected'}):
            with self.subTest(extra=extra):
                result = self.evaluate(result={**self.result, **extra})
                self.assertEqual(result['technical_verdict'], 'rejected')
                self.assertEqual(result['transition'], 'blocked')
        allowed = self.evaluate(self.decision)
        self.assertEqual(allowed['technical_verdict'], 'rejected')
        self.assertEqual(allowed['transition'], 'allowed')
        self.assertEqual(allowed['decision_source'], self.decision['source'])

    def test_exact_exception_binding(self):
        """REQ-79-3: stale/forged source, revision, action, REQ or debt blocks."""
        self.assertEqual(self.evaluate(self.decision)['transition'], 'allowed')
        changes = [('contract_hash', 'old'), ('receipt_hash', 'old'),
                   ('requirement_ids', ['REQ-72-8']), ('actions', ['merge']), ('gate', 'security')]
        for key, value in changes:
            with self.subTest(key=key):
                decision = copy.deepcopy(self.decision); decision[key] = value
                self.assertEqual(self.evaluate(decision)['transition'], 'blocked')
        for replacement in ({'role': 'assistant', 'text': self.source['text']},
                            {'role': 'user', 'text': 'Другая инструкция'},
                            {**self.source, 'message_id': 'invented'},
                            {**self.source, 'thread_id': 'other'},
                            {**self.source, 'provenance': 'tool'}, None):
            with self.subTest(source=replacement):
                original = self.source; self.source = replacement
                self.assertEqual(self.evaluate(self.decision)['transition'], 'blocked')
                self.source = original
        for replacement in ({**self.debt, 'status': 'closed'},
                            {**self.debt, 'revision': 'old'},
                            {**self.debt, 'requirement_ids': ['REQ-72-8']}, None):
            with self.subTest(debt=replacement):
                original = self.debt; self.debt = replacement
                self.assertEqual(self.evaluate(self.decision)['transition'], 'blocked')
                self.debt = original

    def test_unwaivable_blockers(self):
        """REQ-79-3: an NFR exception is not a general release override."""
        for blocker in ('scope', 'source_integrity', 'independence', 'isolation', 'security', 'required_ci'):
            with self.subTest(blocker=blocker):
                subject = {**self.subject, 'blockers': [blocker]}
                self.assertEqual(self.evaluate(self.decision, subject=subject)['transition'], 'blocked')
        for gate in ('reviewer', 'security', 'conformance'):
            with self.subTest(gate=gate):
                result = {**self.result, 'gate': gate}
                decision = {**self.decision, 'gate': gate}
                self.assertEqual(self.evaluate(decision, result=result)['transition'], 'blocked')

if __name__ == '__main__': unittest.main()
