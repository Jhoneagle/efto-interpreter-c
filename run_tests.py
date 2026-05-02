#!/usr/bin/env python3
"""Test runner for the Efto interpreter.

Discovers .efto test files, parses expect directives from comments,
runs each through the interpreter, and compares actual vs expected output.

Directives (in comments):
    // expect: <text>                 - expected stdout line (ordered)
    // expect error: <text>           - expected stderr line (compile error, exit 65)
    // expect runtime error: <text>   - expected first stderr line (runtime error, exit 70)
    // expect exit: <N>               - override expected exit code
    // expect no output               - fail if stdout is non-empty
    // [tag]                          - tag this test (e.g. // [stress], // [error])

Usage:
    python run_tests.py                          # run all tests
    python run_tests.py tests/expressions/       # run a directory
    python run_tests.py tests/literals/nil.efto  # run a single test
    python run_tests.py --tag stress             # run only tagged tests
    python run_tests.py --parallel 4             # run with 4 workers
    python run_tests.py --strict                 # treat warnings as failures
"""

import argparse
import concurrent.futures
import glob as globmod
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
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
RE_EXPECT_NO_OUTPUT = re.compile(r"//\s*expect no output\s*$")
RE_TAG = re.compile(r"//\s*\[(\w+)\]")

TIMEOUT = 10  # seconds per test


