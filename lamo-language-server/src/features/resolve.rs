//! Shared resolution logic: given a cursor position, determine which
//! symbol/definition it refers to. Used by hover, goto-definition,
//! document highlight and completion.

use crate::analyzer::{Analysis, ModuleRef, UseTarget};
use crate::builtins;
use crate::lexer::{Span, Tok};
use crate::stdlib::{self, StdMember, StdModule};
use crate::types::LType;
use tower_lsp::lsp_types::Url;

/// What a reference points at.
#[derive(Debug, Clone)]
pub enum Target {
    /// Symbol in a workspace file (None uri = current file).
    Symbol {
        uri: Option<Url>,
        name: String,
        name_span: Span,
        signature: String,
        doc: Vec<String>,
        kind: crate::analyzer::SymKind,
    },
    Builtin(&'static builtins::BuiltinInfo),
    StdMember {
        module: &'static StdModule,
        member: StdMember,
    },
    StdModule(&'static StdModule),
    EnumVariant {
        uri: Option<Url>,
        enum_name: String,
        variant: String,
        name_span: Span,
    },
    StructField {
        uri: Option<Url>,
        struct_name: String,
        field: String,
        name_span: Span,
    },
    StructMethod {
        uri: Option<Url>,
        struct_name: String,
        method: String,
        name_span: Span,
        signature: String,
    },
    ArrayMethod(&'static str),
    LocalVar {
        name: String,
        ty: LType,
        decl_span: Span,
    },
    ModuleAlias {
        name: String,
        module: Option<&'static StdModule>,
        uri: Option<Url>,
    },
    TypeParam(String),
    None,
}

impl Target {
    pub fn is_none(&self) -> bool {
        matches!(self, Target::None)
    }
}

/// Find the token whose span contains `offset` (cursor may sit at the end).
pub fn token_at<'t>(
    tokens: &'t [crate::lexer::Token],
    offset: usize,
) -> Option<&'t crate::lexer::Token> {
    tokens.iter().find(|t| {
        t.span.start <= offset && offset <= t.span.end && !matches!(t.tok, Tok::Eof)
    })
}

/// The identifier token under (or just before) the cursor.
pub fn ident_context(tokens: &[crate::lexer::Token], offset: usize) -> Option<(String, Span, bool)> {
    let idx = tokens
        .iter()
        .position(|t| t.span.start <= offset && offset < t.span.end && !matches!(t.tok, Tok::Eof))
        // at token end boundary — accept the next token
        .or_else(|| {
            tokens
                .iter()
                .position(|t| t.span.start == offset && !matches!(t.tok, Tok::Eof))
        });
    let tok = tokens.get(idx?)?;
    let name = match &tok.tok {
        Tok::Ident(n) => n.clone(),
        _ => return None,
    };
    // is this ident preceded by a Dot (member access)?
    let after_dot = match idx {
        Some(i) if i > 0 => tokens[i - 1].tok == Tok::Dot,
        _ => false,
    };
    Some((name, tok.span, after_dot))
}

/// Resolve a bare identifier (not after a dot) using precomputed data.
pub fn resolve_bare(analysis: &Analysis, name: &str, offset: usize) -> Target {
    // 1. recorded uses (most precise: covers scope-aware var resolution)
    //    NOTE: a recorded use may fail to resolve to a target (e.g. locals are
    //    not in the top-level index) — fall through to the scope walk instead
    //    of returning Target::None early.
    if let Some(use_) = analysis
        .file
        .uses
        .iter()
        .find(|u| u.name == name && u.span.start <= offset && offset <= u.span.end)
    {
        if let Some(t) = use_to_target(analysis, use_) {
            if !t.is_none() {
                return t;
            }
        }
    }
    // 2. scope vars (completion-time, may not be recorded yet)
    for scope in analysis.file.scopes.iter().rev() {
        if scope.start <= offset && offset <= scope.end {
            if let Some((_, ty, span)) = scope
                .vars
                .iter()
                .rev()
                .find(|(n, _, _)| n == name)
            {
                return Target::LocalVar {
                    name: name.to_string(),
                    ty: ty.clone(),
                    decl_span: *span,
                };
            }
        }
    }
    // 3. top-level symbols
    if let Some(sym) = analysis.file.index.find(name) {
        return Target::Symbol {
            uri: Some(analysis.file.uri.clone()),
            name: sym.name.clone(),
            name_span: sym.name_span,
            signature: sym.signature(),
            doc: sym.doc.clone(),
            kind: sym.kind,
        };
    }
    // 4. builtins
    if let Some(b) = builtins::lookup_any(name) {
        return Target::Builtin(b);
    }
    // 5. enum variants (bare)
    if let Some(sym) = analysis.file.index.find(name) {
        // already covered above
        let _ = sym;
    }
    for sym in &analysis.file.index.symbols {
        if sym.kind == crate::analyzer::SymKind::Enum {
            if let Some((vname, _arity, vspan)) =
                sym.variants.iter().find(|(n, _, _)| n == name)
            {
                return Target::EnumVariant {
                    uri: Some(analysis.file.uri.clone()),
                    enum_name: sym.name.clone(),
                    variant: vname.clone(),
                    name_span: *vspan,
                };
            }
        }
    }
    // 6. module aliases
    if let Some(mref) = analysis.modules.get(name) {
        return module_ref_target(name, mref);
    }
    Target::None
}

fn module_ref_target(name: &str, mref: &ModuleRef) -> Target {
    match mref {
        ModuleRef::Std { module, .. } => Target::ModuleAlias {
            name: name.to_string(),
            module: Some(module),
            uri: None,
        },
        ModuleRef::Workspace { uri, .. } => Target::ModuleAlias {
            name: name.to_string(),
            module: None,
            uri: Some(uri.clone()),
        },
    }
}

fn use_to_target(analysis: &Analysis, u: &crate::analyzer::Use) -> Option<Target> {
    Some(match &u.target {
        UseTarget::Symbol {
            uri,
            name,
            name_span,
        } => {
            // find the symbol for signature info: own file or merged modules
            if uri.as_ref() == Some(&analysis.file.uri) || uri.is_none() {
                match analysis.file.index.find(name).cloned() {
                    Some(sym) => Target::Symbol {
                        uri: uri.clone().or_else(|| Some(analysis.file.uri.clone())),
                        name: sym.name.clone(),
                        name_span: sym.name_span,
                        signature: sym.signature(),
                        doc: sym.doc.clone(),
                        kind: sym.kind,
                    },
                    None => Target::None,
                }
            } else {
                let uri = uri.clone()?;
                let mref = analysis
                    .modules
                    .values()
                    .find(|m| m.uri() == &uri)
                    .or_else(|| analysis.merged.iter().find(|m| m.uri() == &uri))?;
                match mref {
                    ModuleRef::Workspace { analysis: fa, .. } => {
                        let sym = fa.index.find(name)?.clone();
                        Target::Symbol {
                            uri: Some(fa.uri.clone()),
                            name: sym.name.clone(),
                            name_span: sym.name_span,
                            signature: sym.signature(),
                            doc: sym.doc.clone(),
                            kind: sym.kind,
                        }
                    }
                    ModuleRef::Std { module, .. } => {
                        let member = module.member(name)?;
                        Target::StdMember { module, member: member.clone() }
                    }
                }
            }
        }
        UseTarget::Builtin(name) => Target::Builtin(builtins::lookup_any(name)?),
        UseTarget::StdMember { module, member } => {
            let mref = analysis.modules.get(module)?;
            match mref {
                ModuleRef::Std { module: sm, .. } => {
                    let m = sm.member(member)?;
                    Target::StdMember { module: sm, member: m.clone() }
                }
                ModuleRef::Workspace { analysis: fa, .. } => {
                    let sym = fa.index.find(member)?.clone();
                    Target::Symbol {
                        uri: Some(fa.uri.clone()),
                        name: sym.name.clone(),
                        name_span: sym.name_span,
                        signature: sym.signature(),
                        doc: sym.doc.clone(),
                        kind: sym.kind,
                    }
                }
            }
        }
        UseTarget::StdModule(name) => {
            let m = stdlib::library().iter().find(|m| m.name == *name)?;
            Target::StdModule(m)
        }
        UseTarget::EnumVariant {
            uri,
            enum_name,
            variant,
            name_span,
        } => Target::EnumVariant {
            uri: uri.clone(),
            enum_name: enum_name.clone(),
            variant: variant.clone(),
            name_span: *name_span,
        },
        UseTarget::StructField {
            uri,
            struct_name,
            field,
            name_span,
        } => Target::StructField {
            uri: uri.clone(),
            struct_name: struct_name.clone(),
            field: field.clone(),
            name_span: *name_span,
        },
        UseTarget::StructMethod {
            uri,
            struct_name,
            method,
            name_span,
        } => {
            let sym = analysis
                .file
                .index
                .symbols
                .iter()
                .find(|s| {
                    s.kind == crate::analyzer::SymKind::Method
                        && s.owner.as_deref() == Some(struct_name.as_str())
                        && s.name == *method
                })
                .cloned();
            match sym {
                Some(m) => Target::StructMethod {
                    uri: uri.clone(),
                    struct_name: struct_name.clone(),
                    method: method.clone(),
                    name_span: *name_span,
                    signature: m.signature(),
                },
                None => Target::StructMethod {
                    uri: uri.clone(),
                    struct_name: struct_name.clone(),
                    method: method.clone(),
                    name_span: *name_span,
                    signature: format!("fn {}(...)", method),
                },
            }
        }
        UseTarget::ArrayMethod(name) => Target::ArrayMethod(name),
        UseTarget::TypeParam(n) => Target::TypeParam(n.clone()),
        UseTarget::ModuleAlias(name) => {
            let mref = analysis.modules.get(name);
            match mref {
                Some(r) => module_ref_target(name, r),
                None => Target::ModuleAlias {
                    name: name.clone(),
                    module: None,
                    uri: None,
                },
            }
        }
        UseTarget::None => Target::None,
    })
}

/// Full resolution: the target of whatever is at `offset`.
pub fn resolve_at(analysis: &Analysis, offset: usize) -> Option<(Span, String, Target)> {
    // recorded use spanning the cursor?
    if let Some(u) = analysis
        .file
        .uses
        .iter()
        .find(|u| u.span.start <= offset && offset <= u.span.end)
    {
        if let Some(t) = use_to_target(analysis, u) {
            if !t.is_none() {
                return Some((u.span, u.name.clone(), t));
            }
        }
    }
    // bare ident under cursor
    if let Some((name, span, _after_dot)) = ident_context(&analysis.file.tokens, offset) {
        let t = resolve_bare(analysis, &name, offset);
        if !t.is_none() {
            return Some((span, name, t));
        }
    }
    None
}

/// Where a target's definition lives: (uri, byte span).
pub fn definition_of(
    analysis: &Analysis,
    target: &Target,
) -> Option<(Url, Span)> {
    let own = analysis.file.uri.clone();
    match target {
        Target::Symbol { uri, name_span, .. } => Some((uri.clone().unwrap_or(own), *name_span)),
        Target::EnumVariant { uri, name_span, .. } => {
            Some((uri.clone().unwrap_or(own), *name_span))
        }
        Target::StructField { uri, name_span, .. } => {
            Some((uri.clone().unwrap_or(own), *name_span))
        }
        Target::StructMethod { uri, name_span, .. } => {
            Some((uri.clone().unwrap_or(own), *name_span))
        }
        Target::LocalVar { decl_span, .. } => Some((own, *decl_span)),
        Target::StdMember { module, member } => {
            Some((stdlib::std_uri(&module.name), member.name_span))
        }
        Target::StdModule(m) => Some((stdlib::std_uri(&m.name), Span::new(0, 0))),
        Target::ModuleAlias { module, uri, .. } => match (module, uri) {
            (Some(m), _) => Some((stdlib::std_uri(&m.name), Span::new(0, 0))),
            (None, Some(u)) => Some((u.clone(), Span::new(0, 0))),
            _ => None,
        },
        _ => None,
    }
}
