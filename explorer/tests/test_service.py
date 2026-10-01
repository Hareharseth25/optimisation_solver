#!/usr/bin/env python3
"""The Explorer application boundary, tested against the real optimsolver.

Every case solves the same MPS bytes twice -- once through
kairo_explorer.service and once by running the binary directly -- and requires
the application's record to be the core's record: identical in every value
except wall-clock timings, whose presence/absence (which stages ran) must
still match exactly. Each case also checks the specific values that matter
(classification, presolve, dispatch, engine, status, objective, validation),
never just an HTTP status or exit code.

Usage: test_service.py <path-to-optimsolver>
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "explorer"))

from kairo_explorer import server, service  # noqa: E402

BINARY = os.path.abspath(sys.argv.pop(1)) if len(sys.argv) > 1 else os.environ.get("OPTIMSOLVER_BINARY", "")
FIXTURES = ROOT / "tests" / "cli"

# Rejected by the MPS reader itself (unknown row sense), so no model exists.
UNREADABLE_MPS = "NAME          BAD\nROWS\n N  OBJ\n Q  C1\nCOLUMNS\n    X  OBJ  1.0\nENDATA\n"


def request(content: str, **options) -> bytes:
    return json.dumps({"model": {"format": "mps", "content": content},
                       "options": options}).encode()


def fixture(name: str) -> str:
    return (FIXTURES / name).read_text() if "/" not in name else (ROOT / name).read_text()


def direct_record(content: str, *arguments: str):
    """The core's own record for the same bytes, run the way the service runs it."""
    with tempfile.TemporaryDirectory() as work:
        Path(work, "model.mps").write_text(content)
        subprocess.run([BINARY, "solve", "model.mps", "--json", "record.json", *arguments],
                       cwd=work, capture_output=True, text=True, timeout=120)
        path = Path(work, "record.json")
        return json.loads(path.read_text()) if path.exists() else None


def without_clock(record: dict) -> dict:
    """The record with wall-clock values replaced by whether they were measured."""
    copy = json.loads(json.dumps(record))
    copy["stage_seconds"] = {k: v is not None for k, v in copy["stage_seconds"].items()}
    copy["work"]["solve_seconds"] = copy["work"]["solve_seconds"] is not None
    return copy


