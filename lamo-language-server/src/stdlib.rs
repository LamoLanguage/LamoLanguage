//! Embedded Lamo standard library.
//!
//! The actual `std/*.lamo` and `std/*.md` files from the official
//! LamoLanguage repository are embedded at compile time. The `.lamo`
//! sources are parsed with the same parser used for user documents, so
//! signatures are always real; the `.md` API references provide hover
//! documentation.

use std::sync::OnceLock;
use tower_lsp::lsp_types::Url;

use crate::ast::{Decl, TypeKind};
use crate::lexer::Span;
use crate::parser;

pub static STD_SCHEME: &str = "lamo-std";

/// One public member of a std module.
#[derive(Debug, Clone)]
pub struct StdMember {
    pub name: String,
    /// e.g. `fn length(s)` / `let PI`
    pub signature: String,
    pub doc: String,
    pub is_fn: bool,
    /// Parameter count for functions (for arity checks).
    pub arity: usize,
    /// Byte span of the member name within the embedded std source
    /// (for go-to-definition targets).
    pub name_span: Span,
}

#[derive(Debug, Clone)]
pub struct StdModule {
    pub name: String,
    pub doc: String,
    pub members: Vec<StdMember>,
}

impl StdModule {
    pub fn member(&self, name: &str) -> Option<&StdMember> {
        self.members.iter().find(|m| m.name == name)
    }
}

/// Synthetic URI for an embedded std file: `lamo-std://std/io.lamo`.
pub fn std_uri(module: &str) -> Url {
    Url::parse(&format!("{STD_SCHEME}://std/{module}.lamo")).expect("valid std uri")
}

/// Extract the module name back from a `lamo-std://std/X.lamo` URI.
pub fn module_from_uri(uri: &Url) -> Option<String> {
    if uri.scheme() != STD_SCHEME {
        return None;
    }
    let file = uri.path().strip_prefix('/')?.strip_suffix(".lamo")?;
    Some(file.to_string())
}

macro_rules! std_sources {
    ($($name:ident),*) => {
        vec![ $( (stringify!($name), include_str!(concat!("std_embed/", stringify!($name), ".lamo")), include_str!(concat!("std_embed/", stringify!($name), ".md"))) ),* ]
    };
}

fn sources() -> Vec<(&'static str, &'static str, &'static str)> {
    std_sources!(
        collections, debug, env, fs, io, json, math, net, os, path, process, random, string,
        testing, time
    )
}

/// Cached line index for a std module source (used to convert member spans
/// into LSP positions).
pub fn std_line_index(module: &str) -> Option<crate::line_index::LineIndex> {
    static IDX: OnceLock<Vec<(String, crate::line_index::LineIndex)>> = OnceLock::new();
    let idx = IDX.get_or_init(|| {
        sources()
            .into_iter()
            .map(|(name, src, _)| (name.to_string(), crate::line_index::LineIndex::new(src)))
            .collect()
    });
    idx.iter().find(|(n, _)| n == module).map(|(_, i)| i.clone())
}

/// The embedded std source text for a module.
pub fn std_source(module: &str) -> Option<&'static str> {
    static SRC: OnceLock<Vec<(String, &'static str)>> = OnceLock::new();
    let src = SRC.get_or_init(|| {
        sources()
            .into_iter()
            .map(|(name, s, _)| (name.to_string(), s))
            .collect()
    });
    src.iter().find(|(n, _)| n == module).map(|(_, s)| *s)
}

pub fn library() -> &'static Vec<StdModule> {
    static LIB: OnceLock<Vec<StdModule>> = OnceLock::new();
    LIB.get_or_init(build)
}