@dataclass
class TestExpectations:
    stdout: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    runtime_error: str | None = None
    exit_code: int = 0
    tags: set[str] = field(default_factory=set)
    expect_no_output: bool = False
    has_assertions: bool = False


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
    exp = TestExpectations()

    with open(filepath, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")

            m = RE_EXPECT_OUTPUT.search(line)
            if m:
                exp.stdout.append(m.group(1))
                exp.has_assertions = True
                continue

            m = RE_EXPECT_ERROR.search(line)
            if m:
                exp.errors.append(m.group(1))
                exp.has_assertions = True
                if exp.exit_code == 0:
                    exp.exit_code = 65
                continue

            m = RE_EXPECT_RUNTIME_ERROR.search(line)
            if m:
                exp.runtime_error = m.group(1)
                exp.has_assertions = True
                if exp.exit_code == 0:
                    exp.exit_code = 70
                continue

            m = RE_EXPECT_EXIT.search(line)
            if m:
                exp.exit_code = int(m.group(1))
                exp.has_assertions = True

            m = RE_EXPECT_NO_OUTPUT.search(line)
            if m:
                exp.expect_no_output = True
                exp.has_assertions = True

            # Tags can appear on any line.
            for tag_match in RE_TAG.finditer(line):
                exp.tags.add(tag_match.group(1))

    return exp


def run_test(interpreter, filepath):
    """Run a single test. Returns (passed: bool, failures: list[str])."""
    expectations = parse_expectations(filepath)

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

    # Check "expect no output" directive.
    if expectations.expect_no_output:
        if result.stdout and result.stdout.strip():
            failures.append("expected no output but got: %r" % result.stdout.strip())
    else:
        # Check stdout.
        actual_stdout = result.stdout.splitlines() if result.stdout else []
        if actual_stdout != expectations.stdout:
            failures.append("stdout mismatch:")
            max_lines = max(len(actual_stdout), len(expectations.stdout))
            for i in range(max_lines):
                exp = expectations.stdout[i] if i < len(expectations.stdout) else "<missing>"
                act = actual_stdout[i] if i < len(actual_stdout) else "<missing>"
                if exp != act:
                    failures.append("  line %d: expected %r, got %r" % (i + 1, exp, act))

    # Check compile errors.
    actual_stderr = result.stderr if result.stderr else ""
    for expected_err in expectations.errors:
        if expected_err not in actual_stderr:
            failures.append("expected error not found in stderr:")
            failures.append("  expected: %r" % expected_err)

    # Check runtime error (exact first-line match).
    if expectations.runtime_error is not None:
        stderr_first_line = actual_stderr.splitlines()[0] if actual_stderr.strip() else ""
        if stderr_first_line != expectations.runtime_error:
            failures.append("runtime error mismatch:")
            failures.append("  expected: %r" % expectations.runtime_error)
            failures.append("  got:      %r" % stderr_first_line)

    # Check exit code.
    if result.returncode != expectations.exit_code:
        failures.append(
            "exit code: expected %d, got %d" % (expectations.exit_code, result.returncode)
        )

    # Clean up temp files created by the test.
    test_dir = os.path.dirname(os.path.abspath(filepath))
    for pattern in ["_test_tmp*"]:
        for tmp_file in globmod.glob(os.path.join(test_dir, pattern)):
            try:
                os.remove(tmp_file)
            except OSError:
                pass

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


def run_single_test(args):
    """Wrapper for parallel execution."""
    interpreter, filepath = args
    ok, failures = run_test(interpreter, filepath)
    return filepath, ok, failures


def main():
    arg_parser = argparse.ArgumentParser(description="Efto test runner")
    arg_parser.add_argument("paths", nargs="*", default=["tests"],
                            help="Test files or directories to run")
    arg_parser.add_argument("--tag", action="append", default=[],
                            help="Run only tests with this tag (repeatable)")
    arg_parser.add_argument("--parallel", type=int, default=1,
                            help="Number of parallel workers (default: 1)")
    arg_parser.add_argument("--strict", action="store_true",
                            help="Treat warnings (e.g. no assertions) as failures")
    args = arg_parser.parse_args()

    interpreter = find_interpreter()

    tests = discover_tests(args.paths)
    if not tests:
        print("No .efto test files found.")
        sys.exit(1)

    # Pre-parse expectations for tag filtering and assertion validation.
    test_expectations = {}
    for filepath in tests:
        test_expectations[filepath] = parse_expectations(filepath)

    # Filter by tags if specified.
    if args.tag:
        tag_set = set(args.tag)
        tests = [t for t in tests if test_expectations[t].tags & tag_set]
        if not tests:
            print("No tests matched tags: %s" % ", ".join(args.tag))
            sys.exit(1)

    passed = 0
    failed = 0
    failed_tests = []
    warnings = []
    output_lines = []

    def record(line):
        print(line)
        output_lines.append(line)

    # Check for tests without assertions.
    for filepath in tests:
        if not test_expectations[filepath].has_assertions:
            rel = os.path.relpath(filepath)
            warnings.append("WARNING: %s has no expect directives" % rel)

    if args.parallel > 1:
        # Parallel execution.
        test_args = [(interpreter, fp) for fp in tests]
        results = {}
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.parallel) as executor:
            futures = {executor.submit(run_single_test, ta): ta[1] for ta in test_args}
            for future in concurrent.futures.as_completed(futures):
                filepath, ok, failures = future.result()
                results[filepath] = (ok, failures)

        # Print in original discovery order for deterministic output.
        for filepath in tests:
            ok, failures = results[filepath]
            rel = os.path.relpath(filepath)
            if ok:
                record("  PASS  %s" % rel)
                passed += 1
            else:
                record("  FAIL  %s" % rel)
                for f in failures:
                    record("        %s" % f)
                failed += 1
                failed_tests.append(rel)
    else:
        # Sequential execution.
        for filepath in tests:
            ok, failures = run_test(interpreter, filepath)
            rel = os.path.relpath(filepath)
            if ok:
                record("  PASS  %s" % rel)
                passed += 1
            else:
                record("  FAIL  %s" % rel)
                for f in failures:
                    record("        %s" % f)
                failed += 1
                failed_tests.append(rel)

    record("")
    record("--- Results: %d passed, %d failed, %d total ---" % (passed, failed, passed + failed))

    if warnings:
        record("")
        for w in warnings:
            record(w)
        if args.strict:
            failed += len(warnings)

    if failed_tests:
        record("")
        record("Failed tests:")
        for t in failed_tests:
            record("  %s" % t)

    # Always write results to tests.log.
    log_path = Path(__file__).parent / "tests.log"
    with open(log_path, "w", encoding="utf-8") as f:
        f.write("\n".join(output_lines) + "\n")

    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
