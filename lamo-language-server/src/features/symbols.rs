//! Document symbols (outline).

use crate::analyzer::{Analysis, SymKind};
use crate::line_index::LineIndex;
use crate::lexer::Span;
use tower_lsp::lsp_types::{
    DocumentSymbol, DocumentSymbolParams, DocumentSymbolResponse, SymbolKind,
};

pub fn document_symbols(
    analysis: &Analysis,
    line_index: &LineIndex,
    _params: &DocumentSymbolParams,
) -> Option<DocumentSymbolResponse> {
    let mut out = Vec::new();
    let to_range = |span: Span| {
        tower_lsp::lsp_types::Range::new(
            line_index.position(span.start),
            line_index.position(span.end),
        )
    };

    for sym in &analysis.file.index.symbols {
        match sym.kind {
            SymKind::Fn => {
                out.push(DocumentSymbol {
                    name: sym.name.clone(),
                    detail: Some(sym.signature()),
                    kind: SymbolKind::FUNCTION,
                    tags: None,
                    deprecated: None,
                    range: to_range(sym.span),
                    selection_range: to_range(sym.name_span),
                    children: None,
                });
            }
            SymKind::Method => {
                if let Some(owner) = &sym.owner {
                    // attach under the struct symbol if present
                    if let Some(parent) = out
                        .iter_mut()
                        .find(|s| s.name == format!("struct {}", owner))
                    {
                        let child = DocumentSymbol {
                            name: sym.name.clone(),
                            detail: Some(sym.signature()),
                            kind: SymbolKind::METHOD,
                            tags: None,
                            deprecated: None,
                            range: to_range(sym.span),
                            selection_range: to_range(sym.name_span),
                            children: None,
                        };
                        parent.children.get_or_insert_with(Vec::new).push(child);
                        continue;
                    }
                }
                out.push(DocumentSymbol {
                    name: sym.name.clone(),
                    detail: Some(sym.signature()),
                    kind: SymbolKind::METHOD,
                    tags: None,
                    deprecated: None,
                    range: to_range(sym.span),
                    selection_range: to_range(sym.name_span),
                    children: None,
                });
            }
            SymKind::Let => {
                out.push(DocumentSymbol {
                    name: sym.name.clone(),
                    detail: Some(sym.signature()),
                    kind: SymbolKind::VARIABLE,
                    tags: None,
                    deprecated: None,
                    range: to_range(sym.span),
                    selection_range: to_range(sym.name_span),
                    children: None,
                });
            }
            SymKind::Struct => {
                let children: Vec<DocumentSymbol> = sym
                    .fields
                    .iter()
                    .map(|f| DocumentSymbol {
                        name: f.name.clone(),
                        detail: Some(f.ty.name()),
                        kind: SymbolKind::FIELD,
                        tags: None,
                        deprecated: None,
                        range: to_range(f.name_span),
                        selection_range: to_range(f.name_span),
                        children: None,
                    })
                    .collect();
                out.push(DocumentSymbol {
                    name: format!("struct {}", sym.name),
                    detail: Some(sym.signature()),
                    kind: SymbolKind::STRUCT,
                    tags: None,
                    deprecated: None,
                    range: to_range(sym.span),
                    selection_range: to_range(sym.name_span),
                    children: Some(children),
                });
            }
            SymKind::Enum => {
                let children: Vec<DocumentSymbol> = sym
                    .variants
                    .iter()
                    .map(|(n, _arity, sp)| DocumentSymbol {
                        name: n.clone(),
                        detail: None,
                        kind: SymbolKind::ENUM_MEMBER,
                        tags: None,
                        deprecated: None,
                        range: to_range(*sp),
                        selection_range: to_range(*sp),
                        children: None,
                    })
                    .collect();
                out.push(DocumentSymbol {
                    name: format!("enum {}", sym.name),
                    detail: Some(sym.signature()),
                    kind: SymbolKind::ENUM,
                    tags: None,
                    deprecated: None,
                    range: to_range(sym.span),
                    selection_range: to_range(sym.name_span),
                    children: Some(children),
                });
            }
            SymKind::Trait => {
                out.push(DocumentSymbol {
                    name: format!("trait {}", sym.name),
                    detail: Some(sym.signature()),
                    kind: SymbolKind::INTERFACE,
                    tags: None,
                    deprecated: None,
                    range: to_range(sym.span),
                    selection_range: to_range(sym.name_span),
                    children: None,
                });
            }
        }
    }

    // imports
    for imp in &analysis.file.index.imports {
        out.push(DocumentSymbol {
            name: imp.alias.clone().unwrap_or_else(|| imp.path.clone()),
            detail: Some(format!("import {}", imp.path)),
            kind: SymbolKind::MODULE,
            tags: None,
            deprecated: None,
            range: to_range(imp.span),
            selection_range: to_range(imp.path_span),
            children: None,
        });
    }

    if out.is_empty() {
        None
    } else {
        Some(DocumentSymbolResponse::Nested(out))
    }
}
