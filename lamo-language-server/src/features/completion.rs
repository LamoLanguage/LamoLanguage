//! Completion: context-sensitive suggestions.
//!
//! Contexts handled, in priority order:
//! 1. `import ...` statement — std modules, workspace files, `as`
//! 2. after `.` — module members, struct fields/methods, array methods
//! 3. general — keywords, types, snippets, builtins, scope vars, top-level
//!    symbols, enum variants, module aliases

use super::resolve::{resolve_bare, Target};
use crate::analyzer::{Analysis, ModuleRef, SymKind};
use crate::lexer::Tok;
use tower_lsp::lsp_types::{
    CompletionItem, CompletionItemKind, CompletionResponse, Documentation, MarkupContent,
    MarkupKind, InsertTextFormat,
};

fn kw(label: &str, detail: &str) -> CompletionItem {
    CompletionItem {
        label: label.to_string(),
        kind: Some(CompletionItemKind::KEYWORD),
        detail: Some(detail.to_string()),
        ..Default::default()
    }
}

fn snippet(label: &str, body: &str, detail: &str) -> CompletionItem {
    CompletionItem {
        label: label.to_string(),
        kind: Some(CompletionItemKind::SNIPPET),
        detail: Some(detail.to_string()),
        insert_text: Some(body.to_string()),
        insert_text_format: Some(InsertTextFormat::SNIPPET),
        ..Default::default()
    }
}

fn builtin_item(name: &str, arity: usize, doc: &str) -> CompletionItem {
    let _ = arity;
    CompletionItem {
        label: name.to_string(),
        kind: Some(CompletionItemKind::FUNCTION),
        detail: Some(format!(
            "builtin fn {}({})",
            name,
            vec!["arg"; arity].join(", ")
        )),
        documentation: Some(Documentation::MarkupContent(MarkupContent {
            kind: MarkupKind::Markdown,
            value: doc.to_string(),
        })),
        insert_text: Some(name.to_string()),
        ..Default::default()
    }
}

const KEYWORDS: &[&str] = &[
    "let", "fn", "if", "else", "while", "for", "return", "break", "continue", "import", "as",
    "struct", "impl", "enum", "match", "true", "false",
];

const TYPE_NAMES: &[&str] = &["int", "float", "bool", "string", "void", "array"];

const CONSTRAINTS: &[&str] = &["Any", "Eq", "Ord", "Hash", "Num", "Show"];

pub fn completion(analysis: &Analysis, offset: usize, trigger: Option<&str>) -> Option<CompletionResponse> {
    let tokens = &analysis.file.tokens;
    let prev = tokens
        .iter()
        .filter(|t| t.span.end <= offset)
        .next_back();
    let prev2 = tokens
        .iter()
        .filter(|t| t.span.end <= prev.map(|p| p.span.start).unwrap_or(usize::MAX))
        .next_back();

    // ---- import context ----------------------------------------------------
    if in_import_context(tokens, offset) {
        let items = import_completions(analysis, prev, prev2);
        if !items.is_empty() {
            return Some(CompletionResponse::Array(items));
        }
        return None;
    }

    // ---- member access ------------------------------------------------------
    if let Some(p) = prev {
        if p.tok == Tok::Dot || trigger == Some(".") {
            let items = member_completions(analysis, offset, p.span.start);
            if !items.is_empty() {
                return Some(CompletionResponse::Array(items));
            }
            return None;
        }
    }

    // ---- general ------------------------------------------------------------
    Some(CompletionResponse::Array(general_completions(
        analysis, offset,
    )))
}

/// Are we between an `import` keyword and the end of that statement?
fn in_import_context(tokens: &[crate::lexer::Token], offset: usize) -> bool {
    let mut saw_import = false;
    for t in tokens {
        if t.span.start >= offset {
            break;
        }
        match t.tok {
            Tok::Import => saw_import = true,
            Tok::Semicolon | Tok::LBrace | Tok::RBrace => saw_import = false,
            Tok::Fn | Tok::Let | Tok::Struct | Tok::Enum | Tok::Impl | Tok::Return
            | Tok::If | Tok::While | Tok::For | Tok::Match => saw_import = false,
            _ => {}
        }
    }
    saw_import
}

