//! The LSP server: tower-lsp Backend implementation.

use std::sync::Arc;

use tower_lsp::async_trait;
use tower_lsp::jsonrpc::Result as RpcResult;
use tower_lsp::lsp_types::*;
use tower_lsp::{Client, LanguageServer};

use crate::analyzer::{Analysis, ImportEnv, LoadResult};
use crate::features::{completion, goto, highlight, hover, signature, symbols};
use crate::line_index::LineIndex;
use crate::workspace::{Document, Workspace};

pub struct Backend {
    pub client: Client,
    pub ws: std::sync::RwLock<Workspace>,
}

impl Backend {
    pub fn new(client: Client) -> Self {
        Backend {
            client,
            ws: std::sync::RwLock::new(Workspace::new()),
        }
    }

    /// Snapshot of one document's analysis context.
    fn context(
        &self,
        uri: &Url,
    ) -> Option<(Arc<Analysis>, LineIndex, String)> {
        let ws = self.ws.read().expect("ws lock");
        let analysis = ws.analyses.get(uri)?.clone();
        let doc = ws.docs.get(uri)?;
        Some((analysis, doc.line_index.clone(), doc.text.clone()))
    }

    /// Re-analyze every open document and publish diagnostics.
    fn reanalyze_all(&self) {
        let docs: Vec<(Url, String, i32, LineIndex)> = {
            let ws = self.ws.read().expect("ws lock");
            ws.docs
                .iter()
                .map(|(u, d)| (u.clone(), d.text.clone(), d.version, d.line_index.clone()))
                .collect()
        };
        if docs.is_empty() {
            return;
        }
        let mut results: Vec<(Url, Vec<Diagnostic>, i32)> = Vec::new();
        {
            let mut ws = self.ws.write().expect("ws lock");
            for (uri, text, version, index) in &docs {
                let analysis = {
                    ws.visiting.push(uri.clone());
                    let a = crate::analyzer::analyze_env(uri, text, &mut WorkspaceAsEnv(&mut ws));
                    ws.visiting.pop();
                    Arc::new(a)
                };
                let diags = Workspace::to_lsp_diags(&analysis.file.diags, index);
                results.push((uri.clone(), diags, *version));
                ws.analyses.insert(uri.clone(), analysis);
            }
        }
        for (uri, diags, version) in results {
            let client = self.client.clone();
            tokio::spawn(async move {
                client.publish_diagnostics(uri, diags, Some(version)).await;
            });
        }
    }
}

struct ServerEnv<'a> {
    ws: &'a mut Workspace,
}

impl<'a> ImportEnv for ServerEnv<'a> {
    fn load(&mut self, importing: &Url, import_path: &str) -> LoadResult {
        self.ws.load(importing, import_path)
    }
}

/// Adapter letting `analyze_env` borrow the workspace in place.
struct WorkspaceAsEnv<'a>(&'a mut Workspace);

impl<'a> ImportEnv for WorkspaceAsEnv<'a> {
    fn load(&mut self, importing: &Url, import_path: &str) -> LoadResult {
        self.0.load(importing, import_path)
    }
}

#[tower_lsp::async_trait]
impl LanguageServer for Backend {
    async fn initialize(&self, _: InitializeParams) -> RpcResult<InitializeResult> {
        Ok(InitializeResult {
            capabilities: ServerCapabilities {
                text_document_sync: Some(TextDocumentSyncCapability::Kind(
                    TextDocumentSyncKind::INCREMENTAL,
                )),
                completion_provider: Some(CompletionOptions {
                    resolve_provider: Some(false),
                    trigger_characters: Some(vec![".".to_string()]),
                    ..Default::default()
                }),
                hover_provider: Some(HoverProviderCapability::Simple(true)),
                definition_provider: Some(OneOf::Left(true)),
                document_symbol_provider: Some(OneOf::Left(true)),
                signature_help_provider: Some(SignatureHelpOptions {
                    trigger_characters: Some(vec!["(".to_string(), ",".to_string()]),
                    ..Default::default()
                }),
                document_highlight_provider: Some(OneOf::Left(true)),
                ..Default::default()
            },
            server_info: Some(ServerInfo {
                name: "lamo-lsp".to_string(),
                version: Some(env!("CARGO_PKG_VERSION").to_string()),
            }),
        })
    }

    async fn initialized(&self, _: InitializedParams) {
        self.client
            .log_message(MessageType::INFO, "lamo-lsp initialized")
            .await;
    }

    async fn shutdown(&self) -> RpcResult<()> {
        Ok(())
    }

    async fn did_open(&self, params: DidOpenTextDocumentParams) {
        let doc = Document::new(
            params.text_document.uri.clone(),
            params.text_document.text,
            params.text_document.version,
        );
        {
            let mut ws = self.ws.write().expect("ws lock");
            ws.docs.insert(params.text_document.uri.clone(), doc);
        }
        self.reanalyze_all();
    }

