//! Hover provider: markdown docs for builtins, std members, workspace
//! symbols, locals, fields and methods.

use super::resolve::{resolve_at, Target};
use crate::analyzer::Analysis;
use tower_lsp::lsp_types::{Hover, HoverContents, MarkupContent, MarkupKind};

fn md(code: &str, doc: &[String], footer: Option<String>) -> Hover {
    let mut text = String::from("```lamo\n");
    text.push_str(code);
    text.push_str("\n```");
    for d in doc {
        let trimmed = d.trim();
        if !trimmed.is_empty() {
            text.push_str("\n\n");
            text.push_str(trimmed);
        }
    }
    if let Some(f) = footer {
        text.push_str("\n\n---\n");
        text.push_str(&f);
    }
    Hover {
        contents: HoverContents::Markup(MarkupContent {
            kind: MarkupKind::Markdown,
            value: text,
        }),
        range: None,
    }
}

pub fn hover(analysis: &Analysis, offset: usize) -> Option<Hover> {
    let (_, _, target) = resolve_at(analysis, offset)?;
    Some(match target {
        Target::Symbol {
            signature, doc, kind, ..
        } => md(&signature, &doc, Some(format!("*{}*", kind.label()))),
        Target::Builtin(b) => md(
            &format!("fn {}({})", b.name, ", ".repeat(b.arity)),
            &[b.doc.to_string()],
            Some(format!(
                "*builtin ({}) — always available*",
                match b.category {
                    crate::builtins::BuiltinCategory::Lang => "language",
                    crate::builtins::BuiltinCategory::Gui => "GUI",
                    crate::builtins::BuiltinCategory::Http => "HTTP server",
                    crate::builtins::BuiltinCategory::Std => "std runtime",
                }
            )),
        ),
        Target::StdMember { module, member } => md(
            &member.signature,
            &[member.doc.clone()],
            Some(format!(
                "*std.{} {}*",
                module.name,
                if member.is_fn { "function" } else { "constant" }
            )),
        ),
        Target::StdModule(m) => Hover {
            contents: HoverContents::Markup(MarkupContent {
                kind: MarkupKind::Markdown,
                value: format!("**std.{}**\n\n{}", m.name, m.doc),
            }),
            range: None,
        },
        Target::EnumVariant {
            enum_name, variant, ..
        } => md(
            &format!("{}::{}", enum_name, variant),
            &[format!("Variant of enum `{}`.", enum_name)],
            None,
        ),
        Target::StructField {
            struct_name, field, ..
        } => md(&format!("field {} of {}", field, struct_name), &[], None),
        Target::StructMethod {
            struct_name, signature, ..
        } => md(&signature, &[], Some(format!("*method of {}*", struct_name))),
        Target::ArrayMethod(name) => {
            let entry = crate::builtins::ARRAY_METHODS
                .iter()
                .find(|(n, _, _)| *n == name);
            match entry {
                Some((n, arity, doc)) => md(
                    &format!("fn {}({})", n, ", ".repeat(*arity)),
                    &[doc.to_string()],
                    Some("*array method*".into()),
                ),
                None => return None,
            }
        }
        Target::LocalVar { name, ty, .. } => md(
            &if ty.is_known() {
                format!("let {}: {}", name, ty.name())
            } else {
                format!("let {}", name)
            },
            &[],
            Some("*local variable*".into()),
        ),
        Target::ModuleAlias { name, module, .. } => match module {
            Some(m) => Hover {
                contents: HoverContents::Markup(MarkupContent {
                    kind: MarkupKind::Markdown,
                    value: format!(
                        "**{}** — module alias for `std.{}`\n\n{}",
                        name, m.name, m.doc
                    ),
                }),
                range: None,
            },
            None => md(&format!("import {}", name), &[], Some("*module alias*".into())),
        },
        Target::TypeParam(n) => md(&format!("type parameter {}", n), &[], None),
        Target::None => return None,
    })
}
