//! Document store + import resolution environment.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;

use tower_lsp::lsp_types::{Position, Url};

use crate::analyzer::{
    analyze_env, Analysis, Diag, FileAnalysis, ImportEnv, LoadResult, Severity,
};
use crate::line_index::LineIndex;

/// An open (or cached) document.
pub struct Document {
    pub uri: Url,
    pub text: String,
    pub version: i32,
    pub line_index: LineIndex,
}

impl Document {
    pub fn new(uri: Url, text: String, version: i32) -> Self {
        let line_index = LineIndex::new(&text);
        Document { uri, text, version, line_index }
    }

    /// Apply incremental LSP content changes.
    pub fn apply_changes(&mut self, changes: &[(Option<crate_range::Range>, String)]) {
        for (range, new_text) in changes {
            match range {
                None => {
                    self.text = new_text.clone();
                }
                Some(range) => {
                    let start = self.line_index.offset(range.start);
                    let end = self.line_index.offset(range.end);
                    if start <= end && end <= self.text.len() {
                        self.text.replace_range(start..end, new_text);
                    }
                }
            }
            self.line_index = LineIndex::new(&self.text);
        }
    }
}

/// Minimal re-export shim to avoid importing lsp_types everywhere here.
pub mod crate_range {
    pub use tower_lsp::lsp_types::Range;
}

/// Filesystem + open-document backed import environment with a visiting
/// stack for cycle detection and a disk cache for parsed files.
pub struct Workspace {
    pub docs: HashMap<Url, Document>,
    /// Cached full analyses per open document (refreshed by the server).
    pub analyses: HashMap<Url, Arc<Analysis>>,
    disk_cache: HashMap<PathBuf, Arc<FileAnalysis>>,
    pub visiting: Vec<Url>,
}

impl Workspace {
    pub fn new() -> Self {
        Workspace {
            docs: HashMap::new(),
            analyses: HashMap::new(),
            disk_cache: HashMap::new(),
            visiting: Vec::new(),
        }
    }

    /// Invalidate the disk cache entry for a path.
    pub fn invalidate_disk(&mut self, path: &PathBuf) {
        self.disk_cache.remove(path);
    }

    /// Analyze one document with this workspace as the import environment.
    pub fn analyze(&mut self, uri: &Url, text: &str) -> Arc<Analysis> {
        // Put the file on the visiting stack so imports of itself cycle-detect.
        self.visiting.push(uri.clone());
        let analysis = analyze_env(uri, text, self);
        self.visiting.pop();
        Arc::new(analysis)
    }

    /// Convert analyzer diagnostics to LSP diagnostics.
    pub fn to_lsp_diags(diags: &[Diag], line_index: &LineIndex) -> Vec<tower_lsp::lsp_types::Diagnostic> {
        use tower_lsp::lsp_types::{Diagnostic, DiagnosticSeverity};
        diags
            .iter()
            .map(|d| {
                let severity = match d.severity {
                    Severity::Error => DiagnosticSeverity::ERROR,
                    Severity::Warning => DiagnosticSeverity::WARNING,
                    Severity::Info => DiagnosticSeverity::INFORMATION,
                    Severity::Hint => DiagnosticSeverity::HINT,
                };
                let mut diag = Diagnostic::new_simple(
                    tower_lsp::lsp_types::Range::new(
                        line_index.position(d.span.start),
                        line_index.position(d.span.end),
                    ),
                    d.message.clone(),
                );
                diag.severity = Some(severity);
                diag.source = Some("lamo-lsp".to_string());
                diag.code = Some(tower_lsp::lsp_types::NumberOrString::String(d.code.to_string()));
                diag.tags = if d.code == "deprecated" {
                    Some(vec![tower_lsp::lsp_types::DiagnosticTag::DEPRECATED])
                } else {
                    None
                };
                diag
            })
            .collect()
    }
}

