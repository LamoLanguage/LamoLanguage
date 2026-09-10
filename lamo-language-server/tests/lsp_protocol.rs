//! End-to-end LSP protocol integration tests.
//!
//! Drives the real `LspService` (the same stack `main.rs` serves over stdio)
//! through tower's `Service` trait and asserts on the JSON-RPC responses,
//! including server-to-client notifications (`publishDiagnostics`) and the
//! custom `lamo/stdContent` method.

use futures::StreamExt;
use serde::de::DeserializeOwned;
use serde_json::{json, Value};
use tower::{Service, ServiceExt};
use tower_lsp::jsonrpc;
use tower_lsp::lsp_types::*;
use tower_lsp::{LspService, LanguageServer};
use lamo_lsp::server::Backend;

const DOC: &str = concat!(
    "import std.math\n",
    "\n",
    "fn add(a, b) {\n",
    "    return a + b\n",
    "}\n",
    "\n",
    "fn main() {\n",
    "    let total = add(1, 2)\n",
    "    let root = math.sqrt(total)\n",
    "    print(total)\n",
    "    print(root)\n",
    "}\n",
);

const URI_STR: &str = "file:///tmp/lamo-it/demo.lamo";

fn uri() -> Url {
    Url::parse(URI_STR).unwrap()
}

fn pos(line: u32, character: u32) -> Position {
    Position::new(line, character)
}

/// Params as a JSON-RPC Value (tower-lsp's builder takes `Into<Value>`).
fn raw(v: &Value) -> Value {
    v.clone()
}

struct Harness {
    service: LspService<Backend>,
    socket: Box<dyn futures::Stream<Item = jsonrpc::Request> + Unpin>,
    next_id: i64,
}

impl Harness {
    async fn new() -> Self {
        let (service, socket) = LspService::build(|client| Backend::new(client))
            .custom_method("lamo/stdContent", Backend::std_content)
            .finish();
        let mut h = Harness {
            service,
            socket: Box::new(socket),
            next_id: 0,
        };
        // initialize + initialized handshake
        let init_params = InitializeParams {
            capabilities: ClientCapabilities::default(),
            ..Default::default()
        };
        h.service.ready().await.unwrap();
        let resp = h
            .service
            .call(jsonrpc::Request::build("initialize")
                .id(0i64)
                .params(raw(&serde_json::to_value(&init_params).unwrap()))
                .finish())
            .await
            .unwrap()
            .unwrap();
        assert!(resp.error().is_none(), "initialize failed: {resp:?}");
        let init: InitializeResult =
            serde_json::from_value(resp.result().unwrap().clone()).unwrap();
        assert_eq!(init.server_info.as_ref().unwrap().name, "lamo-lsp");
        h.service.ready().await.unwrap();
        h.service
            .call(jsonrpc::Request::build("initialized").finish())
            .await
            .unwrap();
        h
    }

    /// Send a request and deserialize the result.
    async fn request<P: serde::Serialize, R: DeserializeOwned>(&mut self, method: &str, params: &P) -> R {
        self.next_id += 1;
        let id = self.next_id;
        self.service.ready().await.unwrap();
        let resp = self
            .service
            .call(jsonrpc::Request::build(method.to_string())
                .id(id)
                .params(raw(&serde_json::to_value(params).unwrap()))
                .finish())
            .await
            .unwrap()
            .unwrap();
        assert!(
            resp.error().is_none(),
            "{method} returned error: {:?}",
            resp.error()
        );
        serde_json::from_value(resp.result().cloned().unwrap_or(Value::Null))
            .unwrap_or_else(|e| panic!("{method}: failed to decode result: {e}"))
    }

    /// Send a notification.
    async fn notify<P: serde::Serialize>(&mut self, method: &str, params: &P) {
        self.service.ready().await.unwrap();
        self.service
            .call(jsonrpc::Request::build(method.to_string())
                .params(raw(&serde_json::to_value(params).unwrap()))
                .finish())
            .await
            .unwrap();
    }

    /// Collect publishDiagnostics notifications for `uri`, awaiting up to `n`.
    async fn diagnostics(&mut self, uri: &Url, n: usize) -> Vec<Diagnostic> {
        let deadline = tokio::time::Duration::from_secs(2);
        let mut out = Vec::new();
        for _ in 0..n {
            match tokio::time::timeout(deadline, self.socket.next()).await {
                Ok(Some(req)) => {
                    if req.method() == "textDocument/publishDiagnostics" {
                        let p: PublishDiagnosticsParams =
                            serde_json::from_value(req.params().cloned().unwrap_or(Value::Null))
                                .unwrap();
                        if &p.uri == uri {
                            out = p.diagnostics;
                        }
                    }
                }
                other => panic!("expected diagnostics notification, got {other:?}"),
            }
        }
        out
    }