fn import_completions(
    analysis: &Analysis,
    prev: Option<&crate::lexer::Token>,
    prev2: Option<&crate::lexer::Token>,
) -> Vec<CompletionItem> {
    let mut items = Vec::new();
    let after_std_dot = match (prev, prev2) {
        (Some(p), Some(p2)) => {
            p.tok == Tok::Dot && matches!(&p2.tok, Tok::Ident(n) if n == "std")
        }
        _ => false,
    };
    if after_std_dot {
        for m in crate::stdlib::library() {
            items.push(CompletionItem {
                label: m.name.clone(),
                kind: Some(CompletionItemKind::MODULE),
                detail: Some(format!("std.{}", m.name)),
                documentation: Some(Documentation::MarkupContent(MarkupContent {
                    kind: MarkupKind::Markdown,
                    value: m.doc.clone(),
                })),
                ..Default::default()
            });
        }
        return items;
    }
    if matches!(prev.map(|p| &p.tok), Some(Tok::Dot)) {
        // after some other dot in an import — nothing useful
        return items;
    }
    // suggest `std` and workspace .lamo files next to the current file
    items.push(kw("std", "standard library prefix"));
    let dir = analysis
        .file
        .uri
        .join(".")
        .ok()
        .and_then(|d| d.to_file_path().ok());
    if let Some(dir) = dir {
        if let Ok(rd) = std::fs::read_dir(&dir) {
            for e in rd.flatten() {
                let p = e.path();
                if p.extension().map(|x| x == "lamo").unwrap_or(false) {
                    if let Some(name) = p.file_stem().and_then(|s| s.to_str()) {
                        items.push(CompletionItem {
                            label: name.to_string(),
                            kind: Some(CompletionItemKind::MODULE),
                            detail: Some("workspace module".to_string()),
                            ..Default::default()
                        });
                    }
                }
            }
        }
    }
    items.push(kw("as", "import alias"));
    items
}

/// Resolve the receiver before the dot at `dot_pos` and produce members.
fn member_completions(
    analysis: &Analysis,
    offset: usize,
    dot_pos: usize,
) -> Vec<CompletionItem> {
    let tokens = &analysis.file.tokens;
    // find the ident right before the dot
    let receiver = tokens
        .iter()
        .filter(|t| t.span.end <= dot_pos)
        .next_back()
        .cloned();
    let name = match receiver {
        Some(t) => match t.tok {
            Tok::Ident(n) => n,
            _ => return vec![],
        },
        None => return vec![],
    };

    // 1. module alias?
    if let Some(mref) = analysis.modules.get(&name) {
        return module_members(mref);
    }
    // 2. `std.` prefix (mid-typing import or expression)
    if name == "std" {
        return crate::stdlib::library()
            .iter()
            .map(|m| CompletionItem {
                label: m.name.clone(),
                kind: Some(CompletionItemKind::MODULE),
                detail: Some(format!("std.{}", m.name)),
                ..Default::default()
            })
            .collect();
    }
    // 3. local var / self with a struct type?
    let mut var_ty: Option<crate::types::LType> = None;
    if name == "self" {
        // find enclosing impl type: nearest method containing offset
        for sym in &analysis.file.index.symbols {
            if sym.kind == SymKind::Method && sym.span.start <= offset && offset <= sym.span.end {
                var_ty = Some(crate::types::LType::Named(sym.owner.clone().unwrap_or_default()));
                break;
            }
        }
    } else {
        match resolve_bare(analysis, &name, offset) {
            Target::LocalVar { ty, .. } => var_ty = Some(ty),
            Target::Symbol { kind: SymKind::Let, .. } => {
                if let Some(sym) = analysis.file.index.find(&name) {
                    var_ty = Some(sym.ty.clone());
                }
            }
            _ => {}
        }
    }
    match var_ty {
        Some(crate::types::LType::Named(sname)) => {
            let mut items = Vec::new();
            if let Some(sym) = analysis.file.index.find(&sname) {
                for f in &sym.fields {
                    items.push(CompletionItem {
                        label: f.name.clone(),
                        kind: Some(CompletionItemKind::FIELD),
                        detail: Some(format!("{}: {}", f.name, f.ty.name())),
                        ..Default::default()
                    });
                }
            }
            for m in analysis.file.index.methods_of(&sname) {
                items.push(CompletionItem {
                    label: m.name.clone(),
                    kind: Some(CompletionItemKind::METHOD),
                    detail: Some(m.signature()),
                    insert_text: Some(m.name.clone()),
                    ..Default::default()
                });
            }
            items
        }
        Some(crate::types::LType::Array(_)) | Some(crate::types::LType::Unknown) => {
            // arrays: push/pop/len — offer for arrays and unknown receivers
            if matches!(var_ty, Some(crate::types::LType::Array(_))) {
                array_methods()
            } else {
                vec![] // unknown: don't guess
            }
        }
        _ => vec![],
    }
}

