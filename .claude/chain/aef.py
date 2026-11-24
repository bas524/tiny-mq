#!/usr/bin/env python3
"""Installed local AEF entry: ./.claude/chain/aef.py status|next|record-decision."""
import argparse
import json
import sys
from pathlib import Path
sys.dont_write_bytecode = True
from workflow_runtime import Runtime


def main(argv=None, runtime=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation',choices=('status','prepare','next','record-decision','verify','resume','archive','provider-probe','request-exception','audit'))
    parser.add_argument('change')
    parser.add_argument('--decision',help='trusted coordinator JSON file; no model flags')
    parser.add_argument('--reason')
    parser.add_argument('--profile', help='select a check profile when preparing a change')
    parser.add_argument('--actor',default='reviewer')
    args=parser.parse_args(argv)
    try:
        if args.profile is not None and args.operation != 'prepare':
            raise ValueError('--profile applies only to prepare')
        runtime=runtime or Runtime(Path(__file__).resolve().parents[2], change=args.change, profile=args.profile)
        if args.operation in ('record-decision','request-exception'):
            if not args.decision: raise ValueError('--decision required')
            result=getattr(runtime,args.operation.replace('-','_'))(args.change,json.loads(Path(args.decision).read_text()))
        elif args.operation=='resume':
            result=runtime.resume(args.change,args.reason or '')
        elif args.operation in ('verify','provider-probe'):
            result=getattr(runtime,args.operation.replace('-','_'))(args.change,actor=args.actor)
        else:
            result=getattr(runtime,args.operation)(args.change)
        print(json.dumps(result,ensure_ascii=False,sort_keys=True));return 0
    except (ValueError,OSError,KeyError) as error:
        print(json.dumps({'state':'blocked','error':str(error)},ensure_ascii=False),file=sys.stderr);return 1


if __name__=='__main__':
    sys.exit(main())
