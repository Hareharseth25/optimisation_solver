"""The KAIRO Explorer application boundary: one structured solve.

    request (MPS text + options)
        -> optimsolver solve <model> --json <record>      (argv list, no shell)
        -> optimsolver.solve.v1 record, returned unchanged

Responsibilities are split exactly once:

  * KAIRO core owns solver truth. Classification, presolve, dispatch, the
    engines, validation and every number in the record come from one
    solver::solve() call inside the optimsolver binary.
  * This module owns interaction: it checks the request's shape, writes the
    model where KAIRO can read it, runs KAIRO, and maps the record's own
    termination status to an application outcome.

It never parses terminal output, never decides what an option value means
(KAIRO validates engine names, backends and limits), and never touches the
record's contents.
"""
from __future__ import annotations

import json
import math
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

SCHEMA = "optimsolver.solve.v1"

# Request limits. Local/development defaults; the server can override them.
MAX_REQUEST_BYTES = 64 * 1024 * 1024
DEFAULT_WALL_TIMEOUT_SECONDS = 600.0
WALL_TIMEOUT_GRACE_SECONDS = 30.0

# The only options exposed, each mapped to the CLI flag that already carries
# it. KAIRO decides whether a VALUE is acceptable; this table only fixes which
# flags exist and what JSON type each takes. Nothing here can name a file,
# a binary, or another CLI mode (--output, --dump-model, ... are not reachable).
OPTION_FLAGS = {
    "engine": ("--solver", str),
    "backend": ("--backend", str),
    "time_limit_seconds": ("--time-limit", float),
    "threads": ("--threads", int),
    "cuda_device": ("--cuda-device", int),
}

# The record's own termination.status -> (application outcome, HTTP status).
# Completed solves are successes whatever the verdict: proving infeasibility,
# unboundedness or stopping at a limit are answers. A model KAIRO cannot accept
# is the client's problem (422); a numerical breakdown is KAIRO failing (500).
STATUS_OUTCOMES = {
    "optimal": ("completed", 200),
    "infeasible": ("completed", 200),
    "unbounded": ("completed", 200),
    "limit_reached": ("completed", 200),
    "invalid_model": ("invalid_model", 422),
    "unsupported": ("unsupported", 422),
    "numerical_failure": ("solver_failure", 500),
}

# solver::Engine values that are dispatch outcomes rather than solver engines.
PSEUDO_ENGINES = {"infeasible", "trivial", "unsupported"}


def engine_rejected_model(record: dict) -> bool:
    """The model was accepted, but the engine the dispatcher selected refused it.

    KAIRO reports invalid_model when an engine cannot represent the model it
    was given (e.g. a valid MILP forced onto PDLP). That is a property of the
    engine/request, not of the model, so it must not reach the user as
    "invalid model". Decided from structure alone: the model was classified,
    the dispatcher ran and selected a real engine, and nothing executed. The
    record itself -- status included -- is returned unchanged.
    """
    dispatch = record.get("dispatch") or {}
    return (record.get("classification") is not None
            and dispatch.get("invoked") is True
            and dispatch.get("engine") not in PSEUDO_ENGINES
            and dispatch.get("engine") is not None
            and dispatch.get("executed_engine") is None)

# The model is always written under this fixed name inside a private temporary
# directory, and KAIRO runs with that directory as its working directory. The
# client never supplies a path or filename, and the record's instance.path is
# just this name rather than a server path.
MODEL_FILENAME = "model.mps"
RECORD_FILENAME = "record.json"


class RequestError(ValueError):
    """The request is malformed; nothing was run."""


def _response(outcome: str, http_status: int, record: Optional[dict] = None,
              message: Optional[str] = None, process: Optional[dict] = None) -> Tuple[int, dict]:
    return http_status, {
        "outcome": outcome,
        "record": record,
        "error": None if message is None else {"message": message},
        "process": process,
    }


def build_arguments(options: Any) -> Tuple[List[str], Optional[float]]:
    """Option object -> CLI arguments, plus the requested time limit.

    Checks only shape and JSON type. Whether "cplex" is an engine or -3 a
    thread count is KAIRO's decision, reported back as a rejection.
    """
    if options is None:
        return [], None
    if not isinstance(options, dict):
        raise RequestError("options must be an object")
    unknown = sorted(set(options) - set(OPTION_FLAGS))
    if unknown:
        raise RequestError("unknown option(s): " + ", ".join(unknown))

    arguments: List[str] = []
    time_limit: Optional[float] = None
    for key, (flag, kind) in OPTION_FLAGS.items():
        if key not in options or options[key] is None:
            continue
        value = options[key]
        # bool is an int subclass in Python; a JSON true is never a count.
        if isinstance(value, bool):
            raise RequestError(f"option {key} must be {kind.__name__}, not a boolean")
        if kind is str:
            if not isinstance(value, str):
                raise RequestError(f"option {key} must be a string")
            if key == "engine" and value == "auto":
                continue  # automatic dispatch is the absence of --solver
            text = value
        elif kind is int:
            if not isinstance(value, int):
                raise RequestError(f"option {key} must be an integer")
            text = str(value)
        else:
            if not isinstance(value, (int, float)) or not math.isfinite(value):
                raise RequestError(f"option {key} must be a finite number")
            time_limit = float(value)
            text = repr(float(value))
        arguments += [flag, text]
    return arguments, time_limit