fn array_methods() -> Vec<CompletionItem> {
    crate::builtins::ARRAY_METHODS
        .iter()
        .map(|(n, arity, doc)| CompletionItem {
            label: n.to_string(),
            kind: Some(CompletionItemKind::METHOD),
            detail: Some(format!("array method — {}", doc)),
            insert_text: Some(n.to_string()),
            ..Default::default()
        })
        .collect()
}

fn module_members(mref: &ModuleRef) -> Vec<CompletionItem> {
    let mut items = Vec::new();
    match mref {
        ModuleRef::Std { module, .. } => {
            for m in &module.members {
                items.push(CompletionItem {
                    label: m.name.clone(),
                    kind: Some(if m.is_fn {
                        CompletionItemKind::FUNCTION
                    } else {
                        CompletionItemKind::CONSTANT
                    }),
                    detail: Some(m.signature.clone()),
                    documentation: if m.doc.is_empty() {
                        None
                    } else {
                        Some(Documentation::MarkupContent(MarkupContent {
                            kind: MarkupKind::Markdown,
                            value: m.doc.clone(),
                        }))
                    },
                    insert_text: Some(m.name.clone()),
                    ..Default::default()
                });
            }
        }
        ModuleRef::Workspace { analysis: fa, .. } => {
            for sym in &fa.index.symbols {
                if matches!(sym.kind, SymKind::Method) {
                    continue;
                }
                items.push(CompletionItem {
                    label: sym.name.clone(),
                    kind: Some(match sym.kind {
                        SymKind::Fn => CompletionItemKind::FUNCTION,
                        SymKind::Struct => CompletionItemKind::STRUCT,
                        SymKind::Enum => CompletionItemKind::ENUM,
                        SymKind::Trait => CompletionItemKind::INTERFACE,
                        _ => CompletionItemKind::VARIABLE,
                    }),
                    detail: Some(sym.signature()),
                    insert_text: Some(sym.name.clone()),
                    ..Default::default()
                });
            }
        }
    }
    items
}