impl ImportEnv for Workspace {
    fn load(&mut self, importing: &Url, import_path: &str) -> LoadResult {
        // 1. std handled by the caller (analyzer) before delegating here;
        //    we still support local std/ overrides (SPEC §10.4.6).
        let dir: Option<PathBuf> = importing
            .to_file_path()
            .ok()
            .and_then(|p| p.parent().map(|p| p.to_path_buf()));

        let candidates: Vec<PathBuf> = if import_path.starts_with("std.") {
            let rest = import_path.strip_prefix("std.").unwrap();
            let mut v = Vec::new();
            if let Ok(std_dir) = std::env::var("LAMO_STD_DIR") {
                v.push(PathBuf::from(std_dir).join(format!("{rest}.lamo")));
            }
            // Local override next to the importing file, then CWD std/.
            if let Some(d) = &dir {
                v.push(d.join("std").join(format!("{rest}.lamo")));
            }
            v.push(PathBuf::from("std").join(format!("{rest}.lamo")));
            v
        } else {
            let mut v = Vec::new();
            let base = if import_path.contains('/') || import_path.ends_with(".lamo") {
                PathBuf::from(import_path)
            } else {
                // bare identifier: `import utils` -> utils.lamo (SPEC §10.3)
                PathBuf::from(format!("{import_path}.lamo"))
            };
            if let Some(d) = &dir {
                v.push(d.join(&base));
                // folder module fallback: <name>/mod.lamo (SPEC §10.3)
                v.push(d.join(import_path).join("mod.lamo"));
            }
            v.push(base.clone());
            v.push(import_path.clone().into());
            v
        };

        // 2. open documents first.
        for cand in &candidates {
            let url = match Url::from_file_path(cand) {
                Ok(u) => u,
                Err(_) => continue,
            };
            if self.visiting.contains(&url) {
                return LoadResult::Cycle;
            }
            if let Some(doc) = self.docs.get(&url) {
                let text = doc.text.clone();
                return LoadResult::Loaded(url.clone(), self.analyze_cached(&url, &text));
            }
        }

        // 3. disk.
        for cand in &candidates {
            if cand.exists() {
                let abs = cand.canonicalize().unwrap_or_else(|_| cand.clone());
                let url = match Url::from_file_path(&abs) {
                    Ok(u) => u,
                    Err(_) => continue,
                };
                if self.visiting.contains(&url) {
                    return LoadResult::Cycle;
                }
                if let Some(cached) = self.disk_cache.get(&abs) {
                    return LoadResult::Loaded(url, cached.clone());
                }
                match std::fs::read_to_string(&abs) {
                    Ok(text) => {
                        self.visiting.push(url.clone());
                        let analysis = analyze_env(&url, &text, self);
                        self.visiting.pop();
                        let analysis = Arc::new(analysis.file);
                        self.disk_cache.insert(abs, analysis.clone());
                        return LoadResult::Loaded(url, analysis);
                    }
                    Err(_) => continue,
                }
            }
        }
        LoadResult::NotFound
    }
}

impl Workspace {
    /// Analyze a document with the workspace env and cache the full Analysis.
    pub fn analyze_full(&mut self, uri: &Url, text: &str) -> Arc<Analysis> {
        self.visiting.push(uri.clone());
        let analysis = analyze_env(uri, text, self);
        self.visiting.pop();
        Arc::new(analysis)
    }

    fn analyze_cached(&mut self, uri: &Url, text: &str) -> Arc<FileAnalysis> {
        self.visiting.push(uri.clone());
        let analysis = analyze_env(uri, text, self);
        self.visiting.pop();
        Arc::new(analysis.file)
    }
}

/// Helper: find which open document's URI matches a path candidate.
pub fn url_to_path(url: &Url) -> Option<PathBuf> {
    url.to_file_path().ok()
}

/// Convenience: build a Position from line/char (used by tests).
#[allow(dead_code)]
pub fn pos(line: u32, character: u32) -> Position {
    Position::new(line, character)
}
