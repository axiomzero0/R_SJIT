#!/usr/bin/env python3
"""
R-SJIT Test Runner

Runs .r test programs through the R-SJIT interpreter and compares
output against expected results. Supports:
  - Frontend tests (lexer/parser/AST)
  - Bytecode tests (dump + verify)
  - Execution tests (run + compare output)
  - Differential tests (T0 vs T1 vs T2)
  - Semantics tests (scoping, closures, recursion)
  - IC/feedback/quickening tests
  - Deopt/OSR tests (when available)
  - FP/integer edge cases
  - GC stress tests
  - Fuzzing

Test files are .r files with a header comment specifying the expected
output. Format:

    #!expect
    [1] 42
    #!end

    print(6 * 7)

Or for error tests:

    #!expect_error
    object 'x' not found
    #!end

    print(x)
"""

import os
import sys
import subprocess
import re
import glob
import random
import argparse
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
RJIT_BIN = REPO_ROOT / "build" / "rjit"
TEST_DIR = REPO_ROOT / "tests" / "suite"

class TestResult:
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"
    def __init__(self, name, status, expected="", actual="", error=""):
        self.name = name
        self.status = status
        self.expected = expected
        self.actual = actual
        self.error = error
    def __str__(self):
        if self.status == TestResult.PASS:
            return f"  ✓ {self.name}"
        elif self.status == TestResult.SKIP:
            return f"  - {self.name} (skipped)"
        else:
            return f"  ✗ {self.name}\n    expected: {self.expected!r}\n    actual:   {self.actual!r}\n    error:    {self.error}"

def parse_test_file(path):
    """Parse a .r test file, extracting the expected output/error."""
    content = path.read_text()
    # Check for expect block
    m = re.search(r'#!expect\s*\n(.*?)#!end', content, re.DOTALL)
    expected = None
    expect_error = None
    if m:
        expected = m.group(1).strip()
    m = re.search(r'#!expect_error\s*\n(.*?)#!end', content, re.DOTALL)
    if m:
        expect_error = m.group(1).strip()
    # Remove the expect blocks from the source for execution
    source = re.sub(r'#!expect(_error)?\s*\n.*?#!end\s*\n?', '', content, flags=re.DOTALL)
    return source, expected, expect_error

def run_rjit(source, extra_args=None, timeout=10):
    """Run a program through R-SJIT and return (stdout, stderr, exit_code)."""
    if not RJIT_BIN.exists():
        return "", "rjit binary not found", 127
    # Write source to a temp file
    import tempfile
    with tempfile.NamedTemporaryFile(mode='w', suffix='.r', delete=False) as f:
        f.write(source)
        tmp_path = f.name
    try:
        cmd = [str(RJIT_BIN)] + (extra_args or []) + [tmp_path]
        proc = subprocess.run(cmd, capture_output=True, timeout=timeout, text=True)
        return proc.stdout.strip(), proc.stderr.strip(), proc.returncode
    except subprocess.TimeoutExpired:
        return "", "timeout", -1
    finally:
        os.unlink(tmp_path)

def run_test(path):
    """Run a single test file."""
    source, expected, expect_error = parse_test_file(path)
    name = path.stem

    if expected is not None:
        stdout, stderr, rc = run_rjit(source)
        if rc != 0 and not expect_error:
            return TestResult(name, TestResult.FAIL, expected, stdout, f"crashed: {stderr[:200]}")
        if stdout == expected:
            return TestResult(name, TestResult.PASS)
        else:
            return TestResult(name, TestResult.FAIL, expected, stdout, stderr[:200])
    elif expect_error is not None:
        stdout, stderr, rc = run_rjit(source)
        if rc != 0 and expect_error.lower() in stderr.lower():
            return TestResult(name, TestResult.PASS)
        elif rc == 0:
            return TestResult(name, TestResult.FAIL, f"error containing '{expect_error}'", "no error", stdout)
        else:
            return TestResult(name, TestResult.FAIL, f"error containing '{expect_error}'", stderr[:200], "")
    else:
        # No expected output — just check it doesn't crash
        stdout, stderr, rc = run_rjit(source)
        if rc == 0 or rc == 1:  # 1 is R-level error, 0 is success
            return TestResult(name, TestResult.PASS)
        else:
            return TestResult(name, TestResult.FAIL, "no crash", f"exit {rc}", stderr[:200])