fn build() -> Vec<StdModule> {
    let mut modules = Vec::new();
    for (name, lamo_src, md_src) in sources() {
        let lexed = crate::lexer::lex(lamo_src);
        let parsed = parser::parse(lamo_src, &lexed);
        let mut members = Vec::new();
        for decl in &parsed.program.decls {
            match decl {
                Decl::Fn(f) if f.is_pub => {
                    let params = f
                        .params
                        .iter()
                        .map(|p| p.name.clone())
                        .collect::<Vec<_>>()
                        .join(", ");
                    let mut sig = format!("fn {}({})", f.name, params);
                    if let Some(ret) = &f.ret {
                        sig.push_str(" -> ");
                        sig.push_str(&render_ann(ret));
                    }
                    let doc = md_doc_for(md_src, &f.name)
                        .or_else(|| doc_from_lines(&f.doc))
                        .unwrap_or_default();
                    members.push(StdMember {
                        name: f.name.clone(),
                        signature: sig,
                        doc,
                        is_fn: true,
                        arity: f.params.len(),
                        name_span: f.name_span,
                    });
                }
                Decl::Let(l) if l.is_pub && !l.name.starts_with('_') => {
                    let sig = format!("let {}", l.name);
                    let doc = md_doc_for(md_src, &l.name)
                        .or_else(|| doc_from_lines(&l.doc))
                        .unwrap_or_default();
                    members.push(StdMember {
                        name: l.name.clone(),
                        signature: sig,
                        doc,
                        is_fn: false,
                        arity: 0,
                        name_span: l.name_span,
                    });
                }
                _ => {}
            }
        }
        modules.push(StdModule {
            name: name.to_string(),
            doc: module_doc(md_src),
            members,
        });
    }
    modules
}

fn render_ann(ann: &crate::ast::TypeAnn) -> String {
    match &ann.kind {
        TypeKind::Int => "int".into(),
        TypeKind::Float => "float".into(),
        TypeKind::Bool => "bool".into(),
        TypeKind::String => "string".into(),
        TypeKind::Void => "void".into(),
        TypeKind::Array(Some(e)) => format!("array<{}>", render_ann(e)),
        TypeKind::Array(None) => "array".into(),
        TypeKind::Named(n, args) => {
            if args.is_empty() {
                n.clone()
            } else {
                format!(
                    "{}<{}>",
                    n,
                    args.iter().map(render_ann).collect::<Vec<_>>().join(", ")
                )
            }
        }
    }
}

fn doc_from_lines(lines: &[String]) -> Option<String> {
    if lines.is_empty() {
        None
    } else {
        Some(lines.join("\n"))
    }
}

fn module_doc(md: &str) -> String {
    // Everything before the first `## ` heading is the module intro.
    let mut intro = String::new();
    for line in md.lines() {
        if line.starts_with("## ") {
            break;
        }
        intro.push_str(line);
        intro.push('\n');
    }
    intro.trim().to_string()
}

/// Extract the per-function doc bullet from a std `.md` API reference.
fn md_doc_for(md: &str, name: &str) -> Option<String> {
    for line in md.lines() {
        let trimmed = line.trim();
        if !trimmed.starts_with("- `") {
            continue;
        }
        let inner = &trimmed[3..trimmed.len().min(trimmed.len())];
        if let Some(closing) = inner.find('`') {
            let sig = &inner[..closing];
            let rest = &inner[closing + 1..];
            // `name(...)` or `NAME` (constant), possibly grouped with `/`
            let names: Vec<&str> = sig.split('/').map(|s| s.trim()).collect();
            for n in names {
                let bare = n
                    .strip_prefix("fn ")
                    .unwrap_or(n);
                let bare = bare
                    .split('(')
                    .next()
                    .unwrap_or(bare)
                    .trim();
                if bare == name {
                    let desc = rest
                        .trim_start_matches('—')
                        .trim_start_matches("—")
                        .trim();
                    let desc = desc.trim_start_matches('—').trim();
                    return Some(format!("`{sig}` — {desc}"));
                }
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn all_modules_load() {
        let lib = library();
        assert_eq!(lib.len(), 15);
        for m in lib {
            assert!(!m.members.is_empty(), "module {} has no members", m.name);
        }
    }

    #[test]
    fn math_members() {
        let lib = library();
        let math = lib.iter().find(|m| m.name == "math").unwrap();
        assert!(math.member("sqrt").is_some());
        assert!(math.member("PI").is_some());
        let sqrt = math.member("sqrt").unwrap();
        assert_eq!(sqrt.signature, "fn sqrt(x)");
        assert!(sqrt.doc.contains("square root"));
        assert!(math.member("__lamo_std_math_sqrt").is_none());
    }

    #[test]
    fn io_members() {
        let lib = library();
        let io = lib.iter().find(|m| m.name == "io").unwrap();
        assert!(io.member("println").is_some());
        assert!(io.member("readLine").is_some());
    }

    #[test]
    fn uri_roundtrip() {
        let uri = std_uri("io");
        assert_eq!(uri.as_str(), "lamo-std://std/io.lamo");
        assert_eq!(module_from_uri(&uri).as_deref(), Some("io"));
    }
}
