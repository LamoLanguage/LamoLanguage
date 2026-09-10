//! lamo-lsp: Language Server for the Lamo programming language.
//!
//! Runs over stdio. Start it from an editor client; see the README for
//! per-editor configuration.

use tower_lsp::{LspService, Server};

#[tokio::main]
async fn main() {
    let stdin = tokio::io::stdin();
    let stdout = tokio::io::stdout();

    let (service, socket) = LspService::build(|client| lamo_lsp::server::Backend::new(client))
        .custom_method("lamo/stdContent", lamo_lsp::server::Backend::std_content)
        .finish();

    Server::new(stdin, stdout, socket).serve(service).await;

    // The client sent `exit` (or closed stdin) — leave promptly so editors
    // that wait on the process handle see a clean shutdown.
    std::process::exit(0);
}