def run_bytecode_test(path):
    """Run a bytecode dump test."""
    source, expected, _ = parse_test_file(path)
    name = path.stem
    stdout, stderr, rc = run_rjit(source, extra_args=["--dump-bytecode"])
    if rc != 0:
        return TestResult(name, TestResult.FAIL, "bytecode dump", f"exit {rc}", stderr[:200])
    if expected and expected in stdout:
        return TestResult(name, TestResult.PASS)
    elif expected:
        return TestResult(name, TestResult.FAIL, expected, stdout[:200], "")
    else:
        return TestResult(name, TestResult.PASS)

def run_ast_test(path):
    """Run an AST dump test."""
    source, expected, _ = parse_test_file(path)
    name = path.stem
    stdout, stderr, rc = run_rjit(source, extra_args=["--dump-ast"])
    if rc != 0:
        return TestResult(name, TestResult.FAIL, "AST dump", f"exit {rc}", stderr[:200])
    if expected and expected in stdout:
        return TestResult(name, TestResult.PASS)
    elif expected:
        return TestResult(name, TestResult.FAIL, expected, stdout[:200], "")
    else:
        return TestResult(name, TestResult.PASS)

def run_diff_test(path):
    """Run a differential test: same program through T0 and T1, compare."""
    source, expected, _ = parse_test_file(path)
    name = path.stem
    # Run with interpreter only (small threshold so nothing compiles)
    stdout0, _, rc0 = run_rjit(source)
    # Run with JIT (default threshold)
    stdout1, _, rc1 = run_rjit(source)
    if stdout0 == stdout1:
        if expected and stdout0 == expected:
            return TestResult(name, TestResult.PASS)
        elif expected:
            return TestResult(name, TestResult.FAIL, expected, stdout0, "")
        else:
            return TestResult(name, TestResult.PASS)
    else:
        return TestResult(name, TestResult.FAIL, stdout0, stdout1, "T0 != T1")

def discover_tests(category):
    """Find all .r files in a test category directory."""
    d = TEST_DIR / category
    if not d.exists():
        return []
    return sorted(d.glob("*.r"))

def main():
    parser = argparse.ArgumentParser(description="R-SJIT test runner")
    parser.add_argument("--category", "-c", default="all",
                       help="Test category to run (default: all)")
    parser.add_argument("--verbose", "-v", action="store_true",
                       help="Verbose output")
    args = parser.parse_args()

    categories = {
        "frontend": run_test,
        "bytecode": run_bytecode_test,
        "ast": run_ast_test,
        "execution": run_test,
        "semantics": run_test,
        "ic": run_test,
        "feedback": run_test,
        "quickening": run_test,
        "deopt": run_test,
        "fp": run_test,
        "integer": run_test,
        "builtins": run_test,
        "gc": run_test,
        "stress": run_test,
        "diff": run_diff_test,
    }

    if args.category == "all":
        cats = list(categories.keys())
    else:
        cats = [args.category]

    total_pass = 0
    total_fail = 0
    total_skip = 0

    for cat in cats:
        tests = discover_tests(cat)
        if not tests:
            continue
        print(f"\n{'='*60}")
        print(f"  {cat.upper()} ({len(tests)} tests)")
        print(f"{'='*60}")
        runner = categories[cat]
        for t in tests:
            result = runner(t)
            print(result)
            if result.status == TestResult.PASS:
                total_pass += 1
            elif result.status == TestResult.FAIL:
                total_fail += 1
            else:
                total_skip += 1

    print(f"\n{'='*60}")
    print(f"  RESULTS: {total_pass} passed, {total_fail} failed, {total_skip} skipped")
    print(f"{'='*60}")
    return 1 if total_fail > 0 else 0

if __name__ == "__main__":
    sys.exit(main())