class ServiceTest(unittest.TestCase):
    def solve(self, content: str, expected_http: int, expected_outcome: str, **options):
        """Solves through the service and checks it against a direct run."""
        options.setdefault("threads", 1)  # deterministic tree search for the comparison
        status, body = service.solve(request(content, **options), BINARY)
        self.assertEqual(status, expected_http, body.get("error"))
        self.assertEqual(body["outcome"], expected_outcome)
        self.assertEqual(set(body), {"outcome", "record", "error", "process"})
        record = body["record"]
        self.assertIsNotNone(record)
        self.assertEqual(record["schema"], "optimsolver.solve.v1")

        arguments, _ = service.build_arguments(options)
        core = direct_record(content, *arguments)
        self.assertIsNotNone(core)
        self.assertEqual(without_clock(record), without_clock(core),
                         "the application must return the core's record unchanged")
        self.assertEqual(record["instance"]["path"], "model.mps", "no server path leaks")
        self.assert_stages_consistent(record)
        return record

    def assert_stages_consistent(self, record):
        stages = record["stage_seconds"]
        timed = [stages[k] for k in ("validation", "classification", "presolve", "dispatch",
                                     "engine", "reduced_validation", "postsolve")
                 if stages[k] is not None]
        self.assertTrue(all(t >= 0 for t in timed))
        if stages["total"] is not None:
            self.assertEqual(stages["total"], record["work"]["solve_seconds"])
            self.assertLessEqual(sum(timed), stages["total"] + 1e-9)

    def assert_full_pipeline(self, record, problem_class, engine, objective):
        self.assertEqual(record["termination"]["status"], "optimal")
        self.assertEqual(record["classification"]["problem_class"], problem_class)
        self.assertTrue(record["dispatch"]["invoked"])
        self.assertEqual(record["dispatch"]["executed_engine"], engine)
        self.assertEqual(record["termination"]["executed_engine"], engine)
        self.assertAlmostEqual(record["objective"], objective, places=6)
        for space in ("reduced_space", "original_space"):
            self.assertTrue(record["validation"][space]["passed"])
            self.assertEqual(record["validation"][space]["status"], "success")
        self.assertTrue(all(v is not None for v in record["stage_seconds"].values()))

    # 1. LP
    def test_lp(self):
        record = self.solve(fixture("presolve_reduction.mps"), 200, "completed")
        self.assert_full_pipeline(record, "LP", "dual_simplex", 28.0)
        self.assertEqual(record["presolve"]["original_variables"], 3)
        self.assertEqual(record["presolve"]["reduced_variables"], 2)
        self.assertEqual(record["presolve"]["transformations_by_type"]["fix_variable"], 1)
        self.assertEqual(record["instance"]["objective_sense"], "min")

    # 2. MILP
    def test_milp(self):
        record = self.solve(fixture("knapsack_milp.mps"), 200, "completed")
        self.assert_full_pipeline(record, "MILP", "branch_and_cut", 7.0)
        self.assertEqual(record["classification"]["num_binary"], 3)
        self.assertTrue(record["self_reported"]["integrality_respected"])

    # 3. QP
    def test_qp(self):
        record = self.solve(fixture("convex_qp.mps"), 200, "completed")
        self.assert_full_pipeline(record, "QP", "qp", -4.5)

    # 4. presolve-infeasible: a completed solve, with no dispatcher run.
    def test_presolve_infeasible(self):
        record = self.solve(fixture("tests/mps/test_cases/01_basic_lp.mps"), 200, "completed")
        self.assertEqual(record["termination"]["status"], "infeasible")
        self.assertTrue(record["presolve"]["infeasible"])
        self.assertFalse(record["dispatch"]["invoked"])
        self.assertIsNone(record["dispatch"]["executed_engine"])
        self.assertEqual(record["validation"], {"reduced_space": None, "original_space": None})
        self.assertIsNone(record["stage_seconds"]["engine"])

    # 5a. structurally invalid model
    def test_invalid_model(self):
        record = self.solve(fixture("invalid_bounds.mps"), 422, "invalid_model")
        self.assertEqual(record["termination"]["status"], "invalid_model")
        for key in ("classification", "presolve", "dispatch", "validation"):
            self.assertIsNone(record[key])
        self.assertEqual(record["instance"]["variables"], 1)

    # 5b. unreadable MPS: still a structured record, never a stderr scrape.
    def test_unreadable_model(self):
        record = self.solve(UNREADABLE_MPS, 422, "invalid_model")
        self.assertEqual(record["termination"]["status"], "invalid_model")
        self.assertIn("unknown row sense", record["termination"]["message"])
        self.assertIsNone(record["instance"]["variables"])
        self.assertIsNone(record["classification"])
        self.assertIsNotNone(record["stage_seconds"]["parse"])
        self.assertIsNone(record["stage_seconds"]["total"])

    # 6. forced engine
    def test_forced_engine(self):
        record = self.solve(fixture("simple_lp.mps"), 200, "completed", engine="pdlp")
        self.assertEqual(record["settings"]["requested_engine"], "pdlp")
        self.assertEqual(record["dispatch"]["engine"], "pdlp")
        self.assertEqual(record["dispatch"]["reason"], "engine forced by the caller")
        self.assertEqual(record["dispatch"]["executed_engine"], "pdlp")

    def test_automatic_engine_is_no_flag(self):
        self.assertEqual(service.build_arguments({"engine": "auto"}), ([], None))
        record = self.solve(fixture("simple_lp.mps"), 200, "completed", engine="auto")
        self.assertIsNone(record["settings"]["requested_engine"])

    # 7. backend selection
    def test_backend_cpu(self):
        record = self.solve(fixture("simple_lp.mps"), 200, "completed", engine="pdlp", backend="cpu")
        self.assertEqual(record["compute_backend"]["requested"], "cpu")
        self.assertEqual(record["compute_backend"]["executed"], "cpu")

    def test_backend_cuda(self):
        options = {"engine": "pdlp", "backend": "cuda", "threads": 1}
        status, body = service.solve(request(fixture("simple_lp.mps"), **options), BINARY)
        backend = body["record"]["compute_backend"]
        if body["record"]["termination"]["status"] == "unsupported":
            # No usable GPU: refused, never silently run on the CPU.
            self.solve(fixture("simple_lp.mps"), 422, "unsupported", engine="pdlp", backend="cuda")
            self.assertIsNone(backend["executed"])
            self.assertIn("CUDA", backend["reason"])
            self.assertEqual(body["record"]["dispatch"]["engine"], "pdlp")
            self.assertIsNone(body["record"]["dispatch"]["executed_engine"])
        else:
            self.assertEqual(status, 200)
            self.assertEqual(backend["executed"], "cuda")

    # Other solver outcomes the application must keep apart.
    def test_unsupported_problem(self):
        record = self.solve(fixture("nonconvex_qp.mps"), 422, "unsupported")
        self.assertTrue(record["dispatch"]["invoked"])
        self.assertEqual(record["dispatch"]["engine"], "unsupported")
        self.assertIn("non-convex", record["dispatch"]["reason"])

    def test_time_limit(self):
        record = self.solve(fixture("simple_lp.mps"), 200, "completed",
                            engine="pdlp", time_limit_seconds=1e-30)
        self.assertEqual(record["termination"]["status"], "limit_reached")
        self.assertIsNone(record["primal"])

    # A valid model forced onto an engine that cannot represent it: KAIRO's
    # status (invalid_model) is preserved in the record, but the application
    # outcome must not call the model invalid.
    def test_forced_engine_rejecting_a_valid_model_is_not_invalid_model(self):
        record = self.solve(fixture("knapsack_milp.mps"), 422, "engine_rejected", engine="pdlp")
        self.assertEqual(record["termination"]["status"], "invalid_model", "KAIRO status untouched")
        self.assertEqual(record["classification"]["problem_class"], "MILP")
        self.assertTrue(record["dispatch"]["invoked"])
        self.assertEqual(record["dispatch"]["engine"], "pdlp")
        self.assertIsNone(record["dispatch"]["executed_engine"])
        self.assertIn("integer variables", record["termination"]["message"])
        self.assertIsNotNone(record["stage_seconds"]["engine"], "engine path was entered")

        record = self.solve(fixture("convex_qp.mps"), 422, "engine_rejected", engine="dual_simplex")
        self.assertIn("linear objectives only", record["termination"]["message"])

    def test_engine_rejected_needs_the_whole_structure(self):
        base = {"classification": {"problem_class": "LP"},
                "dispatch": {"invoked": True, "engine": "pdlp", "executed_engine": None}}
        self.assertTrue(service.engine_rejected_model(base))
        for change in ({"classification": None},
                       {"dispatch": None},
                       {"dispatch": {"invoked": False, "engine": "pdlp", "executed_engine": None}},
                       {"dispatch": {"invoked": True, "engine": "unsupported", "executed_engine": None}},
                       {"dispatch": {"invoked": True, "engine": "pdlp", "executed_engine": "pdlp"}}):
            self.assertFalse(service.engine_rejected_model({**base, **change}), change)

    def test_outcome_table_covers_every_solver_status(self):
        statuses = {"optimal", "infeasible", "unbounded", "limit_reached",
                    "numerical_failure", "invalid_model", "unsupported"}
        self.assertEqual(set(service.STATUS_OUTCOMES), statuses)
        self.assertEqual(service.STATUS_OUTCOMES["numerical_failure"], ("solver_failure", 500))

    # Request handling: rejected before anything runs, or rejected by KAIRO.
    def test_malformed_requests(self):
        cases = [
            b"not json",
            b"[]",
            json.dumps({"model": {"format": "lp", "content": "x"}}).encode(),
            json.dumps({"model": {"format": "mps", "content": ""}}).encode(),
            json.dumps({"model": {"format": "mps", "content": "x", "path": "/etc/passwd"}}).encode(),
            json.dumps({"model": {"format": "mps", "content": "x"}, "binary": "/bin/sh"}).encode(),
            json.dumps({"model": {"format": "mps", "content": "x"}, "options": {"output": "x"}}).encode(),
            json.dumps({"model": {"format": "mps", "content": "x"}, "options": {"threads": True}}).encode(),
            json.dumps({"model": {"format": "mps", "content": "x"}, "options": {"threads": "4"}}).encode(),
            json.dumps({"model": {"format": "mps", "content": "x"},
                        "options": {"time_limit_seconds": "1; rm -rf /"}}).encode(),
        ]
        for body in cases:
            status, response = service.solve(body, BINARY)
            self.assertEqual(status, 400, body)
            self.assertEqual(response["outcome"], "bad_request")
            self.assertIsNone(response["record"])
            self.assertIsNone(response["process"], "nothing was run")

    def test_option_values_are_judged_by_kairo(self):
        for options in ({"engine": "cplex"}, {"engine": "--output"}, {"backend": "gpu"},
                        {"threads": -1}, {"time_limit_seconds": -5}):
            status, response = service.solve(request(fixture("simple_lp.mps"), **options), BINARY)
            self.assertEqual(status, 400, options)
            self.assertEqual(response["outcome"], "rejected")
            self.assertIsNone(response["record"])
            self.assertNotEqual(response["process"]["exit_code"], 0)

    def test_no_shell_and_no_files_outside_the_run(self):
        hostile = "--output; touch pwned"
        before = set(os.listdir("."))
        status, _ = service.solve(request(fixture("simple_lp.mps"), engine=hostile), BINARY)
        self.assertEqual(status, 400)
        self.assertEqual(set(os.listdir(".")), before)