    async fn open(&mut self, text: &str) -> Vec<Diagnostic> {
        self.notify(
            "textDocument/didOpen",
            &DidOpenTextDocumentParams {
                text_document: TextDocumentItem {
                    uri: uri(),
                    language_id: "lamo".into(),
                    version: 1,
                    text: text.into(),
                },
            },
        )
        .await;
        self.diagnostics(&uri(), 1).await
    }
}

#[tokio::test]
async fn capabilities_and_handshake() {
    // covered inside Harness::new (initialize must succeed with serverInfo)
}

#[tokio::test]
async fn open_valid_document_no_diagnostics() {
    let mut h = Harness::new().await;
    let diags = h.open(DOC).await;
    assert!(
        diags.is_empty(),
        "valid doc produced diagnostics: {diags:?}"
    );
}

#[tokio::test]
async fn hover_shows_local_fn_signature() {
    let mut h = Harness::new().await;
    h.open(DOC).await;
    let hover: Option<Hover> = h
        .request(
            "textDocument/hover",
            &HoverParams {
                text_document_position_params: TextDocumentPositionParams {
                    text_document: TextDocumentIdentifier { uri: uri() },
                    position: pos(7, 17), // on `add`
                },
                work_done_progress_params: Default::default(),
            },
        )
        .await;
    let hv = hover.expect("hover returned None");
    let text = match hv.contents {
        HoverContents::Markup(m) => m.value,
        HoverContents::Scalar(s) => format!("{s:?}"),
        HoverContents::Array(a) => format!("{a:?}"),
    };
    assert!(text.contains("add"), "hover text: {text}");
    assert!(text.contains("a, b"), "hover text: {text}");
}

#[tokio::test]
async fn completion_after_dot_suggests_std_members() {
    let mut h = Harness::new().await;
    h.open(DOC).await;
    let resp: Option<CompletionResponse> = h
        .request(
            "textDocument/completion",
            &CompletionParams {
                text_document_position: TextDocumentPositionParams {
                    text_document: TextDocumentIdentifier { uri: uri() },
                    position: pos(8, 20), // right after `math.`
                },
                context: Some(CompletionContext {
                    trigger_kind: CompletionTriggerKind::TRIGGER_CHARACTER,
                    trigger_character: Some(".".into()),
                }),
                work_done_progress_params: Default::default(),
                partial_result_params: Default::default(),
            },
        )
        .await;
    let items = match resp.unwrap() {
        CompletionResponse::Array(items) => items,
        CompletionResponse::List(l) => l.items,
    };
    assert!(
        items.iter().any(|i| i.label == "sqrt"),
        "no sqrt in {items:?}"
    );
}

#[tokio::test]
async fn completion_includes_locals_and_builtins() {
    let mut h = Harness::new().await;
    h.open(DOC).await;
    let resp: Option<CompletionResponse> = h
        .request(
            "textDocument/completion",
            &CompletionParams {
                text_document_position: TextDocumentPositionParams {
                    text_document: TextDocumentIdentifier { uri: uri() },
                    position: pos(9, 10), // inside print(total)
                },
                context: None,
                work_done_progress_params: Default::default(),
                partial_result_params: Default::default(),
            },
        )
        .await;
    let items = match resp.unwrap() {
        CompletionResponse::Array(items) => items,
        CompletionResponse::List(l) => l.items,
    };
    let labels: Vec<&str> = items.iter().map(|i| i.label.as_str()).collect();
    assert!(labels.contains(&"total"), "labels: {labels:?}");
    assert!(labels.contains(&"add"), "labels: {labels:?}");
    assert!(labels.contains(&"print"), "labels: {labels:?}");
}

#[tokio::test]
async fn goto_definition_local_and_std() {
    let mut h = Harness::new().await;
    h.open(DOC).await;
    // local fn
    let loc: Option<GotoDefinitionResponse> = h
        .request(
            "textDocument/definition",
            &GotoDefinitionParams {
                text_document_position_params: TextDocumentPositionParams {
                    text_document: TextDocumentIdentifier { uri: uri() },
                    position: pos(7, 17),
                },
                work_done_progress_params: Default::default(),
                partial_result_params: Default::default(),
            },
        )
        .await;
    match loc.unwrap() {
        GotoDefinitionResponse::Scalar(l) => {
            assert_eq!(l.uri, uri());
            assert_eq!(l.range.start.line, 2);
        }
        other => panic!("unexpected definition response: {other:?}"),
    }
    // std member
    let loc: Option<GotoDefinitionResponse> = h
        .request(
            "textDocument/definition",
            &GotoDefinitionParams {
                text_document_position_params: TextDocumentPositionParams {
                    text_document: TextDocumentIdentifier { uri: uri() },
                    position: pos(8, 24), // on `sqrt`
                },
                work_done_progress_params: Default::default(),
                partial_result_params: Default::default(),
            },
        )
        .await;
    match loc.unwrap() {
        GotoDefinitionResponse::Scalar(l) => {
            assert_eq!(l.uri.scheme(), "lamo-std");
            assert!(l.uri.as_str().contains("std/math.lamo"), "uri: {:?}", l);
        }
        other => panic!("unexpected definition response: {other:?}"),
    }
}

