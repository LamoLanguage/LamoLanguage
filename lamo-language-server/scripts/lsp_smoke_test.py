#!/usr/bin/env python3
"""End-to-end LSP protocol smoke test for lamo-lsp.

Speaks real stdio LSP framing: each message is
    Content-Length: N\\r\\n
    \\r\\n
    <exactly N bytes of JSON>
Bodies are NOT newline-terminated, so reads must be byte-exact by
Content-Length (the bug that broke the previous attempt).
"""
import json
import os
import subprocess
import sys
import threading
import queue
import time

# Resolve the release binary relative to this script (project/scripts/../target/release).
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(_ROOT, "target", "release", "lamo-lsp")

DOC = (
    "import std.math\n"
    "\n"
    "fn add(a, b) {\n"
    "    return a + b\n"
    "}\n"
    "\n"
    "fn main() {\n"
    "    let total = add(1, 2)\n"
    "    let root = math.sqrt(total)\n"
    "    print(total)\n"
    "    print(root)\n"
    "}\n"
)

BAD_DOC = (
    "fn main() {\n"
    "    undefined_fn(1)\n"
    "    let = 3\n"
    "}\n"
)

URI = "file:///home/z/my-project/lsmoke/demo.lamo"

passed, failed = [], []


def check(name, cond, detail=""):
    if cond:
        passed.append(name)
        print(f"  PASS  {name}")
    else:
        failed.append(name)
        print(f"  FAIL  {name}  {detail}")


class LspClient:
    def __init__(self, proc):
        self.proc = proc
        self.q = queue.Queue()
        self.alive = True
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _read_loop(self):
        stdout = self.proc.stdout  # binary mode
        try:
            while self.alive:
                # 1. Read header lines until the blank \r\n separator.
                headers = {}
                while True:
                    line = stdout.readline()
                    if not line:
                        return
                    line = line.strip()
                    if not line:
                        break  # blank line = end of headers
                    if b":" in line:
                        k, v = line.split(b":", 1)
                        headers[k.strip().lower()] = v.strip()
                if b"content-length" not in headers:
                    continue
                # 2. Read the body byte-exactly (it is NOT newline-terminated).
                n = int(headers[b"content-length"])
                body = stdout.read(n)
                if len(body) < n:
                    return
                try:
                    self.q.put(json.loads(body.decode("utf-8")))
                except json.JSONDecodeError:
                    self.q.put({"raw_parse_error": body[:200].decode("utf-8", "replace")})
        except Exception as e:  # noqa
            self.q.put({"reader_died": str(e)})

    def send(self, method, params=None, is_req=True):
        msg = {"jsonrpc": "2.0", "method": method}
        if is_req:
            msg["id"] = next_id()
        if params is not None:
            msg["params"] = params
        self._write(msg)

    def respond(self, req_id, result):
        self._write({"jsonrpc": "2.0", "id": req_id, "result": result})

    def _write(self, msg):
        data = json.dumps(msg, separators=(",", ":")).encode("utf-8")
        frame = b"Content-Length: " + str(len(data)).encode() + b"\r\n\r\n" + data
        self.proc.stdin.write(frame)
        self.proc.stdin.flush()

    def recv(self, timeout=10.0):
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return {"timeout": True}

    def request(self, method, params=None, timeout=10.0):
        rid = next_id()
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self._write(msg)
        return self.wait_response(rid, timeout)

    def wait_response(self, rid, timeout=10.0):
        """Collect messages until the response with id=rid arrives; re-queue the rest."""
        deadline = time.time() + timeout
        stash = []
        while time.time() < deadline:
            try:
                m = self.q.get(timeout=max(0.05, deadline - time.time()))
            except queue.Empty:
                break
            if m.get("id") == rid and ("result" in m or "error" in m):
                for s in stash:
                    self.q.put(s)
                return m
            stash.append(m)
        for s in stash:
            self.q.put(s)
        return {"timeout": True}

    def drain_notifications(self, seconds=1.0):
        notes = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                m = self.q.get(timeout=max(0.05, deadline - time.time()))
                notes.append(m)
            except queue.Empty:
                break
        return notes


_id = [0]
def next_id():
    _id[0] += 1
    return _id[0]


def pos(line, char):
    return {"line": line, "character": char}