    async fn did_change(&self, params: DidChangeTextDocumentParams) {
        {
            let mut ws = self.ws.write().expect("ws lock");
            if let Some(doc) = ws.docs.get_mut(&params.text_document.uri) {
                doc.version = params.text_document.version;
                let changes: Vec<(Option<Range>, String)> = params
                    .content_changes
                    .iter()
                    .map(|c| (c.range, c.text.clone()))
                    .collect();
                doc.apply_changes(&changes);
            }
        }
        self.reanalyze_all();
    }

    async fn did_save(&self, params: DidSaveTextDocumentParams) {
        if let Some(path) = crate::workspace::url_to_path(&params.text_document.uri) {
            let mut ws = self.ws.write().expect("ws lock");
            ws.invalidate_disk(&path);
        }
        self.reanalyze_all();
    }

    async fn did_close(&self, params: DidCloseTextDocumentParams) {
        {
            let mut ws = self.ws.write().expect("ws lock");
            ws.docs.remove(&params.text_document.uri);
            ws.analyses.remove(&params.text_document.uri);
        }
        self.client
            .publish_diagnostics(params.text_document.uri, vec![], None)
            .await;
    }

    async fn completion(
        &self,
        params: CompletionParams,
    ) -> RpcResult<Option<CompletionResponse>> {
        let uri = params.text_document_position.text_document.uri.clone();
        let pos = params.text_document_position.position;
        let ctx = params.context.and_then(|c| c.trigger_character);
        let Some((analysis, index, text)) = self.context(&uri) else {
            return Ok(None);
        };
        let offset = index.offset(pos).min(text.len());
        Ok(completion::completion(&analysis, offset, ctx.as_deref()))
    }

    async fn hover(&self, params: HoverParams) -> RpcResult<Option<Hover>> {
        let uri = params.text_document_position_params.text_document.uri.clone();
        let pos = params.text_document_position_params.position;
        let Some((analysis, index, text)) = self.context(&uri) else {
            return Ok(None);
        };
        let offset = index.offset(pos).min(text.len());
        Ok(hover::hover(&analysis, offset))
    }

    async fn goto_definition(
        &self,
        params: GotoDefinitionParams,
    ) -> RpcResult<Option<GotoDefinitionResponse>> {
        let uri = params.text_document_position_params.text_document.uri.clone();
        let pos = params.text_document_position_params.position;
        let Some((analysis, index, text)) = self.context(&uri) else {
            return Ok(None);
        };
        let offset = index.offset(pos).min(text.len());
        Ok(goto::definition(&analysis, &index, offset).map(GotoDefinitionResponse::Scalar))
    }

    async fn document_symbol(
        &self,
        params: DocumentSymbolParams,
    ) -> RpcResult<Option<DocumentSymbolResponse>> {
        let uri = params.text_document.uri.clone();
        let Some((analysis, index, _)) = self.context(&uri) else {
            return Ok(None);
        };
        Ok(symbols::document_symbols(&analysis, &index, &params))
    }

    async fn signature_help(
        &self,
        params: SignatureHelpParams,
    ) -> RpcResult<Option<SignatureHelp>> {
        let uri = params.text_document_position_params.text_document.uri.clone();
        let pos = params.text_document_position_params.position;
        let Some((analysis, index, text)) = self.context(&uri) else {
            return Ok(None);
        };
        let offset = index.offset(pos).min(text.len());
        Ok(signature::signature_help(&analysis, offset))
    }

    async fn document_highlight(
        &self,
        params: DocumentHighlightParams,
    ) -> RpcResult<Option<Vec<DocumentHighlight>>> {
        let uri = params.text_document_position_params.text_document.uri.clone();
        let pos = params.text_document_position_params.position;
        let Some((analysis, index, text)) = self.context(&uri) else {
            return Ok(None);
        };
        let offset = index.offset(pos).min(text.len());
        Ok(highlight::highlight(&analysis, &index, offset))
    }

}

#[derive(serde::Deserialize, serde::Serialize)]
pub struct StdContentParams {
    pub uri: String,
}

impl Backend {
    /// Custom request: content of an embedded std file. The VSCode client's
    /// virtual document provider calls this so go-to-definition into the
    /// stdlib shows real source.
    pub async fn std_content(&self, params: StdContentParams) -> RpcResult<Option<String>> {
        let uri = match Url::parse(&params.uri) {
            Ok(u) => u,
            Err(_) => return Ok(None),
        };
        match crate::stdlib::module_from_uri(&uri) {
            Some(module) => Ok(crate::stdlib::std_source(&module).map(|s| s.to_string())),
            None => Ok(None),
        }
    }
}