fn general_completions(analysis: &Analysis, offset: usize) -> Vec<CompletionItem> {
    let mut items = Vec::new();

    // keywords
    for k in KEYWORDS {
        items.push(kw(k, "Lamo keyword"));
    }
    // snippets
    items.push(snippet(
        "fn",
        "fn ${1:name}(${2:args}) {\n\t${3:body}\n}",
        "function declaration",
    ));
    items.push(snippet(
        "struct",
        "struct ${1:Name} {\n\t${2:field}: ${3:int}\n}",
        "struct declaration",
    ));
    items.push(snippet(
        "if",
        "if (${1:cond}) {\n\t${2:body}\n}",
        "if statement",
    ));
    items.push(snippet(
        "ifelse",
        "if (${1:cond}) {\n\t${2:body}\n} else {\n\t${3:body}\n}",
        "if/else statement",
    ));
    items.push(snippet(
        "while",
        "while (${1:cond}) {\n\t${2:body}\n}",
        "while loop",
    ));
    items.push(snippet(
        "for",
        "for (let ${1:i} = 0; ${1:i} < ${2:n}; ${1:i}++) {\n\t${3:body}\n}",
        "for loop",
    ));
    items.push(snippet(
        "match",
        "match ${1:value} {\n\t${2:pattern} => ${3:body},\n\t_ => ${4:default}\n}",
        "match expression",
    ));
    items.push(snippet(
        "impl",
        "impl ${1:Name} {\n\tfn ${2:method}(${3:args}) {\n\t\t${4:body}\n\t}\n}",
        "impl block",
    ));
    // types
    for t in TYPE_NAMES {
        items.push(CompletionItem {
            label: t.to_string(),
            kind: Some(CompletionItemKind::KEYWORD),
            detail: Some("built-in type".to_string()),
            ..Default::default()
        });
    }
    for c in CONSTRAINTS {
        items.push(CompletionItem {
            label: c.to_string(),
            kind: Some(CompletionItemKind::TYPE_PARAMETER),
            detail: Some("generic constraint".to_string()),
            ..Default::default()
        });
    }
    // builtins
    for b in crate::builtins::BUILTINS {
        if b.category == crate::builtins::BuiltinCategory::Std {
            continue; // internal runtime fns — reached via std modules
        }
        items.push(builtin_item(b.name, b.arity, b.doc));
    }

    // scope variables (innermost last, deduped)
    let mut seen = std::collections::HashSet::new();
    for scope in analysis.file.scopes.iter() {
        if scope.start <= offset && offset <= scope.end {
            for (n, ty, _) in &scope.vars {
                if seen.insert(n.clone()) {
                    items.push(CompletionItem {
                        label: n.clone(),
                        kind: Some(CompletionItemKind::VARIABLE),
                        detail: Some(if ty.is_known() {
                            format!("let {}: {}", n, ty.name())
                        } else {
                            format!("let {}", n)
                        }),
                        sort_text: Some(format!("0{}", n)), // locals first
                        ..Default::default()
                    });
                }
            }
        }
    }
    // top-level symbols
    for sym in &analysis.file.index.symbols {
        if sym.kind == SymKind::Method || !seen.insert(sym.name.clone()) {
            continue;
        }
        items.push(CompletionItem {
            label: sym.name.clone(),
            kind: Some(match sym.kind {
                SymKind::Fn => CompletionItemKind::FUNCTION,
                SymKind::Struct => CompletionItemKind::STRUCT,
                SymKind::Enum => CompletionItemKind::ENUM,
                SymKind::Trait => CompletionItemKind::INTERFACE,
                _ => CompletionItemKind::VARIABLE,
            }),
            detail: Some(sym.signature()),
            insert_text: Some(sym.name.clone()),
            ..Default::default()
        });
        // bare enum variants
        if sym.kind == SymKind::Enum {
            for (vname, arity, _) in &sym.variants {
                items.push(CompletionItem {
                    label: vname.clone(),
                    kind: Some(CompletionItemKind::ENUM_MEMBER),
                    detail: Some(format!("variant of enum {}", sym.name)),
                    insert_text: Some(vname.clone()),
                    ..Default::default()
                });
            }
        }
    }
    // module aliases
    for alias in analysis.modules.keys() {
        items.push(CompletionItem {
            label: alias.clone(),
            kind: Some(CompletionItemKind::MODULE),
            detail: Some("imported module".to_string()),
            ..Default::default()
        });
    }
    items
}
