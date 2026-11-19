"""Fail-closed authorization. A model verdict is never an Owner decision."""
import hashlib
import json
from pathlib import Path


def _hash(text):
    return hashlib.sha256(text.encode()).hexdigest()


def resolve_codex_source(ref, sessions_dir=None):
    """Resolve full item identity across current and archived local JSONL sessions.

    This executes only in the trusted coordinator, never in a model tool sandbox.
    No credentials/configuration are opened.
    """
    home = Path.home() / '.codex'
    roots = [Path(sessions_dir)] if sessions_dir else [home / 'sessions', home / 'archived_sessions']
    candidates = []
    quote = ref.get('quote', ref.get('text'))
    if 'quote' in ref and 'text' in ref and ref['quote']!=ref['text']:
        raise ValueError('quote/text mismatch')
    if not all(ref.get(k) for k in ('thread_id','message_id','turn_id')) or not isinstance(quote, str):
        raise ValueError('source requires thread/message/turn and exact quote/text')
    for root in roots:
        for path in sorted(root.rglob('*.jsonl')):
            session, items = None, []
            with path.open(encoding='utf-8') as stream:
                for line in stream:
                    try:
                        item = json.loads(line)
                    except ValueError:
                        raise ValueError(f'invalid source JSONL: {path}')
                    payload = item.get('payload', {})
                    if item.get('type') == 'session_meta':
                        if session is not None and session != payload.get('id'):
                            raise ValueError('conflicting session identity')
                        session = payload.get('id')
                    if item.get('type') == 'response_item' and payload.get('id') == ref['message_id']:
                        items.append(item)
            for item in items:
                if session != ref['thread_id']:
                    continue
                candidates.append((path, item))
    if not candidates:
        raise ValueError('unknown source message/session')
    variants = {json.dumps(item, sort_keys=True, ensure_ascii=False) for _, item in candidates}
    if len(variants) != 1:
        raise ValueError('conflicting duplicate message identity')
    path, item = candidates[0]
    payload = item['payload']
    metadata = payload.get('internal_chat_message_metadata_passthrough', {})
    content = payload.get('content', [])
    provenance = metadata.get('content_item_metadata', [])
    text = ''.join(c.get('text','') for c in content if c.get('type') == 'input_text')
    if (payload.get('type') != 'message' or payload.get('role') != 'user'
            or metadata.get('turn_id') != ref['turn_id'] or not content
            or any(c.get('type') != 'input_text' for c in content)
            or len(provenance) != len(content)
            or any(c.get('provenance',{}).get('type') != 'user' for c in provenance)
            or '<codex_delegation' in text or text != quote
            or (ref.get('text_sha256') and ref['text_sha256'] != _hash(text))):
        raise ValueError('source identity/revision/provenance mismatch')
    retained = item.get('metadata',{}).get('retained_source',{})
    if ref.get('revision') and ref['revision'] != retained.get('revision'):
        raise ValueError('source revision mismatch')
    return {'role':'user','provenance':'user','thread_id':ref['thread_id'], 'message_id':payload['id'],
            'turn_id':metadata['turn_id'], 'text':text, 'text_sha256':_hash(text),
            'revision':retained.get('revision'), 'source_path':str(path), 'record':item}


def valid_source(source, ref):
    return bool(source and source.get('role') == 'user' and source.get('provenance') == 'user'
        and source.get('message_id') == ref.get('message_id') and source.get('thread_id') == ref.get('thread_id')
        and source.get('text') == ref.get('quote', ref.get('text'))
        and (not ref.get('turn_id') or source.get('turn_id') == ref['turn_id']))


def evaluate_gate(result, subject, decision=None, *, source_reader=None, debt_reader=None):
    verdict = result.get('technical_verdict', 'unobserved')
    if result.get('performance_verdict') == 'rejected':
        verdict = 'rejected'
    out = {'technical_verdict':verdict,'transition':'blocked'}
    if subject.get('blockers'):
        return out
    if verdict in ('passed','not_applicable'):
        out['transition'] = 'allowed'
        return out
    if verdict != 'rejected' or result.get('gate') != 'nfr' or not decision:
        return out
    if ('nfr' not in subject.get('exception_gates', []) or decision.get('kind') != 'exception'
            or decision.get('gate') != 'nfr' or not decision.get('owner')
            or decision.get('contract_hash') != subject.get('contract_hash')
            or not result.get('receipt_hash') or decision.get('receipt_hash') != result['receipt_hash']
            or set(decision.get('requirement_ids', [])) != set(subject.get('requirement_ids', []))
            or set(result.get('requirement_ids', [])) != set(subject.get('requirement_ids', []))
            or decision.get('actions') != [subject.get('action')]):
        return out
    try:
        ref = decision['source']; source = source_reader(ref)
        debt_ref = decision['debt']; debt = debt_reader(debt_ref)
        if (not valid_source(source, ref) or not debt or debt.get('status') != 'open'
                or debt.get('revision') != debt_ref.get('revision')
                or set(debt.get('requirement_ids',[])) != set(subject.get('requirement_ids',[]))):
            return out
    except (KeyError, ValueError, OSError, TypeError):
        return out
    out.update(transition='allowed', decision_source=ref)
    return out