def main():
    proc = subprocess.Popen(
        [SERVER],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    c = LspClient(proc)

    # ---- 1. initialize -------------------------------------------------
    r = c.request("initialize", {
        "processId": None,
        "rootUri": "file:///home/z/my-project/lsmoke",
        "capabilities": {
            "textDocument": {
                "completion": {"completionItem": {"documentationFormat": ["markdown"]}},
                "signatureHelp": {"signatureInformation": {"documentationFormat": ["markdown"]}},
                "hover": {"contentFormat": ["markdown"]},
            }
        },
    })
    caps = r.get("result", {}).get("capabilities", {})
    check("initialize returns capabilities", bool(caps), str(r)[:200])
    check("hover provider", caps.get("hoverProvider") is True)
    check("definition provider", caps.get("definitionProvider") is True)
    check("documentSymbol provider", caps.get("documentSymbolProvider") is True)
    check("documentHighlight provider", caps.get("documentHighlightProvider") is True)
    check("signatureHelp provider", caps.get("signatureHelpProvider") is not None)
    check(
        "incremental sync",
        caps.get("textDocumentSync") == 2 or
        (isinstance(caps.get("textDocumentSync"), dict) and caps["textDocumentSync"].get("change") == 2),
        str(caps.get("textDocumentSync")),
    )
    check("serverInfo name", r.get("result", {}).get("serverInfo", {}).get("name") == "lamo-lsp")

    c.send("initialized", {}, is_req=False)

    # ---- 2. didOpen valid document -------------------------------------
    c.send("textDocument/didOpen", {
        "textDocument": {"uri": URI, "languageId": "lamo", "version": 1, "text": DOC}
    }, is_req=False)
    notes = c.drain_notifications(2.0)
    diags = [n for n in notes if n.get("method") == "textDocument/publishDiagnostics"]
    check("publishDiagnostics received for open", len(diags) >= 1, str(notes)[:200])
    if diags:
        items = diags[0]["params"].get("diagnostics", [])
        check("valid doc has zero error diagnostics", len(items) == 0, json.dumps(items)[:300])

    # ---- 3. hover on `add` call ----------------------------------------
    # L7: "    let total = add(1, 2)"  -> 'add' at col 16..19
    r = c.request("textDocument/hover", {
        "textDocument": {"uri": URI}, "position": pos(7, 17)
    })
    hv = r.get("result")
    hv_text = json.dumps(hv) if hv else ""
    check("hover returns result", hv is not None, str(r)[:200])
    check("hover mentions add", "add" in hv_text, hv_text[:200])
    check("hover shows (a, b)", "a, b" in hv_text, hv_text[:300])

    # ---- 4. completion after `math.` ------------------------------------
    # L8: "    let root = math.sqrt(total)" -> dot at col 19, cursor after it = col 20
    r = c.request("textDocument/completion", {
        "textDocument": {"uri": URI},
        "position": pos(8, 20),
        "context": {"triggerKind": 2, "triggerCharacter": "."},
    })
    items = r.get("result") or {}
    if isinstance(items, dict):
        items = items.get("items", [])
    names = [i.get("label", "") for i in items]
    check("completion after math. non-empty", len(items) > 0, str(r)[:200])
    check("completion includes sqrt", "sqrt" in names, str(names[:20]))

    # ---- 5. completion of local var -------------------------------------
    # L9: "    print(total)" -> col 10 is just after '('
    r = c.request("textDocument/completion", {
        "textDocument": {"uri": URI}, "position": pos(9, 10)
    })
    items = r.get("result") or {}
    if isinstance(items, dict):
        items = items.get("items", [])
    names = [i.get("label", "") for i in items]
    check("local completion includes total", "total" in names, str(names[:25]))
    check("local completion includes add", "add" in names, str(names[:25]))
    check("local completion includes print (builtin)", "print" in names, str(names[:30]))

    # ---- 6. goto definition of `add` ------------------------------------
    r = c.request("textDocument/definition", {
        "textDocument": {"uri": URI}, "position": pos(7, 17)
    })
    loc = r.get("result")
    ok = loc and loc.get("uri") == URI and loc.get("range", {}).get("start", {}).get("line") == 2
    check("goto definition add -> line 2", bool(ok), str(r)[:200])

    # ---- 6b. goto definition of std member (math.sqrt) -------------------
    r = c.request("textDocument/definition", {
        "textDocument": {"uri": URI}, "position": pos(8, 24)  # on 'sqrt'
    })
    loc = r.get("result")
    ok = loc and str(loc.get("uri", "")).startswith("lamo-std://std/math.lamo")
    check("goto definition sqrt -> lamo-std://std/math.lamo", bool(ok), str(r)[:300])

    # ---- 7. document symbols --------------------------------------------
    r = c.request("textDocument/documentSymbol", {"textDocument": {"uri": URI}})
    syms = r.get("result") or []
    names = [s.get("name") for s in syms]
    check("documentSymbol includes add", "add" in names, str(names))
    check("documentSymbol includes main", "main" in names, str(names))
    kinds = {s.get("name"): s.get("kind") for s in syms}
    check("functions are kind Function(12)", kinds.get("add") == 12, str(kinds))

    # ---- 8. signature help inside add( ) ---------------------------------
    # L7: "    let total = add(1, 2)" -> after '(' col 20
    r = c.request("textDocument/signatureHelp", {
        "textDocument": {"uri": URI}, "position": pos(7, 20)
    })
    sh = r.get("result") or {}
    sigs = sh.get("signatures", [])
    check("signatureHelp has add(a, b)", any("a, b" in s.get("label", "") for s in sigs), str(r)[:300])
    check("signatureHelp activeSignature set", "activeSignature" in sh, str(sh)[:200])

    # ---- 9. document highlight on `total` ---------------------------------
    r = c.request("textDocument/documentHighlight", {
        "textDocument": {"uri": URI}, "position": pos(7, 8)  # 'total' decl
    })
    hl = r.get("result") or []
    lines = sorted({h["range"]["start"]["line"] for h in hl})
    check("highlight total: 3 occurrences (L7,8,9)", lines == [7, 8, 9], str(lines))

    # ---- 10. didChange introduces errors -----------------------------------
    c.send("textDocument/didChange", {
        "textDocument": {"uri": URI, "version": 2},
        "contentChanges": [{"text": BAD_DOC}],
    }, is_req=False)
    notes = c.drain_notifications(2.0)
    diags = [n for n in notes if n.get("method") == "textDocument/publishDiagnostics"]
    got_errors = any(
        d.get("severity") == 1
        for n in diags if n["params"]["uri"] == URI
        for d in n["params"].get("diagnostics", [])
    )
    check("error diagnostics after bad edit", got_errors, json.dumps(diags)[:400])

    # ---- 11. incremental change: fix the doc --------------------------------
    # BAD_DOC L2 is "    let = 3" — insert "x " at col 8 => "    let x = 3".
    # The `let` syntax error must disappear (the undefined_fn error on L1
    # legitimately remains).
    c.send("textDocument/didChange", {
        "textDocument": {"uri": URI, "version": 3},
        "contentChanges": [{
            "range": {"start": pos(2, 8), "end": pos(2, 8)},
            "text": "x ",
        }],
    }, is_req=False)
    notes = c.drain_notifications(2.0)
    diags = [n for n in notes if n.get("method") == "textDocument/publishDiagnostics"]
    line2_errors = [d for n in diags if n["params"]["uri"] == URI
                    for d in n["params"].get("diagnostics", [])
                    if d.get("severity") == 1 and d["range"]["start"]["line"] == 2]
    check("incremental edit removes `let` error", len(line2_errors) == 0,
          json.dumps(line2_errors)[:300])

    # ---- 12. custom request lamo/stdContent ---------------------------------
    r = c.request("lamo/stdContent", {"uri": "lamo-std://std/math.lamo"})
    src = r.get("result")
    check("stdContent returns math source", isinstance(src, str) and "sqrt" in src,
          f"len={len(src) if isinstance(src, str) else None}")

    # ---- 13. shutdown / exit -------------------------------------------------
    r = c.request("shutdown", None)
    check("shutdown returns null", "result" in r and r["result"] is None, str(r)[:100])
    c.send("exit", None, is_req=False)
    proc.stdin.close()  # tower-lsp ends its loop at EOF; editors close the pipe too
    try:
        rc = proc.wait(timeout=5)
        check("server exits cleanly (code 0)", rc == 0, f"rc={rc}")
    except subprocess.TimeoutExpired:
        proc.kill()
        check("server exits cleanly (code 0)", False, "timeout")

    print()
    print(f"RESULTS: {len(passed)} passed, {len(failed)} failed")
    if failed:
        print("Failed:", *failed, sep="\n  - ")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
