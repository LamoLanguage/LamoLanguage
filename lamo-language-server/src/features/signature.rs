//! Signature help: shows the active function signature while typing args.

use super::resolve::{resolve_bare, Target};
use crate::analyzer::Analysis;
use crate::lexer::{Span, Tok};
use tower_lsp::lsp_types::{
    Documentation, MarkupContent, MarkupKind, ParameterInformation, ParameterLabel,
    SignatureHelp, SignatureInformation,
};

/// Find the innermost open call at `offset`: returns (callee name span,
/// comma depth). Scans tokens backwards with nesting awareness.
fn enclosing_call(tokens: &[crate::lexer::Token], offset: usize) -> Option<(Span, String, usize)> {
    let mut depth = 0i32;
    let mut commas = 0i32;
    let mut i = tokens.len();
    // scan tokens from the end backwards until we cross the open paren
    let mut found_open: Option<usize> = None;
    while i > 0 {
        i -= 1;
        let t = &tokens[i];
        if t.span.end > offset + 1 && t.span.start >= offset {
            continue; // only tokens strictly before the cursor
        }
        if t.span.start >= offset {
            continue;
        }
        match t.tok {
            Tok::RParen => depth += 1,
            Tok::LParen => {
                if depth == 0 {
                    found_open = Some(i);
                    break;
                }
                depth -= 1;
            }
            Tok::Comma if depth == 0 => commas += 1,
            Tok::Gt | Tok::Lt => { /* turbofish — part of callee; ignore */ }
            _ => {}
        }
    }
    let open_idx = found_open?;
    // callee = token before `(`, skipping `< ... >` type args
    let mut j = open_idx;
    if j > 0 && tokens[j - 1].tok == Tok::Gt {
        // skip balanced < ... >
        let mut d = 0i32;
        while j > 0 {
            j -= 1;
            match tokens[j].tok {
                Tok::Gt => d += 1,
                Tok::Lt => {
                    d -= 1;
                    if d == 0 {
                        break;
                    }
                }
                _ => {}
            }
        }
    }
    let callee_tok = tokens.get(j.checked_sub(1)?)?;
    let name = match &callee_tok.tok {
        Tok::Ident(n) => n.clone(),
        _ => return None,
    };
    Some((callee_tok.span, name, commas as usize))
}

/// Resolve the callee to a param-count + label pair.
fn callee_signature<'a>(
    analysis: &'a Analysis,
    name: &str,
    offset: usize,
) -> Option<(String, Vec<String>, Option<String>)> {
    // module member? `io.println(` — token two before is Ident alias
    if let Some(dot_idx) = analysis
        .file
        .tokens
        .iter()
        .position(|t| t.tok == Tok::Dot && t.span.end <= offset && offset - t.span.end < 4)
    {
        let _ = dot_idx;
    }
    // Try module member: find the alias by scanning back for `alias . name (`
    let tokens = &analysis.file.tokens;
    for idx in (1..tokens.len()).rev() {
        if tokens[idx].tok != Tok::Dot || tokens[idx].span.end > offset {
            continue;
        }
        // alias . name  where name matches and `(` right after name
        let (alias, member) = (tokens[idx - 1].clone(), tokens.get(idx + 1)?.clone());
        if let (Tok::Ident(a), Tok::Ident(m)) = (&alias.tok, &member.tok) {
            if m == name && member.span.end <= offset + 1 {
                if let Some(mref) = analysis.modules.get(a) {
                    if let Some(mi) = mref.find_member(m) {
                        let (label, params) = std_member_sig(a, m, mref);
                        return Some((label, params, mi.doc.first().cloned()));
                    }
                }
            }
        }
    }

    match resolve_bare(analysis, name, offset) {
        Target::Symbol { signature, kind, .. } if matches!(
            kind,
            crate::analyzer::SymKind::Fn | crate::analyzer::SymKind::Method
        ) =>
        {
            let sym = analysis.file.index.find(name)?;
            Some((
                signature,
                sym.params.iter().map(|p| p.name.clone()).collect(),
                sym.doc.first().cloned(),
            ))
        }
        Target::Builtin(b) => Some((
            format!(
                "fn {}({}) -> {}",
                b.name,
                (0..b.arity).map(|i| format!("arg{}", i + 1)).collect::<Vec<_>>().join(", "),
                b.ret
            ),
            (0..b.arity).map(|i| format!("arg{}", i + 1)).collect(),
            Some(b.doc.to_string()),
        )),
        Target::StdMember { .. } => None, // handled via module scan above
        _ => {
            // array method?
            if let Some((n, arity, doc)) = crate::builtins::ARRAY_METHODS
                .iter()
                .find(|(n, _, _)| *n == name)
            {
                return Some((
                    format!("fn {}({})", n, ", ".repeat(*arity)),
                    (0..*arity).map(|i| format!("arg{}", i + 1)).collect(),
                    Some(doc.to_string()),
                ));
            }
            None
        }
    }
}

fn std_member_sig(alias: &str, member: &str, mref: &crate::analyzer::ModuleRef) -> (String, Vec<String>) {
    if let crate::analyzer::ModuleRef::Std { module, .. } = mref {
        if let Some(m) = module.member(member) {
            if m.is_fn {
                let params = m
                    .signature
                    .strip_prefix("fn ")
                    .and_then(|s| s.find('(').map(|i| &s[i + 1..]))
                    .and_then(|s| s.strip_suffix(')'))
                    .unwrap_or("")
                    .split(", ")
                    .filter(|s| !s.is_empty())
                    .map(|s| s.to_string())
                    .collect();
                return (format!("{}.{}` — `{}`", alias, member, m.signature), params);
            }
        }
    }
    (format!("{}.{}(...)", alias, member), vec![])
}

pub fn signature_help(analysis: &Analysis, offset: usize) -> Option<SignatureHelp> {
    let (callee_span, name, active) = enclosing_call(&analysis.file.tokens, offset)?;
    let _ = callee_span;
    let (label, params, doc) = callee_signature(analysis, &name, offset)?;

    let parameters: Vec<ParameterInformation> = params
        .iter()
        .map(|p| ParameterInformation {
            label: ParameterLabel::Simple(p.clone()),
            documentation: None::<Documentation>,
        })
        .collect();

    let mut sig = SignatureInformation {
        label,
        documentation: doc.map(|d| {
            Documentation::MarkupContent(MarkupContent {
                kind: MarkupKind::Markdown,
                value: d,
            })
        }),
        parameters: Some(parameters),
        active_parameter: None,
    };
    sig.active_parameter = Some(active.min(params.len().saturating_sub(1)) as u32);

    Some(SignatureHelp {
        signatures: vec![sig],
        active_signature: Some(0),
        active_parameter: Some(active.min(params.len().saturating_sub(1)) as u32),
    })
}
