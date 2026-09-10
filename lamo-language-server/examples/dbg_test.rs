use lamo_lsp::analyzer::{analyze_env, Severity};
use lamo_lsp::workspace::Workspace;
use tower_lsp::lsp_types::Url;

fn main() {
    let files = [
        "/home/z/my-project/lamo-research/std/json.lamo",
        "/home/z/my-project/lamo-research/std/os.lamo",
        "/home/z/my-project/lamo-research/std/env.lamo",
        "/home/z/my-project/lamo-research/tests/runtime/generics_full.lamo",
    ];
    for f in files {
        let text = std::fs::read_to_string(f).unwrap();
        let uri = Url::from_file_path(f).unwrap();
        let mut ws = Workspace::new();
        let a = ws.analyze(&uri, &text);
        let errs: Vec<_> = a.file.diags.iter().filter(|d| d.severity == Severity::Error).collect();
        if errs.is_empty() { continue; }
        println!("=== {}", f);
        for d in errs.iter().take(8) {
            let line = text[..d.span.start].matches('\n').count() + 1;
            let src_line = text.lines().nth(line - 1).unwrap_or("").trim();
            println!("  L{}: {} | {}", line, d.message, src_line);
        }
    }
}
