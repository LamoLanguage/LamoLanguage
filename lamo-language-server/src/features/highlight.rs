//! Document highlight: all references of the symbol under the cursor.
//!
//! Identity rule: two references denote the same symbol when their
//! recorded UseTargets are equal, or when their declaration spans match
//! (robust for local variables and same-file symbols).

use super::resolve::{resolve_at, Target};
use crate::analyzer::{Analysis, UseTarget};
use crate::line_index::LineIndex;
use tower_lsp::lsp_types::{DocumentHighlight, DocumentHighlightKind, Range};

pub fn highlight(
    analysis: &Analysis,
    line_index: &LineIndex,
    offset: usize,
) -> Option<Vec<DocumentHighlight>> {
    let (cursor_span, _, target) = resolve_at(analysis, offset)?;
    if target.is_none() {
        return None;
    }
    let cursor_decl = decl_span_of(&target);

    let to_range = |span: crate::lexer::Span| {
        Range::new(line_index.position(span.start), line_index.position(span.end))
    };

    let mut out: Vec<DocumentHighlight> = Vec::new();
    for u in &analysis.file.uses {
        let eq = use_targets_same(analysis, &u.target, &target, &cursor_decl);
        if eq {
            out.push(DocumentHighlight {
                range: to_range(u.span),
                kind: Some(if u.is_write {
                    DocumentHighlightKind::WRITE
                } else {
                    DocumentHighlightKind::READ
                }),
            });
        }
    }
    // always include the cursor occurrence
    if !out
        .iter()
        .any(|h| h.range.start == to_range(cursor_span).start)
    {
        out.push(DocumentHighlight {
            range: to_range(cursor_span),
            kind: Some(DocumentHighlightKind::TEXT),
        });
    }
    if out.is_empty() {
        None
    } else {
        Some(out)
    }
}

/// The declaration span a target points at (identity anchor).
fn decl_span_of(target: &Target) -> Option<crate::lexer::Span> {
    match target {
        Target::Symbol { name_span, .. }
        | Target::EnumVariant { name_span, .. }
        | Target::StructField { name_span, .. }
        | Target::StructMethod { name_span, .. }
        | Target::LocalVar { decl_span: name_span, .. } => Some(*name_span),
        _ => None,
    }
}

fn use_targets_same(
    analysis: &Analysis,
    a: &UseTarget,
    b: &Target,
    cursor_decl: &Option<crate::lexer::Span>,
) -> bool {
    let b_decl = decl_span_of(b);
    // 1. decl-span identity (works across scopes and uri-less targets)
    if let (Some(ad), Some(bd)) = (decl_span_of_use(a), &b_decl) {
        if ad == *bd {
            return true;
        }
    }
    // 2. name equality for std members / builtins / array methods
    if let (Some(an), Some(bn)) = (use_name(a), target_name(b)) {
        if an == bn {
            // same name AND same kind — avoid matching `len` the builtin
            // with `len` an array method... they are actually the same fn.
            return true;
        }
    }
    let _ = analysis;
    false
}

fn decl_span_of_use(u: &UseTarget) -> Option<crate::lexer::Span> {
    match u {
        UseTarget::Symbol { name_span, .. }
        | UseTarget::EnumVariant { name_span, .. }
        | UseTarget::StructField { name_span, .. }
        | UseTarget::StructMethod { name_span, .. } => Some(*name_span),
        _ => None,
    }
}

fn use_name(u: &UseTarget) -> Option<String> {
    match u {
        UseTarget::Builtin(n) => Some(n.to_string()),
        UseTarget::StdMember { member, .. } => Some(member.clone()),
        UseTarget::ArrayMethod(n) => Some(n.to_string()),
        UseTarget::ModuleAlias(n) => Some(n.clone()),
        UseTarget::StdModule(n) => Some(n.clone()),
        UseTarget::TypeParam(n) => Some(n.clone()),
        _ => None,
    }
}

fn target_name(t: &Target) -> Option<String> {
    match t {
        Target::Builtin(b) => Some(b.name.to_string()),
        Target::StdMember { member, .. } => Some(member.name.clone()),
        Target::ArrayMethod(n) => Some(n.to_string()),
        Target::ModuleAlias { name: n, .. } => Some(n.clone()),
        Target::StdModule(m) => Some(m.name.clone()),
        Target::TypeParam(n) => Some(n.clone()),
        Target::LocalVar { name, .. } => Some(name.clone()),
        Target::Symbol { name, .. } => Some(name.clone()),
        _ => None,
    }
}
