#!/usr/bin/env python3

# Copyright (c) 2026 Centre Tecnologic de Telecomunicacions de Catalunya (CTTC)
#
# SPDX-License-Identifier: GPL-2.0-only

"""MR triage: decide whether a merge request should be kept as is or split.

Asks a local LLM (llama-server, Maple-Preview) for a structured verdict and, if the
MR should be split, a proposed split. MRs too big for its context window are rejected.
Output: report printed to the job log, and written to triage.json.

Stdlib only.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import textwrap
import time
import urllib.request

# Tunables (override via env)
# Tokens reserved for the answer, reasoning included
MAX_OUTPUT_TOKENS = int(os.getenv("TRIAGE_MAX_TOKENS", "16384"))
# Largest context window to start llama-server with (Maple-Preview was trained on 128k)
MAX_CTX = int(os.getenv("TRIAGE_MAX_CTX", "131072"))
# Conservative estimate; nr diffs measured 3.2-3.5 characters per token
CHARS_PER_TOKEN = 3


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def summarize(base, head):
    """Plain facts about the MR, shown to the model and in the job log."""
    rng = f"{base}...{head}"
    lines = 0
    files = git("diff", "--numstat", "-M", rng).splitlines()
    for line in files:
        add, rem, _ = line.split("\t", 2)
        if add != "-":  # binary files have no line counts
            lines += int(add) + int(rem)
    return {
        "files": len(files),
        "lines_changed": lines,
        "commits": git("log", "--reverse", "--format=%h %s", f"{base}..{head}").splitlines(),
    }


def build_prompt(base, head, summary):
    diff = git("diff", "-U2", "-M", f"{base}...{head}")

    commits = []
    for line in git("log", "--reverse", "--format=%h %s", f"{base}..{head}").splitlines():
        sha = line.split()[0]
        paths = git("show", "--format=", "--name-only", sha).split()
        commits.append(f"{line}\n" + "\n".join(f"  {p}" for p in paths))

    return f"""You review merge requests for a C++ network simulator (ns-3 / 5G-LENA).
Decide whether this MR should be merged as one MR ("keep") or split into several ("split").

Split when the MR mixes independent concerns that reviewers would rather see separately,
for example: refactor without behaviour change + new feature, unrelated bug fixes,
formatting churn + logic, API changes + their many call-site updates in other modules.
Keep when the changes are one coherent unit (a feature with its tests, docs and examples
belongs together). Every proposed MR must build and pass tests on its own, and the
proposed MRs must be in a mergeable order.

If the verdict is "split", propose at least two MRs, in merge order, and assign every
changed file to one of them. Prefer splitting along commit boundaries. A test that only
exercises the code changed by another commit belongs with that commit's MR.
If the verdict is "keep", leave proposed_mrs empty.
confidence is a number between 0 and 1.
Base the reason only on the commits and diff shown below, and refer to commits by their
subject. Do not describe bugs, features or behaviour that the diff does not show.

Commits (oldest first) and the files they touch:
{chr(10).join(commits)}

Summary (JSON):
{json.dumps(summary, indent=1)}

Diff:
{diff}

