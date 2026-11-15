#!/usr/bin/env python3
"""Check profile selection, proxy removal and interactive-only dispatch without calling a provider."""

import json
import os
import pty
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


CHAIN = Path(__file__).resolve().parent.parent
PROXY_KEYS = ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy", "OPENAI_BASE_URL")


class CodexStageTest(unittest.TestCase):
    def test_provider_arguments_and_proxy_environment(self):
        prompt = "Inspect this prompt exactly.\n`command` $HOME apostrophe:'"
        with tempfile.TemporaryDirectory() as directory:
            stub = Path(directory) / "codex"
            stub.write_text(
                f"#!{sys.executable}\n"
                "import json, os, sys\n"
                f"print(json.dumps([sys.argv[1:], [key for key in {PROXY_KEYS!r} if key in os.environ]]))\n"
            )
            stub.chmod(0o755)
            env = dict(os.environ, PATH=f"{directory}{os.pathsep}{os.environ['PATH']}")
            env.update(dict.fromkeys(PROXY_KEYS, "http://unused.invalid"))
            for name, expected in (
                ("openai", ["-c", 'model_provider="openai"', "--model", "gpt-6.1-sol"]),
                ("deepseek", ["--profile", "deepseek", "-c", 'model_provider="deepseek"']),
                ("deepseek-pro", ["--profile", "deepseek-pro", "-c", 'model_provider="deepseek"']),
            ):
                with self.subTest(name=name):
                    master, terminal = pty.openpty()
                    try:
                        result = subprocess.run(
                            [CHAIN / f"codex-{name}", "-p", prompt],
                            stdin=terminal, env=env, capture_output=True, text=True, check=True, timeout=10,
                        )
                    finally:
                        os.close(terminal)
                        os.close(master)
                    args, proxies = json.loads(result.stdout)
                    self.assertEqual(args[: len(expected)], expected)
                    self.assertEqual(args[-2:], ["--", prompt])
                    self.assertEqual(args[args.index("--ask-for-approval") + 1], "on-request")
                    self.assertEqual(args[args.index("--sandbox") + 1], "workspace-write")
                    self.assertEqual(proxies, [])

    def test_noninteractive_launch_is_rejected(self):
        result = subprocess.run(
            [CHAIN / "codex-deepseek-pro", "-p", "smoke"],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("interactive terminal", result.stderr)


if __name__ == "__main__":
    unittest.main()
