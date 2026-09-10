//! Repo-wide validation: run the analyzer over every real .lamo file in the
//! official repository and flag errors in files expected to be valid.
//!
//! Expected-valid: std/*.lamo, tests/valid/**, tests/runtime/**, examples/**
//! (everything else: parse for panics, don't require zero diags).

use std::path::{Path, PathBuf};
use tower_lsp::lsp_types::Url;

use lamo_lsp::analyzer::Severity;
use lamo_lsp::workspace::Workspace;

fn collect(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            collect(&p, out);
        } else if p.extension().map(|x| x == "lamo").unwrap_or(false) {
            out.push(p);
        }
    }
}

fn is_expected_valid(p: &Path) -> bool {
    let s = p.to_string_lossy();
    // Example fixtures whose own comments say they intentionally contain
    // errors (used to demo diagnostics output).
    let intentionally_broken = [
        "builtins_as_idents.lamo",
        "errors_with_import.lamo",
        "multi_errors.lamo",
    ];
    if intentionally_broken.iter().any(|n| s.ends_with(n)) {
        return false;
    }
    s.contains("/std/") && s.ends_with(".lamo") && !s.contains("/std/tests/")
        || s.contains("/tests/valid/")
        || s.contains("/tests/runtime/")
        || s.contains("/examples/")
}

fn main() {
    let arg = std::env::args().nth(1);
    let root = PathBuf::from(arg.unwrap_or_else(|| "/home/z/my-project/lamo-research".into()));
    let mut files = Vec::new();
    collect(&root, &mut files);
    files.sort();

    let mut total = 0;
    let mut false_positives = 0;
    let mut flagged_valid = 0;
    for f in &files {
        let Ok(text) = std::fs::read_to_string(f) else { continue };
        total += 1;
        let uri = Url::from_file_path(f).unwrap();
        let mut ws = Workspace::new();
        let a = ws.analyze(&uri, &text);
        let errs: Vec<_> = a
            .file
            .diags
            .iter()
            .filter(|d| d.severity == Severity::Error)
            .collect();
        if errs.is_empty() {
            continue;
        }
        if is_expected_valid(f) {
            flagged_valid += 1;
            println!("FALSE-POSITIVE {} ({} errors)", f.display(), errs.len());
            for d in errs.iter().take(5) {
                let line = text[..d.span.start.min(text.len())].matches('\n').count() + 1;
                let src_line = text.lines().nth(line - 1).unwrap_or("").trim();
                println!("   L{}: {} | {}", line, d.message, src_line);
            }
        }
        let _ = false_positives;
    }
    println!(
        "\n{} files analyzed; {} expected-valid files with errors",
        total, flagged_valid
    );
    if flagged_valid > 0 {
        std::process::exit(1);
    }
}
