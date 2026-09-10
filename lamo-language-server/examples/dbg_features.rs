//! Debug harness: introspect completion/highlight/symbol outputs at the
//! exact offsets the Python smoke test uses.

use lamo_lsp::analyzer::analyze_env;
use lamo_lsp::features::{completion, highlight, symbols};
use lamo_lsp::line_index::LineIndex;
use lamo_lsp::workspace::Workspace;
use tower_lsp::lsp_types::{DocumentSymbolParams, PartialResultParams, TextDocumentIdentifier, Url, WorkDoneProgressParams};

const DOC: &str = "import std.math\n\
\n\
fn add(a, b) {\n\
    return a + b\n\
}\n\
\n\
fn main() {\n\
    let total = add(1, 2)\n\
    let root = math.sqrt(total)\n\
    print(total)\n\
    print(root)\n\
}\n";

fn offset_of(line: u32, col: u32) -> usize {
    let idx = LineIndex::new(DOC);
    idx.offset(tower_lsp::lsp_types::Position::new(line, col))
}

fn main() {
    let uri = Url::parse("file:///home/z/my-project/lsmoke/demo.lamo").unwrap();
    let mut ws = Workspace::new();
    let analysis = ws.analyze(&uri, DOC);

    println!("== scopes ==");
    for s in &analysis.file.scopes {
        println!("  scope [{}, {}] vars: {:?}", s.start, s.end,
            s.vars.iter().map(|(n, _, _)| n.clone()).collect::<Vec<_>>());
    }
    println!("== uses containing L9 region ==");
    for u in &analysis.file.uses {
        if u.span.start > offset_of(7, 0) && u.span.end < offset_of(11, 0) {
            println!("  use {:?} span [{}, {}] target={:?}", u.name, u.span.start, u.span.end,
                std::mem::discriminant(&u.target));
        }
    }

    // completion at L9 col 10 (after `print(`)
    let off = offset_of(9, 10);
    println!("\n== completion at L9 c10 (offset {}) ==", off);
    if let Some(resp) = completion::completion(&analysis, off, None) {
        if let tower_lsp::lsp_types::CompletionResponse::Array(items) = &resp {
            println!("  {} items", items.len());
            let labels: Vec<&str> = items.iter().map(|i| i.label.as_str()).collect();
            println!("  labels: {:?}", labels);
        }
    } else {
        println!("  None!");
    }

    // member completion at L8 col 20 (right after `math.`)
    let off = offset_of(8, 20);
    println!("\n== completion at L8 c20 (offset {}) ==", off);
    match completion::completion(&analysis, off, Some(".")) {
        Some(tower_lsp::lsp_types::CompletionResponse::Array(items)) => {
            let labels: Vec<&str> = items.iter().map(|i| i.label.as_str()).collect();
            println!("  labels: {:?}", labels);
        }
        Some(_) => println!("  (other response shape)"),
        None => println!("  None!"),
    }
    // also at col 19 (before the dot, with trigger)
    let off = offset_of(8, 19);
    println!("== completion at L8 c19 trigger '.' (offset {}) ==", off);
    match completion::completion(&analysis, off, Some(".")) {
        Some(tower_lsp::lsp_types::CompletionResponse::Array(items)) => {
            let labels: Vec<&str> = items.iter().map(|i| i.label.as_str()).collect();
            println!("  labels: {:?}", labels);
        }
        Some(_) => println!("  (other response shape)"),
        None => println!("  None!"),
    }

    // highlight at decl of total L7 c8
    let off = offset_of(7, 8);
    println!("\n== highlight at L7 c8 (offset {}) ==", off);
    // tokens around the cursor
    println!("  tokens near {}:", off);
    for t in analysis.file.tokens.iter().filter(|t| t.span.start < off + 10 && t.span.end > off.saturating_sub(10)) {
        println!("    tok {:?} span [{}, {})", std::mem::discriminant(&t.tok), t.span.start, t.span.end);
    }
    let ident = lamo_lsp::features::resolve::ident_context(&analysis.file.tokens, off);
    println!("  ident_context: {:?}", ident.as_ref().map(|(n, s, _)| (n.clone(), *s)));
    if let Some((ref name, _, _)) = ident {
        let t = lamo_lsp::features::resolve::resolve_bare(&analysis, name, off);
        println!("  resolve_bare({:?}): is_none={}", name, t.is_none());
    }
    println!("  uses with span containing {}:", off);
    for u in analysis.file.uses.iter().filter(|u| u.span.start <= off && off <= u.span.end) {
        println!("    use {:?} span [{}, {})", u.name, u.span.start, u.span.end);
    }
    // introspect resolve_at first
    println!("  resolve_at({}):", off);
    match lamo_lsp::features::resolve::resolve_at(&analysis, off) {
        Some((span, name, target)) => println!("    span {:?} name {:?} target {:?}", span, name,
            match &target {
                lamo_lsp::features::resolve::Target::LocalVar { name, decl_span, .. } => format!("LocalVar({name}) decl {decl_span:?}"),
                lamo_lsp::features::resolve::Target::Symbol { name, .. } => format!("Symbol({name})"),
                other => format!("{other:?}"),
            }),
        None => println!("    None"),
    }
    // same at the use site
    let off2 = offset_of(9, 10);
    println!("  resolve_at({}):", off2);
    match lamo_lsp::features::resolve::resolve_at(&analysis, off2) {
        Some((span, name, target)) => println!("    span {:?} name {:?} target {:?}", span, name,
            match &target {
                lamo_lsp::features::resolve::Target::LocalVar { name, decl_span, .. } => format!("LocalVar({name}) decl {decl_span:?}"),
                lamo_lsp::features::resolve::Target::Symbol { name, .. } => format!("Symbol({name})"),
                other => format!("{other:?}"),
            }),
        None => println!("    None"),
    }
    let idx = LineIndex::new(DOC);
    match highlight::highlight(&analysis, &idx, off) {
        Some(hs) => {
            println!("  {} highlights", hs.len());
            for h in hs {
                println!("    {:?}", h.range);
            }
        }
        None => println!("  None!"),
    }
    // also try L9 (use of total)
    let off = offset_of(9, 10);
    match highlight::highlight(&analysis, &idx, off) {
        Some(hs) => println!("  highlight at L9 c10: {} hits", hs.len()),
        None => println!("  highlight at L9 c10: None!"),
    }

    // document symbols
    println!("\n== document symbols ==");
    let params = DocumentSymbolParams {
        text_document: TextDocumentIdentifier { uri: uri.clone() },
        work_done_progress_params: WorkDoneProgressParams::default(),
        partial_result_params: PartialResultParams::default(),
    };
    if let Some(resp) = symbols::document_symbols(&analysis, &idx, &params) {
        if let tower_lsp::lsp_types::DocumentSymbolResponse::Nested(syms) = &resp {
            for s in syms {
                println!("  name={:?} kind={:?} detail={:?}", s.name, s.kind, s.detail);
            }
        }
    }
}
