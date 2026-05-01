#!/usr/bin/env python3
"""Test runner for the Efto interpreter.

Discovers .efto test files, parses expect directives from comments,
runs each through the interpreter, and compares actual vs expected output.

Directives (in comments):
    // expect: <text>                 - expected stdout line (ordered)
    // expect error: <text>           - expected stderr line (compile error, exit 65)
    // expect runtime error: <text>   - expected first stderr line (runtime error, exit 70)
    // expect exit: <N>               - override expected exit code

Usage:
    python run_tests.py                          # run all tests
    python run_tests.py tests/expressions/       # run a directory
    python run_tests.py tests/literals/nil.efto  # run a single test
"""

import os
import re
import subprocess
import sys
from pathlib import Path

INTERPRETER_PATHS = [
    "out/build/x64-Debug/interpreter.exe",
    "out/build/x64-Debug/interpreter",
    "build/Debug/interpreter.exe",
    "build/Debug/interpreter",
    "build/interpreter.exe",
    "build/interpreter",
]

RE_EXPECT_OUTPUT = re.compile(r"// expect:\s?(.*)$")
RE_EXPECT_ERROR = re.compile(r"// expect error:\s?(.*)$")
RE_EXPECT_RUNTIME_ERROR = re.compile(r"// expect runtime error:\s?(.*)$")
RE_EXPECT_EXIT = re.compile(r"// expect exit:\s*(\d+)$")

TIMEOUT = 10  # seconds per test


def find_interpreter():
    root = Path(__file__).parent
    for rel in INTERPRETER_PATHS:
        path = root / rel
        if path.exists():
            return str(path)
    print("Error: interpreter binary not found. Build the project first.")
    print("Searched:", [str(root / r) for r in INTERPRETER_PATHS])
    sys.exit(1)


def parse_expectations(filepath):
    expected_stdout = []
    expected_errors = []
    expected_runtime_error = None
    expected_exit = 0

    with open(filepath, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")

            m = RE_EXPECT_OUTPUT.search(line)
            if m:
                expected_stdout.append(m.group(1))
                continue

            m = RE_EXPECT_ERROR.search(line)
            if m:
                expected_errors.append(m.group(1))
                if expected_exit == 0:
                    expected_exit = 65
                continue

            m = RE_EXPECT_RUNTIME_ERROR.search(line)
            if m:
                expected_runtime_error = m.group(1)
                if expected_exit == 0:
                    expected_exit = 70
                continue

            m = RE_EXPECT_EXIT.search(line)
            if m:
                expected_exit = int(m.group(1))

    return expected_stdout, expected_errors, expected_runtime_error, expected_exit


def run_test(interpreter, filepath):
    """Run a single test. Returns (passed: bool, failures: list[str])."""
    expected_stdout, expected_errors, expected_runtime_error, expected_exit = (
        parse_expectations(filepath)
    )

    try:
        result = subprocess.run(
            [interpreter, filepath],
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return False, ["TIMEOUT: test exceeded %d seconds" % TIMEOUT]

    failures = []

    # Check stdout
    actual_stdout = result.stdout.splitlines() if result.stdout else []
    if actual_stdout != expected_stdout:
        failures.append("stdout mismatch:")
        max_lines = max(len(actual_stdout), len(expected_stdout))
        for i in range(max_lines):
            exp = expected_stdout[i] if i < len(expected_stdout) else "<missing>"
            act = actual_stdout[i] if i < len(actual_stdout) else "<missing>"
            if exp != act:
                failures.append("  line %d: expected %r, got %r" % (i + 1, exp, act))

    # Check compile errors
    actual_stderr = result.stderr if result.stderr else ""
    for expected_err in expected_errors:
        if expected_err not in actual_stderr:
            failures.append("expected error not found in stderr:")
            failures.append("  expected: %r" % expected_err)

    # Check runtime error
    if expected_runtime_error is not None:
        stderr_first_line = actual_stderr.splitlines()[0] if actual_stderr.strip() else ""
        if expected_runtime_error not in stderr_first_line:
            failures.append("runtime error mismatch:")
            failures.append("  expected: %r" % expected_runtime_error)
            failures.append("  got:      %r" % stderr_first_line)

    # Check exit code
    if result.returncode != expected_exit:
        failures.append(
            "exit code: expected %d, got %d" % (expected_exit, result.returncode)
        )

    return len(failures) == 0, failures


SKIP_DIRS = {"attachments"}


def discover_tests(paths):
    """Find all .efto files from given paths (files or directories).

    Skips directories whose name is in SKIP_DIRS (e.g. 'attachments'),
    which contain module files used by tests but are not tests themselves.
    """
    tests = []
    for p in paths:
        path = Path(p)
        if path.is_file() and path.suffix == ".efto":
            tests.append(str(path))
        elif path.is_dir():
            for f in sorted(path.rglob("*.efto")):
                if not any(part in SKIP_DIRS for part in f.parts):
                    tests.append(str(f))
    return tests


def main():
    interpreter = find_interpreter()

    if len(sys.argv) > 1:
        paths = sys.argv[1:]
    else:
        paths = ["tests"]

    tests = discover_tests(paths)
    if not tests:
        print("No .efto test files found.")
        sys.exit(1)

    passed = 0
    failed = 0
    failed_tests = []

    for filepath in tests:
        ok, failures = run_test(interpreter, filepath)
        rel = os.path.relpath(filepath)
        if ok:
            print("  PASS  %s" % rel)
            passed += 1
        else:
            print("  FAIL  %s" % rel)
            for f in failures:
                print("        %s" % f)
            failed += 1
            failed_tests.append(rel)

    print()
    print("--- Results: %d passed, %d failed, %d total ---" % (passed, failed, passed + failed))

    if failed_tests:
        print()
        print("Failed tests:")
        for t in failed_tests:
            print("  %s" % t)
        sys.exit(1)


if __name__ == "__main__":
    main()