class UiContractTest(unittest.TestCase):
    """The UI's engine menu must only offer names KAIRO accepts."""

    def test_ui_engine_names_are_accepted_by_kairo(self):
        source = (ROOT / "explorer" / "web" / "js" / "options.js").read_text()
        block = source.split("export const ENGINES = [", 1)[1].split("];", 1)[0]
        names = re.findall(r"value: '([a-z_]+)'", block)
        self.assertEqual(names[0], "auto")
        self.assertGreater(len(names), 1)
        for name in names[1:]:
            _, body = service.solve(request(fixture("simple_lp.mps"), engine=name, threads=1), BINARY)
            self.assertNotIn(body["outcome"], ("rejected", "bad_request"), name)
            self.assertEqual(body["record"]["settings"]["requested_engine"], name)


class HttpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.httpd = server.make_server(BINARY, "127.0.0.1", 0, quiet=True)
        cls.base = f"http://127.0.0.1:{cls.httpd.server_address[1]}"
        cls.thread = threading.Thread(target=cls.httpd.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    def call(self, method, path, body=None, content_type="application/json"):
        req = urllib.request.Request(self.base + path, data=body, method=method)
        if body is not None:
            req.add_header("Content-Type", content_type)
        try:
            with urllib.request.urlopen(req, timeout=120) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def test_health(self):
        self.assertEqual(self.call("GET", "/api/health"),
                         (200, {"status": "ok", "contract": "optimsolver.solve.v1"}))

    def test_solve_round_trip(self):
        status, body = self.call("POST", "/api/solve", request(fixture("knapsack_milp.mps"), threads=1))
        self.assertEqual((status, body["outcome"]), (200, "completed"))
        core = direct_record(fixture("knapsack_milp.mps"), "--threads", "1")
        self.assertEqual(without_clock(body["record"]), without_clock(core))

    def test_failure_is_not_success(self):
        status, body = self.call("POST", "/api/solve", request(fixture("invalid_bounds.mps")))
        self.assertEqual((status, body["outcome"]), (422, "invalid_model"))
        self.assertEqual(body["record"]["termination"]["status"], "invalid_model")

    def fetch(self, path):
        try:
            with urllib.request.urlopen(self.base + path, timeout=30) as response:
                return response.status, response.headers, response.read()
        except urllib.error.HTTPError as error:
            return error.code, error.headers, error.read()

    def test_ui_is_served_same_origin(self):
        status, headers, body = self.fetch("/")
        self.assertEqual(status, 200)
        self.assertTrue(headers["Content-Type"].startswith("text/html"))
        self.assertIn(b"KAIRO", body)
        self.assertIn("script-src 'self'", headers["Content-Security-Policy"])
        self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
        for path, kind in (("/styles.css", "text/css"), ("/js/app.js", "text/javascript"),
                           ("/js/report.js", "text/javascript")):
            status, headers, _ = self.fetch(path)
            self.assertEqual((status, headers["Content-Type"].split(";")[0]), (200, kind), path)

    def test_only_ui_files_are_reachable(self):
        for path in ("/../kairo_explorer/service.py", "/js/../../kairo_explorer/server.py",
                     "/%2e%2e/kairo_explorer/service.py", "/tests/report.test.js",
                     "/tests/fixtures/lp_optimal.json", "/package.json", "/README.md",
                     "//etc/passwd", "/js/"):
            self.assertEqual(self.fetch(path)[0], 404, path)

    def test_transport_errors(self):
        self.assertEqual(self.call("GET", "/etc/passwd")[0], 404)
        self.assertEqual(self.call("POST", "/api/solve", b"x", "text/plain")[0], 415)


if __name__ == "__main__":
    if not BINARY or not shutil.which(BINARY):
        sys.exit("usage: test_service.py <path-to-optimsolver>")
    unittest.main(verbosity=1)