def parse_request(body: bytes) -> Tuple[str, List[str], Optional[float]]:
    """Request bytes -> (MPS text, CLI option arguments, requested time limit)."""
    if len(body) > MAX_REQUEST_BYTES:
        raise RequestError("request too large")
    try:
        request = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RequestError(f"request is not valid JSON: {error}") from None
    if not isinstance(request, dict):
        raise RequestError("request must be a JSON object")
    unknown = sorted(set(request) - {"model", "options"})
    if unknown:
        raise RequestError("unknown request field(s): " + ", ".join(unknown))

    model = request.get("model")
    if not isinstance(model, dict):
        raise RequestError("model must be an object")
    unknown = sorted(set(model) - {"format", "content"})
    if unknown:
        raise RequestError("unknown model field(s): " + ", ".join(unknown))
    if model.get("format") != "mps":
        raise RequestError('model.format must be "mps"')
    content = model.get("content")
    if not isinstance(content, str) or not content.strip():
        raise RequestError("model.content must be non-empty MPS text")
    if "\x00" in content:
        raise RequestError("model.content must be text")

    arguments, time_limit = build_arguments(request.get("options"))
    return content, arguments, time_limit


def solve(body: bytes, binary: str, *,
          default_timeout: float = DEFAULT_WALL_TIMEOUT_SECONDS) -> Tuple[int, dict]:
    """Runs one solve. Returns (HTTP status, response object).

    The response always carries `outcome`; `record` is the untouched
    optimsolver.solve.v1 record whenever KAIRO produced one.
    """
    try:
        content, arguments, time_limit = parse_request(body)
    except RequestError as error:
        return _response("bad_request", 400, message=str(error))

    # KAIRO's own time limit governs the solve; the wall timeout only guards
    # the application against a run that never returns.
    wall_timeout = default_timeout if time_limit is None else time_limit + WALL_TIMEOUT_GRACE_SECONDS

    with tempfile.TemporaryDirectory(prefix="kairo-run-") as workdir:
        work = Path(workdir)
        (work / MODEL_FILENAME).write_text(content, encoding="utf-8")
        command = [binary, "solve", MODEL_FILENAME, "--json", RECORD_FILENAME, *arguments]
        started = time.monotonic()
        try:
            process = subprocess.run(command, cwd=work, shell=False, stdin=subprocess.DEVNULL,
                                     capture_output=True, text=True, timeout=wall_timeout)
        except subprocess.TimeoutExpired:
            return _response("timeout", 504,
                             message=f"KAIRO did not finish within {wall_timeout:g} s",
                             process={"exit_code": None,
                                      "wall_seconds": time.monotonic() - started})
        info = {"exit_code": process.returncode, "wall_seconds": time.monotonic() - started}

        if process.returncode < 0:
            return _response("solver_crashed", 500, process=info,
                             message=f"KAIRO terminated by signal {-process.returncode}")

        record_path = work / RECORD_FILENAME
        if not record_path.exists():
            # KAIRO refused the request before reading the model -- an option
            # value it does not accept. Its own message is passed through as
            # opaque text for a person to read; nothing is inferred from it.
            if process.returncode != 0:
                return _response("rejected", 400, process=info,
                                 message=process.stderr.strip() or "KAIRO rejected the request")
            return _response("no_record", 502, process=info,
                             message="KAIRO finished without writing a record")
        try:
            record = json.loads(record_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            return _response("malformed_record", 502, process=info,
                             message=f"KAIRO wrote an unreadable record: {error}")

    if not isinstance(record, dict) or record.get("schema") != SCHEMA:
        return _response("unexpected_record", 502, process=info,
                         message=f"expected a {SCHEMA} record")
    status = (record.get("termination") or {}).get("status")
    if status not in STATUS_OUTCOMES:
        return _response("unexpected_record", 502, record=record, process=info,
                         message=f"unknown termination status {status!r}")
    outcome, http_status = STATUS_OUTCOMES[status]
    if status == "invalid_model" and engine_rejected_model(record):
        outcome = "engine_rejected"  # the request is unprocessable; the model is not invalid
    return _response(outcome, http_status, record=record, process=info)