Answer with JSON only."""


SCHEMA = {
    "type": "object",
    "properties": {
        "verdict": {"type": "string", "enum": ["keep", "split"]},
        "confidence": {"type": "number"},
        "reason": {"type": "string"},
        "proposed_mrs": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "title": {"type": "string"},
                    "files": {"type": "array", "items": {"type": "string"}},
                    "rationale": {"type": "string"},
                },
                "required": ["title", "files", "rationale"],
            },
        },
    },
    "required": ["verdict", "confidence", "reason", "proposed_mrs"],
}

# A split must come with a proposal. Enforcing that in the grammar makes sampling
# an order of magnitude slower, so ask again instead.
SPLIT_FOLLOW_UP = (
    'Your verdict was "split" but proposed_mrs has fewer than two entries. '
    "Answer again with the same JSON, listing the MRs to split into."
)


def http_json(url, payload=None, headers=None, method=None, timeout=3600):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={"Content-Type": "application/json", **(headers or {})},
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read() or b"null")


def wait_server(url, server, timeout=300):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if server.poll() is not None:
            raise RuntimeError(f"llama-server exited with code {server.returncode}")
        try:
            if http_json(f"{url}/health", timeout=5).get("status") == "ok":
                return
        except Exception:
            pass
        time.sleep(2)
    raise RuntimeError("llama-server did not become healthy")


def start_server(url, ctx):
    """Start llama-server with a context window of ctx tokens; host/port come from LLAMA_ARG_*."""
    print(f"starting llama-server with a {ctx}-token context window")
    log = open("llama-server.log", "a")
    server = subprocess.Popen(
        [
            "llama-server",
            "-m",
            os.environ["TRIAGE_MODEL"],
            "--ctx-size",
            str(ctx),
            "--parallel",
            "1",
            "--flash-attn",
            "on",
            "--cache-type-k",
            "q8_0",
            "--cache-type-v",
            "q8_0",
            "--jinja",
            "--reasoning-format",
            "deepseek",
        ],
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    wait_server(url, server)
    return server


def stop_server(server):
    server.terminate()
    try:
        server.wait(timeout=30)
    except subprocess.TimeoutExpired:
        server.kill()


def ctx_for(prompt_tokens):
    """Smallest context window, in 4k steps, holding the prompt and the answer."""
    need = prompt_tokens + MAX_OUTPUT_TOKENS
    return min(-(-need // 4096) * 4096, MAX_CTX)


def count_tokens(url, prompt):
    """Exact token count of the prompt, once wrapped in the chat template."""
    messages = [{"role": "user", "content": prompt}]
    text = http_json(f"{url}/apply-template", {"messages": messages})["prompt"]
    return len(http_json(f"{url}/tokenize", {"content": text})["tokens"])


class TooBig(Exception):
    """The MR does not fit in the largest context window."""


def prepare_llm(url, prompt):
    """Start llama-server with a context window that fits the prompt and the answer.

    The window starts from a character-based estimate and is enlarged once the
    exact token count is known. Raises TooBig if not even MAX_CTX can hold it.
    """
    ctx = ctx_for(len(prompt) // CHARS_PER_TOKEN)
    server = start_server(url, ctx)
    tokens = count_tokens(url, prompt)
    if tokens + MAX_OUTPUT_TOKENS > MAX_CTX:
        stop_server(server)
        raise TooBig(tokens)
    if ctx_for(tokens) > ctx:
        stop_server(server)
        ctx = ctx_for(tokens)
        server = start_server(url, ctx)
    print(f"prompt: {len(prompt)} chars, {tokens} tokens, context window {ctx}")
    return server


def chat(url, messages):
    resp = http_json(
        f"{url}/v1/chat/completions",
        {
            "messages": messages,
            "temperature": 1.0,
            "top_p": 0.95,
            "max_tokens": MAX_OUTPUT_TOKENS,
            "response_format": {"type": "json_schema", "json_schema": {"schema": SCHEMA}},
        },
    )
    choice = resp["choices"][0]
    if choice.get("finish_reason") == "length":
        raise ValueError("model output hit TRIAGE_MAX_TOKENS before finishing")
    text = choice["message"].get("content") or ""
    # Maple always reasons; strip inline think blocks if the server didn't separate them.
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.S).strip()
    m = re.search(r"\{.*\}", text, flags=re.S)
    if not m:
        raise ValueError(f"no JSON in model output: {text[:500]}")
    return json.loads(m.group(0)), m.group(0), resp.get("timings", {})


def ask_llm(url, prompt):
    t0 = time.time()
    messages = [{"role": "user", "content": prompt}]
    out, raw, timings = chat(url, messages)
    if out["verdict"] == "split" and len(out["proposed_mrs"]) < 2:
        messages += [
            {"role": "assistant", "content": raw},
            {"role": "user", "content": SPLIT_FOLLOW_UP},
        ]
        out, _, timings = chat(url, messages)
        if out["verdict"] == "split" and len(out["proposed_mrs"]) < 2:
            raise ValueError("split verdict without a proposal")
    out["confidence"] = min(max(float(out["confidence"]), 0.0), 1.0)
    out["_timing"] = {"seconds": round(time.time() - t0, 1), **timings}
    return out


VERDICT_COLOR = {"keep": "32", "split": "33", "unavailable": "31"}  # green, yellow, red


def render_terminal(result):
    """Plain-text report for the job log."""

    def wrap(text, indent):
        return textwrap.fill(text, width=88, initial_indent=indent, subsequent_indent=indent)

    s = result["summary"]
    color = VERDICT_COLOR.get(result["verdict"], "0")
    head = f"MR triage: \033[1;{color}m{result['verdict'].upper()}\033[0m"
    if "confidence" in result:
        head += f" (confidence {result['confidence']:.2f})"
    lines = [
        "",
        "=" * 88,
        head,
        f"source: {result['source']}",
        f"MR: {s['files']} files, {s['lines_changed']} lines changed, {len(s['commits'])} commit{'s' if len(s['commits']) != 1 else ''}",
        "=" * 88,
        "",
        wrap(result["reason"], ""),
    ]
    for i, mr in enumerate(result.get("proposed_mrs") or [], 1):
        lines += ["", f"{i}. {mr['title']}", wrap(mr["rationale"], "   ")]
        lines += [f"   - {p}" for p in mr["files"]]
    if "_timing" in result:
        t = result["_timing"]
        lines += [
            "",
            f"model: {t.get('prompt_n', '?')} prompt tokens, {t.get('predicted_n', '?')} generated "
            f"at {t.get('predicted_per_second', 0):.1f} tokens/s, {t['seconds']} s",
        ]
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=os.getenv("CI_MERGE_REQUEST_DIFF_BASE_SHA"))
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--llm-url", default=os.getenv("TRIAGE_LLM_URL", "http://127.0.0.1:8080"))
    a = ap.parse_args()
    if not a.base:
        sys.exit("no --base and CI_MERGE_REQUEST_DIFF_BASE_SHA unset")

    summary = summarize(a.base, a.head)

    server = None
    try:
        prompt = build_prompt(a.base, a.head, summary)
        server = prepare_llm(a.llm_url, prompt)
        result = ask_llm(a.llm_url, prompt)
        result["source"] = "maple-preview"
    except TooBig as e:
        result = {
            "verdict": "split",
            "source": "size limit",
            "reason": f"Too big to review as one MR: {e.args[0]} tokens, while at most "
            f"{MAX_CTX - MAX_OUTPUT_TOKENS} fit the reviewer's context window. "
            "Split it into smaller MRs.",
        }
    except Exception as e:  # never fail the pipeline on model trouble
        result = {
            "verdict": "unavailable",
            "source": "error",
            "reason": f"LLM unavailable or invalid output: {e}",
        }
    finally:
        if server:
            stop_server(server)
    result["summary"] = summary

    with open("triage.json", "w") as fh:
        json.dump(result, fh, indent=1)
    print(render_terminal(result))


if __name__ == "__main__":
    main()