#[tokio::test]
async fn document_symbols_outline() {
    let mut h = Harness::new().await;
    h.open(DOC).await;
    let resp: Option<DocumentSymbolResponse> = h
        .request(
            "textDocument/documentSymbol",
            &DocumentSymbolParams {
                text_document: TextDocumentIdentifier { uri: uri() },
                work_done_progress_params: Default::default(),
                partial_result_params: Default::default(),
            },
        )
        .await;
    match resp.unwrap() {
        DocumentSymbolResponse::Nested(syms) => {
            let names: Vec<&str> = syms.iter().map(|s| s.name.as_str()).collect();
            assert!(names.contains(&"add"), "names: {names:?}");
            assert!(names.contains(&"main"), "names: {names:?}");
            let add = syms.iter().find(|s| s.name == "add").unwrap();
            assert_eq!(add.kind, SymbolKind::FUNCTION);
            assert_eq!(add.detail.as_deref(), Some("fn add(a, b)"));
        }
        other => panic!("unexpected symbol response: {other:?}"),
    }
}

#[tokio::test]
async fn signature_help_on_call() {
    let mut h = Harness::new().await;
    h.open(DOC).await;
    let sh: Option<SignatureHelp> = h
        .request(
            "textDocument/signatureHelp",
            &SignatureHelpParams {
                text_document_position_params: TextDocumentPositionParams {
                    text_document: TextDocumentIdentifier { uri: uri() },
                    position: pos(7, 20), // right after add(
                },
                work_done_progress_params: Default::default(),
                context: None,
            },
        )
        .await;
    let sh = sh.expect("no signature help");
    assert!(
        sh.signatures.iter().any(|s| s.label.contains("a, b")),
        "signatures: {:?}",
        sh.signatures
    );
}

#[tokio::test]
async fn document_highlight_finds_all_uses() {
    let mut h = Harness::new().await;
    h.open(DOC).await;
    let hl: Option<Vec<DocumentHighlight>> = h
        .request(
            "textDocument/documentHighlight",
            &DocumentHighlightParams {
                text_document_position_params: TextDocumentPositionParams {
                    text_document: TextDocumentIdentifier { uri: uri() },
                    position: pos(7, 8), // `total` decl
                },
                work_done_progress_params: Default::default(),
                partial_result_params: Default::default(),
            },
        )
        .await;
    let hl = hl.expect("no highlights");
    let mut lines: Vec<u32> = hl.iter().map(|h| h.range.start.line).collect();
    lines.sort();
    lines.dedup();
    assert_eq!(lines, vec![7, 8, 9], "highlight lines: {lines:?}");
}

#[tokio::test]
async fn diagnostics_report_errors_after_bad_edit() {
    let mut h = Harness::new().await;
    let _ = h.open(DOC).await;
    let bad = "fn main() {\n    undefined_fn(1)\n    let = 3\n}\n";
    h.notify(
        "textDocument/didChange",
        &DidChangeTextDocumentParams {
            text_document: VersionedTextDocumentIdentifier {
                uri: uri(),
                version: 2,
            },
            content_changes: vec![TextDocumentContentChangeEvent {
                range: None,
                range_length: None,
                text: bad.into(),
            }],
        },
    )
    .await;
    let diags = h.diagnostics(&uri(), 1).await;
    assert!(
        diags.iter().any(|d| d.severity == Some(DiagnosticSeverity::ERROR)),
        "expected error diagnostics: {diags:?}"
    );
}

#[tokio::test]
async fn std_content_custom_method() {
    let mut h = Harness::new().await;
    let src: Option<String> = h
        .request("lamo/stdContent", &json!({ "uri": "lamo-std://std/math.lamo" }))
        .await;
    let src = src.expect("stdContent returned None");
    assert!(src.contains("sqrt"), "math source should contain sqrt");
}

#[tokio::test]
async fn did_close_clears_diagnostics() {
    let mut h = Harness::new().await;
    h.open(DOC).await;
    h.notify(
        "textDocument/didClose",
        &DidCloseTextDocumentParams {
            text_document: TextDocumentIdentifier { uri: uri() },
        },
    )
    .await;
    let diags = h.diagnostics(&uri(), 1).await;
    assert!(diags.is_empty(), "didClose must clear diagnostics: {diags:?}");
}
