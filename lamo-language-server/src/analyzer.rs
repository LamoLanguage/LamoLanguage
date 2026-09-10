//! Semantic analyzer: per-file analysis (symbols, scopes, diagnostics) and
//! import resolution across the workspace and the embedded std library.
//!
//! Philosophy: never report a diagnostic without confidence. When a type or
//! symbol is unknown (e.g. inferred from an unannotated fn), checks are
//! skipped rather than guessed.

use std::collections::HashMap;
use std::sync::Arc;

use tower_lsp::lsp_types::Url;

use crate::ast::*;
use crate::builtins;
use crate::lexer::{self, Comment, LexError, Span, Tok, Token};
use crate::parser::{self, ParseError};
use crate::stdlib::{self, StdModule};
use crate::types::LType;

pub use crate::types::BUILTIN_TYPE_NAMES;

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
    Error,
    Warning,
    Info,
    Hint,
}

#[derive(Debug, Clone)]
pub struct Diag {
    pub span: Span,
    pub severity: Severity,
    pub message: String,
    pub code: &'static str,
}

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct FieldInfo {
    pub name: String,
    pub name_span: Span,
    pub ty: LType,
}

#[derive(Debug, Clone)]
pub struct ParamInfo {
    pub name: String,
    pub ty: LType,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SymKind {
    Fn,
    Method,
    Let,
    Struct,
    Enum,
    Trait,
}

impl SymKind {
    pub fn label(&self) -> &'static str {
        match self {
            SymKind::Fn => "function",
            SymKind::Method => "method",
            SymKind::Let => "variable",
            SymKind::Struct => "struct",
            SymKind::Enum => "enum",
            SymKind::Trait => "trait",
        }
    }
}

#[derive(Debug, Clone)]
pub struct Symbol {
    pub name: String,
    pub kind: SymKind,
    /// Declaring type for methods.
    pub owner: Option<String>,
    pub name_span: Span,
    pub span: Span,
    pub is_pub: bool,
    pub doc: Vec<String>,
    pub params: Vec<ParamInfo>,
    pub ret: Option<LType>,
    pub ty: LType,
    pub type_params: Vec<String>,
    pub fields: Vec<FieldInfo>,
    /// For enum symbols: (variant name, payload arity, name span).
    pub variants: Vec<(String, usize, Span)>,
    /// For traits: required (method, arity) pairs.
    pub trait_methods: Vec<(String, usize)>,
}

impl Symbol {
    pub fn signature(&self) -> String {
        match self.kind {
            SymKind::Fn | SymKind::Method => {
                let mut tp = String::new();
                if !self.type_params.is_empty() {
                    tp = format!("<{}>", self.type_params.join(", "));
                }
                let params = self
                    .params
                    .iter()
                    .map(|p| match &p.ty {
                        LType::Unknown => p.name.clone(),
                        t => format!("{}: {}", p.name, t.name()),
                    })
                    .collect::<Vec<_>>()
                    .join(", ");
                let mut sig = format!("fn {}{}({})", self.name, tp, params);
                if let Some(ret) = &self.ret {
                    if ret.is_known() {
                        sig.push_str(" -> ");
                        sig.push_str(&ret.name());
                    }
                }
                sig
            }
            SymKind::Let => {
                if self.ty.is_known() {
                    format!("let {}: {}", self.name, self.ty.name())
                } else {
                    format!("let {}", self.name)
                }
            }
            SymKind::Struct => {
                let fields = self
                    .fields
                    .iter()
                    .map(|f| format!("{}: {}", f.name, f.ty.name()))
                    .collect::<Vec<_>>()
                    .join(", ");
                format!("struct {} {{ {} }}", self.name, fields)
            }
            SymKind::Enum => {
                let vs = self
                    .variants
                    .iter()
                    .map(|(n, arity, _)| {
                        if *arity == 0 {
                            n.clone()
                        } else {
                            format!("{}({})", n, ", ".repeat(*arity))
                        }
                    })
                    .collect::<Vec<_>>()
                    .join(", ");
                format!("enum {} {{ {} }}", self.name, vs)
            }
            SymKind::Trait => {
                let ms = self
                    .trait_methods
                    .iter()
                    .map(|(n, a)| format!("{}({})", n, ", ".repeat(*a)))
                    .collect::<Vec<_>>()
                    .join(", ");
                format!("trait {} {{ {} }}", self.name, ms)
            }
        }
    }
}

#[derive(Debug, Clone)]
pub struct ImportInfo {
    pub path: String,
    pub path_span: Span,
    pub alias: Option<String>,
    pub alias_span: Option<Span>,
    pub span: Span,
}

/// Index of one file's top-level structure.
#[derive(Debug, Clone, Default)]
pub struct FileIndex {
    pub symbols: Vec<Symbol>,
    pub imports: Vec<ImportInfo>,
}

impl FileIndex {
    pub fn find(&self, name: &str) -> Option<&Symbol> {
        self.symbols.iter().find(|s| s.name == name)
    }

    pub fn methods_of(&self, struct_name: &str) -> Vec<&Symbol> {
        self.symbols
            .iter()
            .filter(|s| s.kind == SymKind::Method && s.owner.as_deref() == Some(struct_name))
            .collect()
    }
}

// ---------------------------------------------------------------------------
// Uses (recorded identifier resolutions, for goto-def / highlight)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
pub enum UseTarget {
    /// Top-level symbol in some file (uri None = the file itself).
    Symbol {
        uri: Option<Url>,
        name: String,
        name_span: Span,
    },
    Builtin(&'static str),
    /// Member of a std module: `io.println`, `math.PI`.
    StdMember { module: String, member: String },
    StdModule(String),
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
    },
    ArrayMethod(&'static str),
    TypeParam(String),
    /// A module alias identifier itself.
    ModuleAlias(String),
    /// Resolved but with no definition site.
    None,
}

#[derive(Debug, Clone)]
pub struct Use {
    /// Span of the referenced identifier token.
    pub span: Span,
    pub name: String,
    pub target: UseTarget,
    /// Write reference (assignment target)?
    pub is_write: bool,
}

// ---------------------------------------------------------------------------
// Scopes (for completion)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct ScopeInfo {
    pub start: usize,
    pub end: usize,
    pub vars: Vec<(String, LType, Span)>,
}

// ---------------------------------------------------------------------------
// File analysis
// ---------------------------------------------------------------------------

#[derive(Debug)]
pub struct FileAnalysis {
    pub uri: Url,
    pub tokens: Vec<Token>,
    pub comments: Vec<Comment>,
    pub program: Program,
    pub index: FileIndex,
    pub uses: Vec<Use>,
    pub scopes: Vec<ScopeInfo>,
    /// lexer + parser + semantic diagnostics.
    pub diags: Vec<Diag>,
}

/// Fully resolved context for one file: its own analysis plus imported
/// modules (aliased and legacy merged).
pub struct Analysis {
    pub file: FileAnalysis,
    /// alias -> resolved module
    pub modules: HashMap<String, ModuleRef>,
    /// legacy merged imports (`import "x.lamo"` with no alias)
    pub merged: Vec<ModuleRef>,
}

// ---------------------------------------------------------------------------
// Import environment
// ---------------------------------------------------------------------------

/// How an import was resolved.
#[derive(Debug, Clone)]
pub enum ModuleRef {
    /// A real .lamo file (open document or on disk).
    Workspace { uri: Url, analysis: Arc<FileAnalysis> },
    /// An embedded std module.
    Std { module: &'static StdModule, uri: Url },
}

impl ModuleRef {
    pub fn uri(&self) -> &Url {
        match self {
            ModuleRef::Workspace { uri, .. } => uri,
            ModuleRef::Std { uri, .. } => uri,
        }
    }

    pub fn find_member(&self, name: &str) -> Option<MemberInfo> {
        match self {
            ModuleRef::Workspace { analysis, .. } => {
                analysis.index.find(name).map(|s| MemberInfo {
                    name: s.name.clone(),
                    is_pub: s.is_pub,
                    kind: s.kind,
                    params: s.params.clone(),
                    ret: s.ret.clone(),
                    ty: s.ty.clone(),
                    name_span: s.name_span,
                    uri: analysis.uri.clone(),
                    doc: s.doc.clone(),
                })
            }
            ModuleRef::Std { module, uri } => module.member(name).map(|m| MemberInfo {
                name: m.name.clone(),
                is_pub: true,
                kind: if m.is_fn { SymKind::Fn } else { SymKind::Let },
                params: vec![],
                ret: None,
                ty: LType::Unknown,
                name_span: m.name_span,
                uri: uri.clone(),
                doc: vec![m.doc.clone()],
            }),
        }
    }

    pub fn std_module(&self) -> Option<&'static StdModule> {
        match self {
            ModuleRef::Std { module, .. } => Some(module),
            _ => None,
        }
    }
}

#[derive(Debug, Clone)]
pub struct MemberInfo {
    pub name: String,
    pub is_pub: bool,
    pub kind: SymKind,
    pub params: Vec<ParamInfo>,
    pub ret: Option<LType>,
    pub ty: LType,
    pub name_span: Span,
    pub uri: Url,
    pub doc: Vec<String>,
}

pub enum LoadResult {
    Loaded(Url, Arc<FileAnalysis>),
    NotFound,
    /// The file is already being analyzed up the stack: import cycle.
    Cycle,
}

/// Environment for resolving imports to workspace files or std modules.
pub trait ImportEnv {
    fn load(&mut self, importing: &Url, import_path: &str) -> LoadResult;
}

/// Analyze a file, resolving imports through `env`.
pub fn analyze_env(uri: &Url, text: &str, env: &mut dyn ImportEnv) -> Analysis {
    let mut diags = Vec::new();
    let lexed = lexer::lex(text);
    for err in &lexed.errors {
        diags.push(Diag {
            span: err.span,
            severity: Severity::Error,
            message: err.message.clone(),
            code: "lex",
        });
    }
    let parsed = parser::parse(text, &lexed);
    for err in &parsed.errors {
        diags.push(Diag {
            span: err.span,
            severity: Severity::Error,
            message: err.message.clone(),
            code: "parse",
        });
    }

    let mut checker = Checker::new(uri.clone());
    let imports = checker.collect_imports(&parsed.program);

    // Resolve imports (aliased + merged) before the semantic walk so that
    // alias.member checks and merged-symbol lookups work.
    let mut modules: HashMap<String, ModuleRef> = HashMap::new();
    let mut merged: Vec<ModuleRef> = Vec::new();
    
    for imp in &imports {
        if imp.path == "std" {
            diags.push(Diag {
                span: imp.path_span,
                severity: Severity::Error,
                message: "cannot import bare 'std' — import a module like 'std.io'".into(),
                code: "import",
            });
            continue;
        }
        let is_std = imp.path.starts_with("std.");
        let is_string_file = imp.path.ends_with(".lamo") || imp.path.contains('/');
        let legacy_merge = is_string_file && imp.alias.is_none() && !is_std;

        let alias: Option<String> = if legacy_merge {
            None
        } else if is_std {
            Some(
                imp.alias
                    .clone()
                    .unwrap_or_else(|| imp.path.rsplit('.').next().unwrap_or("std").to_string()),
            )
        } else {
            Some(
                imp.alias
                    .clone()
                    .unwrap_or_else(|| {
                        imp.path
                            .trim_end_matches(".lamo")
                            .rsplit('/')
                            .next()
                            .unwrap_or("module")
                            .to_string()
                    }),
            )
        };

        if legacy_merge {
            match env.load(uri, &imp.path) {
                LoadResult::Loaded(target, analysis) => {
                    if !merged.iter().any(|m: &ModuleRef| m.uri() == &target)
                        && target != *uri
                    {
                        merged.push(ModuleRef::Workspace { uri: target, analysis });
                    }
                }
                LoadResult::NotFound => diags.push(Diag {
                    span: imp.path_span,
                    severity: Severity::Error,
                    message: format!("cannot resolve module '{import}'", import = imp.path),
                    code: "import",
                }),
                LoadResult::Cycle => diags.push(Diag {
                    span: imp.path_span,
                    severity: Severity::Error,
                    message: format!("import cycle detected while loading '{import}'", import = imp.path),
                    code: "import",
                }),
            }
            continue;
        }

        let loaded: Option<(Url, ModuleRef)> = if is_std {
            let mod_name = imp.path.strip_prefix("std.").unwrap().to_string();
            match stdlib::library().iter().find(|m| m.name == mod_name) {
                Some(m) => Some((
                    stdlib::std_uri(&mod_name),
                    ModuleRef::Std { module: m, uri: stdlib::std_uri(&mod_name) },
                )),
                None => {
                    diags.push(Diag {
                        span: imp.path_span,
                        severity: Severity::Error,
                        message: format!("unknown std module '{import}'", import = imp.path),
                        code: "import",
                    });
                    None
                }
            }
        } else {
            match env.load(uri, &imp.path) {
                LoadResult::Loaded(target, analysis) => {
                    Some((target.clone(), ModuleRef::Workspace { uri: target, analysis }))
                }
                LoadResult::NotFound => {
                    diags.push(Diag {
                        span: imp.path_span,
                        severity: Severity::Error,
                        message: format!("cannot resolve module '{import}'", import = imp.path),
                        code: "import",
                    });
                    None
                }
                LoadResult::Cycle => {
                    diags.push(Diag {
                        span: imp.path_span,
                        severity: Severity::Error,
                        message: format!(
                            "import cycle detected while loading '{import}'",
                            import = imp.path
                        ),
                        code: "import",
                    });
                    None
                }
            }
        };

        if let Some((target, mref)) = loaded {
            if let Some(a) = alias {
                if modules.contains_key(&a) {
                    diags.push(Diag {
                        span: imp.alias_span.unwrap_or(imp.path_span),
                        severity: Severity::Error,
                        message: format!(
                            "failed to register module alias '{a}': a module with this name is already imported"
                        ),
                        code: "import",
                    });
                } else {
                    checker.register_alias(&a, imp.span);
                    checker.owned_modules.insert(a.clone(), mref.clone());
                    modules.insert(a, mref);
                }
            }
            let _ = target;
        }
    }

    checker.attach_merged(&merged);
    checker.check_program(&parsed.program);
    diags.extend(checker.diags);

    Analysis {
        file: FileAnalysis {
            uri: uri.clone(),
            tokens: lexed.tokens,
            comments: lexed.comments,
            program: parsed.program,
            index: checker.index,
            uses: checker.uses,
            scopes: checker.scopes,
            diags,
        },
        modules,
        merged,
    }
}

/// Convenience for files with no imports (tests, std modules).
pub fn analyze(uri: &Url, text: &str) -> Analysis {
    struct NullEnv;
    impl ImportEnv for NullEnv {
        fn load(&mut self, _importing: &Url, _path: &str) -> LoadResult {
            LoadResult::NotFound
        }
    }
    analyze_env(uri, text, &mut NullEnv)
}

// ---------------------------------------------------------------------------
// Checker
// ---------------------------------------------------------------------------

#[derive(Clone)]
struct VarEntry {
    name: String,
    ty: LType,
    decl_span: Span,
}

struct Checker<'a> {
    uri: Url,
    #[allow(dead_code)]
    marker: std::marker::PhantomData<&'a ()>,
    diags: Vec<Diag>,
    uses: Vec<Use>,
    scopes: Vec<ScopeInfo>,
    scope_stack: Vec<VarEntry>,
    scope_spans: Vec<(usize, usize)>,
    type_params: Vec<String>,
    /// enclosing impl struct name (declares `self` inside methods)
    self_ty: Option<String>,
    loop_depth: usize,
    fn_depth: usize,
    top_syms: HashMap<String, Span>,
    /// struct methods by struct name (own file)
    methods: HashMap<String, Vec<Symbol>>,
    enums: HashMap<String, Vec<(String, usize, Span)>>,
    /// import aliases registered before the walk
    aliases: HashMap<String, Span>,
    /// resolved modules by alias (for member access during the walk)
    owned_modules: HashMap<String, ModuleRef>,
    /// legacy merged symbols: fns / structs / enums by name
    merged_fns: HashMap<String, Symbol>,
    merged_structs: HashMap<String, Symbol>,
    merged_enums: HashMap<String, Vec<(String, usize, Span)>>,
    index: FileIndex,
}

impl<'a> Checker<'a> {
    fn new(uri: Url) -> Self {
        Checker {
            uri,
            marker: std::marker::PhantomData,
            diags: Vec::new(),
            uses: Vec::new(),
            scopes: Vec::new(),
            scope_stack: Vec::new(),
            scope_spans: Vec::new(),
            type_params: Vec::new(),
            self_ty: None,
            loop_depth: 0,
            fn_depth: 0,
            top_syms: HashMap::new(),
            methods: HashMap::new(),
            enums: HashMap::new(),
            aliases: HashMap::new(),
            owned_modules: HashMap::new(),
            merged_fns: HashMap::new(),
            merged_structs: HashMap::new(),
            merged_enums: HashMap::new(),
            index: FileIndex::default(),
        }
    }

    fn error(&mut self, span: Span, message: String, code: &'static str) {
        self.diags.push(Diag { span, severity: Severity::Error, message, code });
    }

    fn warning(&mut self, span: Span, message: String, code: &'static str) {
        self.diags.push(Diag { span, severity: Severity::Warning, message, code });
    }

    // ---- indexing ---------------------------------------------------------

    fn collect_imports(&mut self, program: &Program) -> Vec<ImportInfo> {
        let mut imports = Vec::new();
        for decl in &program.decls {
            if let Decl::Import(i) = decl {
                imports.push(ImportInfo {
                    path: i.path.clone(),
                    path_span: i.path_span,
                    alias: i.alias.clone(),
                    alias_span: i.alias_span,
                    span: i.span,
                });
            }
        }
        imports
    }

    fn register_alias(&mut self, alias: &str, span: Span) {
        self.aliases.insert(alias.to_string(), span);
    }

    fn attach_merged(&mut self, merged: &[ModuleRef]) {
        for m in merged {
            if let ModuleRef::Workspace { analysis, .. } = m {
                for sym in &analysis.index.symbols {
                    match sym.kind {
                        SymKind::Fn => {
                            self.merged_fns.insert(sym.name.clone(), sym.clone());
                        }
                        SymKind::Struct => {
                            self.merged_structs.insert(sym.name.clone(), sym.clone());
                        }
                        SymKind::Enum => {
                            self.merged_enums
                                .insert(sym.name.clone(), sym.variants.clone());
                        }
                        SymKind::Method => {
                            if let Some(owner) = &sym.owner {
                                self.methods
                                    .entry(owner.clone())
                                    .or_default()
                                    .push(sym.clone());
                            }
                        }
                        _ => {}
                    }
                }
            }
        }
    }

    fn index_program(&mut self, program: &Program) {
        for decl in &program.decls {
            self.index_decl(decl);
        }
    }

    fn register_name(&mut self, name: &str, name_span: Span, kind: SymKind) {
        if name.is_empty() {
            return;
        }
        if self.top_syms.contains_key(name) {
            self.error(
                name_span,
                format!(
                    "redeclaration of '{name}': a top-level {kind} with this name already exists",
                    kind = kind.label()
                ),
                "duplicate",
            );
        } else {
            self.top_syms.insert(name.to_string(), name_span);
        }
    }

    fn index_decl(&mut self, decl: &Decl) {
        match decl {
            Decl::Import(_) => {}
            Decl::Stmt(_) => {}
            Decl::Fn(f) => {
                self.register_name(&f.name, f.name_span, SymKind::Fn);
                self.index.symbols.push(Symbol {
                    name: f.name.clone(),
                    kind: SymKind::Fn,
                    owner: None,
                    name_span: f.name_span,
                    span: f.span,
                    is_pub: f.is_pub,
                    doc: f.doc.clone(),
                    params: f
                        .params
                        .iter()
                        .map(|p| ParamInfo {
                            name: p.name.clone(),
                            ty: p
                                .ty
                                .as_ref()
                                .map(crate::types::resolve_type_ann)
                                .unwrap_or(LType::Unknown),
                        })
                        .collect(),
                    ret: f.ret.as_ref().map(crate::types::resolve_type_ann),
                    ty: LType::Unknown,
                    type_params: f.type_params.iter().map(|t| t.name.clone()).collect(),
                    fields: vec![],
                    variants: vec![],
                    trait_methods: vec![],
                });
            }
            Decl::Let(l) => {
                self.register_name(&l.name, l.name_span, SymKind::Let);
                self.index.symbols.push(Symbol {
                    name: l.name.clone(),
                    kind: SymKind::Let,
                    owner: None,
                    name_span: l.name_span,
                    span: l.span,
                    is_pub: l.is_pub,
                    doc: l.doc.clone(),
                    params: vec![],
                    ret: None,
                    ty: LType::Unknown,
                    type_params: vec![],
                    fields: vec![],
                    variants: vec![],
                    trait_methods: vec![],
                });
            }
            Decl::Struct(s) => {
                self.register_name(&s.name, s.name_span, SymKind::Struct);
                self.index.symbols.push(Symbol {
                    name: s.name.clone(),
                    kind: SymKind::Struct,
                    owner: None,
                    name_span: s.name_span,
                    span: s.span,
                    is_pub: s.is_pub,
                    doc: s.doc.clone(),
                    params: vec![],
                    ret: None,
                    ty: LType::Unknown,
                    type_params: s.type_params.iter().map(|t| t.name.clone()).collect(),
                    fields: s
                        .fields
                        .iter()
                        .map(|f| FieldInfo {
                            name: f.name.clone(),
                            name_span: f.name_span,
                            ty: crate::types::bind_type_params(
                                &crate::types::resolve_type_ann(&f.ty),
                                &s.type_params.iter().map(|t| t.name.clone()).collect::<Vec<_>>(),
                            ),
                        })
                        .collect(),
                    variants: vec![],
                    trait_methods: vec![],
                });
            }
            Decl::Enum(e) => {
                self.register_name(&e.name, e.name_span, SymKind::Enum);
                let variants: Vec<(String, usize, Span)> = e
                    .variants
                    .iter()
                    .map(|v| (v.name.clone(), v.payloads.len(), v.name_span))
                    .collect();
                self.enums.insert(e.name.clone(), variants.clone());
                self.index.symbols.push(Symbol {
                    name: e.name.clone(),
                    kind: SymKind::Enum,
                    owner: None,
                    name_span: e.name_span,
                    span: e.span,
                    is_pub: e.is_pub,
                    doc: e.doc.clone(),
                    params: vec![],
                    ret: None,
                    ty: LType::Unknown,
                    type_params: e.type_params.iter().map(|t| t.name.clone()).collect(),
                    fields: vec![],
                    variants,
                    trait_methods: vec![],
                });
            }
            Decl::Impl(i) => {
                for m in &i.methods {
                    self.methods
                        .entry(i.type_name.clone())
                        .or_default()
                        .push(Symbol {
                            name: m.name.clone(),
                            kind: SymKind::Method,
                            owner: Some(i.type_name.clone()),
                            name_span: m.name_span,
                            span: m.span,
                            is_pub: true,
                            doc: m.doc.clone(),
                            params: m
                                .params
                                .iter()
                                .map(|p| ParamInfo {
                                    name: p.name.clone(),
                                    ty: p
                                        .ty
                                        .as_ref()
                                        .map(crate::types::resolve_type_ann)
                                        .unwrap_or(LType::Unknown),
                                })
                                .collect(),
                            ret: m.ret.as_ref().map(crate::types::resolve_type_ann),
                            ty: LType::Unknown,
                            type_params: m
                                .type_params
                                .iter()
                                .chain(i.type_params.iter())
                                .map(|t| t.name.clone())
                                .collect(),
                            fields: vec![],
                            variants: vec![],
                            trait_methods: vec![],
                        });
                    self.index.symbols.push(Symbol {
                        name: m.name.clone(),
                        kind: SymKind::Method,
                        owner: Some(i.type_name.clone()),
                        name_span: m.name_span,
                        span: m.span,
                        is_pub: true,
                        doc: m.doc.clone(),
                        params: m
                            .params
                            .iter()
                            .map(|p| ParamInfo {
                                name: p.name.clone(),
                                ty: p
                                    .ty
                                    .as_ref()
                                    .map(crate::types::resolve_type_ann)
                                    .unwrap_or(LType::Unknown),
                            })
                            .collect(),
                        ret: m.ret.as_ref().map(crate::types::resolve_type_ann),
                        ty: LType::Unknown,
                        type_params: m
                            .type_params
                            .iter()
                            .chain(i.type_params.iter())
                            .map(|t| t.name.clone())
                            .collect(),
                        fields: vec![],
                        variants: vec![],
                        trait_methods: vec![],
                    });
                }
            }
            Decl::Trait(t) => {
                self.register_name(&t.name, t.name_span, SymKind::Trait);
                self.index.symbols.push(Symbol {
                    name: t.name.clone(),
                    kind: SymKind::Trait,
                    owner: None,
                    name_span: t.name_span,
                    span: t.span,
                    is_pub: t.is_pub,
                    doc: t.doc.clone(),
                    params: vec![],
                    ret: None,
                    ty: LType::Unknown,
                    type_params: vec![],
                    fields: vec![],
                    variants: vec![],
                    trait_methods: t
                        .methods
                        .iter()
                        .map(|m| (m.name.clone(), m.params.len()))
                        .collect(),
                });
            }
        }
    }

    // ---- scopes -----------------------------------------------------------

    fn push_scope(&mut self, span: (usize, usize)) {
        self.scope_spans.push(span);
    }

    fn pop_scope(&mut self) {
        let (start, end) = self.scope_spans.pop().expect("scope stack underflow");
        let vars: Vec<(String, LType, Span)> = self
            .scope_stack
            .iter()
            .filter(|v| v.decl_span.start >= start && v.decl_span.start < end)
            .map(|v| (v.name.clone(), v.ty.clone(), v.decl_span))
            .collect();
        self.scopes.push(ScopeInfo { start, end, vars });
        let (start_b, end_b) = (start, end);
        self.scope_stack
            .retain(|v| !(v.decl_span.start >= start_b && v.decl_span.start < end_b));
    }

    fn declare(&mut self, name: &str, span: Span, ty: LType) {
        if name.is_empty() {
            return;
        }
        let scope_start = self.scope_spans.last().map(|s| s.0).unwrap_or(0);
        if let Some(_prev) = self
            .scope_stack
            .iter()
            .rev()
            .find(|v| v.name == name && v.decl_span.start >= scope_start)
        {
            self.error(
                span,
                format!("redeclaration of '{name}' in the same scope"),
                "scope",
            );
        }
        self.scope_stack.push(VarEntry {
            name: name.to_string(),
            ty,
            decl_span: span,
        });
    }

    fn lookup_var(&self, name: &str) -> Option<&VarEntry> {
        self.scope_stack.iter().rev().find(|v| v.name == name)
    }

    fn find_struct(&self, name: &str) -> Option<&Symbol> {
        if let Some(s) = self
            .index
            .find(name)
            .filter(|s| s.kind == SymKind::Struct)
        {
            return Some(s);
        }
        self.merged_structs.get(name)
    }

    fn find_method(&self, struct_name: &str, method: &str) -> Option<&Symbol> {
        // own file methods (index) + merged module methods (methods map)
        if let Some(m) = self
            .index
            .symbols
            .iter()
            .find(|s| s.kind == SymKind::Method && s.owner.as_deref() == Some(struct_name) && s.name == method)
        {
            return Some(m);
        }
        self.methods
            .get(struct_name)
            .and_then(|ms| ms.iter().find(|m| m.name == method))
    }

    // ---- program walk -----------------------------------------------------

    fn check_program(&mut self, program: &Program) {
        self.index_program(program);
        for decl in &program.decls {
            match decl {
                Decl::Let(l) => {
                    let ty = self.infer(&l.init);
                    if let Some(ann) = &l.ty {
                        self.check_annotation(ann);
                        let at = crate::types::resolve_type_ann(ann);
                        if at.is_known() && ty.is_known() && !self.types_compatible(&ty, &at) {
                            self.error(
                                l.init.span(),
                                format!(
                                    "type annotation '{}' does not match initializer type '{}'",
                                    at.name(),
                                    ty.name()
                                ),
                                "type",
                            );
                        }
                        let ty = at;
                        self.store_global_type(&l.name, ty);
                    } else {
                        self.store_global_type(&l.name, ty);
                    }
                    self.record_use(
                        l.name_span,
                        &l.name,
                        UseTarget::Symbol {
                            uri: Some(self.uri.clone()),
                            name: l.name.clone(),
                            name_span: l.name_span,
                        },
                        false,
                    );
                }
                Decl::Fn(f) => self.check_fn(f),
                Decl::Impl(i) => self.check_impl(i),
                Decl::Struct(s) => {
                    self.type_params
                        .extend(s.type_params.iter().map(|t| t.name.clone()));
                    for field in &s.fields {
                        self.check_annotation(&field.ty);
                    }
                    for _ in 0..s.type_params.len() {
                        self.type_params.pop();
                    }
                }
                Decl::Enum(e) => {
                    self.type_params
                        .extend(e.type_params.iter().map(|t| t.name.clone()));
                    for v in &e.variants {
                        for p in &v.payloads {
                            self.check_annotation(p);
                        }
                    }
                    for _ in 0..e.type_params.len() {
                        self.type_params.pop();
                    }
                }
                Decl::Stmt(s) => self.check_stmt(s),
                Decl::Trait(_) | Decl::Import(_) => {}
            }
        }
    }

    fn store_global_type(&mut self, name: &str, ty: LType) {
        for sym in self.index.symbols.iter_mut() {
            if sym.name == name && sym.kind == SymKind::Let {
                sym.ty = ty;
                return;
            }
        }
    }

    fn check_impl(&mut self, imp: &ImplDecl) {
        if let Some(trait_name) = &imp.trait_name {
            if self.find_struct(&imp.type_name).is_none() {
                self.error(
                    imp.type_name_span,
                    format!("impl for unknown struct '{}'", imp.type_name),
                    "impl",
                );
            }
            let trait_sym = self
                .index
                .find(trait_name)
                .filter(|s| s.kind == SymKind::Trait)
                .cloned();
            match trait_sym {
                None => self.error(
                    imp.trait_name_span.unwrap_or(imp.type_name_span),
                    format!("unknown trait '{trait_name}'"),
                    "impl",
                ),
                Some(t) => {
                    // completeness: every trait method present with same arity
                    let methods = self.index.methods_of(&imp.type_name);
                    let methods: Vec<Symbol> = methods.into_iter().cloned().collect();
                    for (req_name, req_arity) in &t.trait_methods {
                        match methods.iter().find(|m| m.name == *req_name) {
                            None => {
                                self.error(
                                    imp.type_name_span,
                                    format!(
                                        "impl of trait '{trait_name}' is missing required method '{req_name}'"
                                    ),
                                    "impl",
                                );
                            }
                            Some(m) => {
                                if m.params.len() != *req_arity {
                                    self.error(
                                        m.name_span,
                                        format!(
                                            "method '{req_name}' of trait '{trait_name}' expects {req_arity} argument(s), impl has {}",
                                            m.params.len()
                                        ),
                                        "impl",
                                    );
                                }
                            }
                        }
                    }
                }
            }
        }
        self.type_params
            .extend(imp.type_params.iter().map(|t| t.name.clone()));
        for m in &imp.methods {
            self.self_ty = Some(imp.type_name.clone());
            self.check_fn(m);
            self.self_ty = None;
        }
        for _ in 0..imp.type_params.len() {
            self.type_params.pop();
        }
    }

    fn check_fn(&mut self, f: &FnDecl) {
        self.type_params
            .extend(f.type_params.iter().map(|t| t.name.clone()));
        self.fn_depth += 1;
        // Scope starts at the fn header (not the body) so that parameters
        // (declared before the body) are captured in the fn scope's vars and
        // correctly removed from scope_stack when the fn scope pops.
        self.push_scope((f.span.start, f.body.span.end));
        for p in &f.params {
            if let Some(t) = &p.ty {
                self.check_annotation(t);
            }
            let ty = p
                .ty
                .as_ref()
                .map(crate::types::resolve_type_ann)
                .unwrap_or(LType::Unknown);
            self.declare(&p.name, p.name_span, ty);
        }
        if f.is_method {
            if let Some(owner) = &self.self_ty {
                self.declare(
                    "self",
                    Span::new(f.body.span.start, f.body.span.start),
                    LType::Named(owner.clone()),
                );
            }
        }
        self.check_block(&f.body);
        self.pop_scope();
        self.fn_depth -= 1;
        for _ in 0..f.type_params.len() {
            self.type_params.pop();
        }
    }

    fn check_block(&mut self, block: &Block) {
        self.push_scope((block.span.start, block.span.end));
        for stmt in &block.stmts {
            self.check_stmt(stmt);
        }
        self.pop_scope();
    }

    fn check_stmt(&mut self, stmt: &Stmt) {
        match stmt {
            Stmt::Let(l) => {
                let ty = self.infer(&l.init);
                if let Some(ann) = &l.ty {
                    self.check_annotation(ann);
                    let at = crate::types::resolve_type_ann(ann);
                    if at.is_known() && ty.is_known() && !self.types_compatible(&ty, &at) {
                        self.error(
                            l.init.span(),
                            format!(
                                "value of type '{}' does not match annotation '{}'",
                                ty.name(),
                                at.name()
                            ),
                            "type",
                        );
                    }
                }
                self.record_use(
                    l.name_span,
                    &l.name,
                    UseTarget::Symbol {
                        uri: Some(self.uri.clone()),
                        name: l.name.clone(),
                        name_span: l.name_span,
                    },
                    false,
                );
                self.declare(&l.name, l.name_span, ty);
            }
            Stmt::Return(value, span) => {
                if self.fn_depth == 0 {
                    self.error(*span, "return outside of a function".into(), "return");
                }
                if let Some(v) = value {
                    self.infer(v);
                }
            }
            Stmt::Break(span) => {
                if self.loop_depth == 0 {
                    self.error(*span, "break outside of a loop".into(), "loop");
                }
            }
            Stmt::Continue(span) => {
                if self.loop_depth == 0 {
                    self.error(*span, "continue outside of a loop".into(), "loop");
                }
            }
            Stmt::If(i) => {
                self.infer(&i.cond);
                self.check_block(&i.then_body);
                if let Some(e) = &i.else_body {
                    self.check_block(e);
                }
            }
            Stmt::While(w) => {
                self.infer(&w.cond);
                self.loop_depth += 1;
                self.check_block(&w.body);
                self.loop_depth -= 1;
            }
            Stmt::For(f) => {
                self.push_scope((f.span.start, f.body.span.end));
                if let Some(init) = &f.init {
                    self.check_stmt(init);
                }
                if let Some(cond) = &f.cond {
                    self.infer(cond);
                }
                if let Some(update) = &f.update {
                    self.check_stmt(update);
                }
                self.loop_depth += 1;
                self.check_block(&f.body);
                self.loop_depth -= 1;
                self.pop_scope();
            }
            Stmt::Match(m) => {
                self.infer_match(m);
            }
            Stmt::Assign(a) => self.check_assign(a),
            Stmt::Expr(e) => {
                self.infer(e);
            }
        }
    }

    fn check_assign(&mut self, a: &AssignStmt) {
        // Validate + type the target; record write-uses.
        let target_ty: LType;
        match &a.target {
            Expr::Ident { name, span } => {
                if let Some(v) = self.lookup_var(name).cloned() {
                    target_ty = v.ty.clone();
                    self.record_use(
                        *span,
                        name,
                        UseTarget::Symbol {
                            uri: Some(self.uri.clone()),
                            name: name.to_string(),
                            name_span: v.decl_span,
                        },
                        true,
                    );
                } else if self.top_syms.contains_key(name) {
                    target_ty = LType::Unknown;
                    if let Some(sym) = self.index.find(name).cloned() {
                        self.record_use(
                            *span,
                            name,
                            UseTarget::Symbol {
                                uri: Some(self.uri.clone()),
                                name: name.to_string(),
                                name_span: sym.name_span,
                            },
                            true,
                        );
                    }
                } else if self.aliases.contains_key(name) {
                    target_ty = LType::Unknown;
                    self.record_use(*span, name, UseTarget::ModuleAlias(name.clone()), true);
                } else {
                    target_ty = LType::Unknown;
                    self.record_use(*span, name, UseTarget::None, true);
                    self.error(
                        *span,
                        format!(
                            "assignment to undeclared variable '{name}' (declare it with 'let')"
                        ),
                        "resolve",
                    );
                }
            }
            Expr::Index { base, index, .. } => {
                let bt = self.infer(base);
                target_ty = match bt {
                    LType::Array(elem) => elem.as_ref().map(|e| (**e).clone()).unwrap_or(LType::Unknown),
                    LType::Unknown => LType::Unknown,
                    other => {
                        self.error(
                            base.span(),
                            format!("cannot index a value of type '{}'", other.name()),
                            "type",
                        );
                        LType::Unknown
                    }
                };
                self.infer(index);
            }
            Expr::Field { base, name, name_span, .. } => {
                let bt = self.infer(base);
                target_ty = match &bt {
                    LType::Named(sname) => {
                        if let Some(sym) = self.find_struct(sname).cloned() {
                            match sym.fields.iter().find(|f| f.name == *name) {
                                Some(f) => {
                                    self.record_use(
                                        *name_span,
                                        name,
                                        UseTarget::StructField {
                                            uri: Some(self.uri.clone()),
                                            struct_name: sname.clone(),
                                            field: name.clone(),
                                            name_span: f.name_span,
                                        },
                                        true,
                                    );
                                    f.ty.clone()
                                }
                                None => {
                                    self.error(
                                        *name_span,
                                        format!("struct '{sname}' has no field '{name}'"),
                                        "field",
                                    );
                                    LType::Unknown
                                }
                            }
                        } else {
                            LType::Unknown
                        }
                    }
                    _ => LType::Unknown,
                };
            }
            other => {
                self.error(
                    other.span(),
                    "invalid assignment target (expected a variable, index, or field)".into(),
                    "assign",
                );
                target_ty = LType::Unknown;
                self.infer(other);
            }
        }
        if !matches!(a.target, Expr::Ident { .. } | Expr::Index { .. } | Expr::Field { .. }) {
            return;
        }
        let value_ty = self.infer(&a.value);
        if target_ty.is_known() && value_ty.is_known() && !self.types_compatible(&value_ty, &target_ty) {
            self.error(
                a.value.span(),
                format!(
                    "cannot assign a value of type '{}' to a target of type '{}'",
                    value_ty.name(),
                    target_ty.name()
                ),
                "type",
            );
        }
    }

    // ---- type inference + use recording -----------------------------------

    fn infer(&mut self, expr: &Expr) -> LType {
        match expr {
            Expr::Int(_) => LType::Int,
            Expr::Float(_) => LType::Float,
            Expr::Str(_) => LType::String,
            Expr::Bool(_, _) => LType::Bool,
            Expr::Array(items, _) => {
                let mut elem_ty: Option<LType> = None;
                for it in items {
                    let t = self.infer(it);
                    match (&elem_ty, t) {
                        (None, t2) => elem_ty = Some(t2),
                        (Some(prev), t2) => {
                            if prev != &t2 && prev.is_known() && t2.is_known() {
                                if t2.compatible_with(prev) {
                                    // keep the wider type
                                } else if prev.compatible_with(&t2) {
                                    elem_ty = Some(t2);
                                } else {
                                    elem_ty = Some(LType::Unknown);
                                }
                            }
                        }
                    }
                }
                LType::Array(elem_ty.map(Box::new))
            }
            Expr::StructLit { name, name_span, fields, .. } => {
                if let Some(sym) = self.find_struct(name).cloned() {
                    let struct_tps = sym.type_params.clone();
                    for fv in fields {
                        if !sym.fields.iter().any(|f| f.name == fv.name) {
                            self.error(
                                fv.name_span,
                                format!("struct '{name}' has no field '{}'", fv.name),
                                "field",
                            );
                            self.record_use(
                                fv.name_span,
                                &fv.name,
                                UseTarget::None,
                                false,
                            );
                        } else if let Some(f) = sym.fields.iter().find(|f| f.name == fv.name) {
                            self.record_use(
                                fv.name_span,
                                &fv.name,
                                UseTarget::StructField {
                                    uri: Some(self.uri.clone()),
                                    struct_name: name.clone(),
                                    field: fv.name.clone(),
                                    name_span: f.name_span,
                                },
                                false,
                            );
                            let vt = self.infer(&fv.value);
                            let fty = self.relax_tp(&f.ty, &struct_tps);
                            if fty.is_known() && vt.is_known() && !self.types_compatible(&vt, &fty) {
                                self.error(
                                    fv.value.span(),
                                    format!(
                                        "field '{}' expects '{}', found '{}'",
                                        fv.name,
                                        fty.name(),
                                        vt.name()
                                    ),
                                    "type",
                                );
                            }
                        }
                    }
                    for f in &sym.fields {
                        if !fields.iter().any(|fv| fv.name == f.name) {
                            self.warning(
                                *name_span,
                                format!(
                                    "struct literal for '{name}' is missing field '{}'",
                                    f.name
                                ),
                                "field",
                            );
                        }
                    }
                    LType::Named(name.clone())
                } else if self.enums.contains_key(name) || self.merged_enums.contains_key(name) {
                    LType::Named(name.clone())
                } else if self.type_params.contains(name) {
                    LType::Unknown
                } else {
                    self.error(
                        *name_span,
                        format!("unknown struct '{name}' in struct literal"),
                        "type",
                    );
                    LType::Unknown
                }
            }
            Expr::Unary { op, expr, .. } => {
                let t = self.infer(expr);
                match op {
                    UnaryOp::Neg => {
                        if t.is_known() && !t.is_numeric() {
                            self.error(
                                expr.span(),
                                format!("cannot negate a value of type '{}'", t.name()),
                                "type",
                            );
                        }
                        t
                    }
                    UnaryOp::Not => t,
                }
            }
            Expr::Binary { op, lhs, rhs, .. } => {
                let lt = self.infer(lhs);
                let rt = self.infer(rhs);
                use BinOp::*;
                let known_pair = lt.is_known() && rt.is_known();
                // Generic (type-param) operands: concrete types unknowable
                // without instantiation tracking — never judge.
                let generic_involved =
                    matches!(lt, LType::TypeParam(_)) || matches!(rt, LType::TypeParam(_));
                if matches!(op, Add | Sub | Mul | Div | Mod) {
                    let valid = generic_involved
                        || match op {
                            // string + X coerces at runtime (see tests/valid/concat_mixed.lamo)
                            Add => (lt.is_numeric() && rt.is_numeric())
                                || (lt == LType::String && rt != LType::Void)
                                || (rt == LType::String && lt != LType::Void)
                                || !known_pair,
                            _ => (lt.is_numeric() && rt.is_numeric()) || !known_pair,
                        };
                    if !valid {
                        self.error(
                            lhs.span().merged(rhs.span()),
                            format!(
                                "invalid operands to '{}' ('{}' and '{}')",
                                op.text(),
                                lt.name(),
                                rt.name()
                            ),
                            "type",
                        );
                    }
                } else if matches!(op, Eq | Ne | Lt | Le | Gt | Ge) {
                    if known_pair && !lt.compatible_with(&rt) && !rt.compatible_with(&lt) {
                        self.error(
                            lhs.span().merged(rhs.span()),
                            format!("cannot compare '{}' with '{}'", lt.name(), rt.name()),
                            "type",
                        );
                    }
                }
                LType::binary_result(op, &lt, &rt)
            }
            Expr::Call { callee, args, .. } => self.infer_call(callee, args),
            Expr::Method { receiver, name, name_span, args, .. } => {
                // `alias.member(args)` module call?
                if let Expr::Ident { name: base, span: base_span } = &**receiver {
                    if self.aliases.contains_key(base) && !self.lookup_var(base).is_some() {
                        self.record_use(
                            *base_span,
                            base,
                            UseTarget::ModuleAlias(base.clone()),
                            false,
                        );
                        return self.infer_module_call(base, name, *name_span, args);
                    }
                }
                let recv_ty = self.infer(receiver);
                for a in args {
                    self.infer(a);
                }
                match &recv_ty {
                    LType::Array(_) => {
                        if let Some((mname, arity, _)) =
                            builtins::ARRAY_METHODS.iter().find(|(n, _, _)| *n == name)
                        {
                            self.record_use(*name_span, name, UseTarget::ArrayMethod(mname), false);
                            if args.len() != *arity {
                                self.error(
                                    *name_span,
                                    format!(
                                        "method '{name}' expects {arity} argument(s), found {}",
                                        args.len()
                                    ),
                                    "arity",
                                );
                            }
                            match name.as_str() {
                                "len" => LType::Int,
                                "push" => recv_ty.clone(),
                                _ => LType::Unknown,
                            }
                        } else {
                            self.error(
                                *name_span,
                                format!("unknown array method '{name}'"),
                                "method",
                            );
                            LType::Unknown
                        }
                    }
                    LType::Named(sname) => {
                        if self.find_struct(sname).is_some() {
                            if let Some(m) = self.find_method(sname, name).cloned() {
                                self.record_use(
                                    *name_span,
                                    name,
                                    UseTarget::StructMethod {
                                        uri: Some(self.uri.clone()),
                                        struct_name: sname.clone(),
                                        method: name.clone(),
                                        name_span: m.name_span,
                                    },
                                    false,
                                );
                                if args.len() != m.params.len() {
                                    self.error(
                                        *name_span,
                                        format!(
                                            "method '{}.{}' expects {} argument(s), found {}",
                                            sname,
                                            name,
                                            m.params.len(),
                                            args.len()
                                        ),
                                        "arity",
                                    );
                                }
                                for (p, a) in m.params.iter().zip(args.iter()) {
                                    let at = self.infer(a);
                                    let pty = self.relax_tp(&p.ty, &m.type_params);
                                    if pty.is_known() && at.is_known() && !self.types_compatible(&at, &pty) {
                                        self.error(
                                            a.span(),
                                            format!(
                                                "argument '{}' of '{}.{}' expects '{}', found '{}'",
                                                p.name,
                                                sname,
                                                name,
                                                pty.name(),
                                                at.name()
                                            ),
                                            "type",
                                        );
                                    }
                                }
                                m.ret.clone().unwrap_or(LType::Unknown)
                            } else {
                                self.error(
                                    *name_span,
                                    format!("struct '{sname}' has no method '{name}'"),
                                    "method",
                                );
                                LType::Unknown
                            }
                        } else {
                            LType::Unknown
                        }
                    }
                    _ => LType::Unknown,
                }
            }
            Expr::Index { base, index, .. } => {
                let bt = self.infer(base);
                let it = self.infer(index);
                if it.is_known() && !it.is_numeric() {
                    self.error(
                        index.span(),
                        format!("array index must be numeric, found '{}'", it.name()),
                        "type",
                    );
                }
                match bt {
                    LType::Array(elem) => elem
                        .as_ref()
                        .map(|e| (**e).clone())
                        .unwrap_or(LType::Unknown),
                    _ => LType::Unknown,
                }
            }
            Expr::Field { base, name, name_span, .. } => {
                // `alias.member` module member access?
                if let Expr::Ident { name: base, span: base_span } = &**base {
                    if self.aliases.contains_key(base) && !self.lookup_var(base).is_some() {
                        self.record_use(
                            *base_span,
                            base,
                            UseTarget::ModuleAlias(base.clone()),
                            false,
                        );
                        return self.infer_module_member(base, name, *name_span);
                    }
                }
                let bt = self.infer(base);
                match &bt {
                    LType::Named(sname) => {
                        if let Some(sym) = self.find_struct(sname).cloned() {
                            if let Some(f) = sym.fields.iter().find(|f| f.name == *name) {
                                self.record_use(
                                    *name_span,
                                    name,
                                    UseTarget::StructField {
                                        uri: Some(self.uri.clone()),
                                        struct_name: sname.clone(),
                                        field: name.clone(),
                                        name_span: f.name_span,
                                    },
                                    false,
                                );
                                f.ty.clone()
                            } else {
                                self.error(
                                    *name_span,
                                    format!("struct '{sname}' has no field '{name}'"),
                                    "field",
                                );
                                LType::Unknown
                            }
                        } else {
                            LType::Unknown
                        }
                    }
                    _ => LType::Unknown,
                }
            }
            Expr::QualifiedVariant { enum_name, variant, variant_span, .. } => {
                let variants = self
                    .enums
                    .get(enum_name)
                    .or_else(|| self.merged_enums.get(enum_name))
                    .cloned();
                match variants {
                    Some(vs) => match vs.iter().find(|(n, _, _)| n == variant) {
                        Some((_, arity, _)) => {
                            self.record_use(
                                *variant_span,
                                variant,
                                UseTarget::EnumVariant {
                                    uri: Some(self.uri.clone()),
                                    enum_name: enum_name.clone(),
                                    variant: variant.clone(),
                                    name_span: *variant_span,
                                },
                                false,
                            );
                            if *arity > 0 {
                                self.warning(
                                    *variant_span,
                                    format!(
                                        "variant '{enum_name}::{variant}' carries a payload and cannot be used as a value"
                                    ),
                                    "enum",
                                );
                            }
                            LType::Named(enum_name.clone())
                        }
                        None => {
                            self.error(
                                *variant_span,
                                format!("enum '{enum_name}' has no variant '{variant}'"),
                                "variant",
                            );
                            LType::Unknown
                        }
                    },
                    None => {
                        if !self.type_params.contains(enum_name)
                            && !self.enums.contains_key(enum_name)
                            && !self.merged_enums.contains_key(enum_name)
                        {
                            self.error(*variant_span, format!("unknown enum '{enum_name}'"), "type");
                        }
                        LType::Unknown
                    }
                }
            }
            Expr::Match(m) => self.infer_match(m),
            Expr::Ident { name, span } => self.infer_ident(name, *span),
            Expr::Error(_) => LType::Unknown,
        }
    }

    fn infer_module_call(&mut self, alias: &str, member: &str, member_span: Span, args: &[Expr]) -> LType {
        // Infer argument expressions so their uses/types are recorded
        // (e.g. `math.sqrt(total)` must record the use of `total`).
        for a in args {
            self.infer(a);
        }
        if let Some(mref) = self.module_ref(alias) {
            match mref.find_member(member) {
                Some(mi) => {
                    self.record_use(
                        member_span,
                        member,
                        UseTarget::StdMember {
                            module: alias.to_string(),
                            member: member.to_string(),
                        },
                        false,
                    );
                    if args.len() != mi.params.len() && mi.params.is_empty() {
                        // std members: arity from StdModule
                        if let Some(std_mod) = mref.std_module() {
                            if let Some(sm) = std_mod.member(member) {
                                if args.len() != sm.arity {
                                    self.error(
                                        member_span,
                                        format!(
                                            "'{alias}.{member}' expects {} argument(s), found {}",
                                            sm.arity,
                                            args.len()
                                        ),
                                        "arity",
                                    );
                                }
                            }
                        }
                    } else if args.len() != mi.params.len() {
                        self.error(
                            member_span,
                            format!(
                                "'{alias}.{member}' expects {} argument(s), found {}",
                                mi.params.len(),
                                args.len()
                            ),
                            "arity",
                        );
                    }
                    mi.ret.clone().unwrap_or(LType::Unknown)
                }
                None => {
                    self.error(
                        member_span,
                        format!("module '{alias}' has no member '{member}'"),
                        "module",
                    );
                    LType::Unknown
                }
            }
        } else {
            LType::Unknown
        }
    }

    fn infer_module_member(&mut self, alias: &str, member: &str, member_span: Span) -> LType {
        if let Some(mref) = self.module_ref(alias) {
            match mref.find_member(member) {
                Some(mi) => {
                    self.record_use(
                        member_span,
                        member,
                        UseTarget::StdMember {
                            module: alias.to_string(),
                            member: member.to_string(),
                        },
                        false,
                    );
                    if !mi.is_pub && mi.kind != SymKind::Struct && mi.kind != SymKind::Enum {
                        self.warning(
                            member_span,
                            format!(
                                "member '{member}' of module '{alias}' is not marked 'pub'; it will become private in a future release"
                            ),
                            "visibility",
                        );
                    }
                    mi.ty.clone()
                }
                None => {
                    self.error(
                        member_span,
                        format!("module '{alias}' has no member '{member}'"),
                        "module",
                    );
                    LType::Unknown
                }
            }
        } else {
            LType::Unknown
        }
    }

    fn module_ref(&self, alias: &str) -> Option<ModuleRef> {
        // We can't clone ModuleRef cheaply (Arc<FileAnalysis> is cloneable — OK).
        self.owned_modules.get(alias).cloned()
    }

    fn infer_call(&mut self, callee: &Expr, args: &[Expr]) -> LType {
        for a in args {
            self.infer(a);
        }
        match callee {
            Expr::Ident { name, span } => {
                if let Some(sym) = self.index.find(name).cloned() {
                    if sym.kind == SymKind::Fn {
                        self.record_use(
                            *span,
                            name,
                            UseTarget::Symbol {
                                uri: Some(self.uri.clone()),
                                name: name.clone(),
                                name_span: sym.name_span,
                            },
                            false,
                        );
                        self.check_args(name, &sym.params, args, *span, &sym.type_params);
                        return sym.ret.clone().unwrap_or(LType::Unknown);
                    }
                }
                if let Some(sym) = self.merged_fns.get(name).cloned() {
                    self.record_use(
                        *span,
                        name,
                        UseTarget::Symbol {
                            uri: None,
                            name: name.clone(),
                            name_span: sym.name_span,
                        },
                        false,
                    );
                    self.check_args(name, &sym.params, args, *span, &sym.type_params);
                    return sym.ret.clone().unwrap_or(LType::Unknown);
                }
                if let Some(b) = builtins::lookup_any(name) {
                    self.record_use(*span, name, UseTarget::Builtin(b.name), false);
                    if args.len() != b.arity {
                        self.error(
                            *span,
                            format!(
                                "builtin '{name}' expects {} argument(s), found {}",
                                b.arity,
                                args.len()
                            ),
                            "arity",
                        );
                    }
                    return match b.ret {
                        "string" => LType::String,
                        "bool" => LType::Bool,
                        "int" => LType::Int,
                        _ => LType::Unknown,
                    };
                }
                // bare variant constructor (later enum wins per SPEC §3.5)
                let all_variants = self
                    .enums
                    .iter()
                    .chain(self.merged_enums.iter())
                    .flat_map(|(e, vs)| vs.iter().map(move |(n, a, s)| (e.clone(), n.clone(), *a, *s)))
                    .collect::<Vec<_>>();
                if let Some((enum_name, _, arity, vspan)) =
                    all_variants.iter().rev().find(|(_e, n, _, _)| n == name)
                {
                    self.record_use(
                        *span,
                        name,
                        UseTarget::EnumVariant {
                            uri: Some(self.uri.clone()),
                            enum_name: enum_name.clone(),
                            variant: name.clone(),
                            name_span: *vspan,
                        },
                        false,
                    );
                    if args.len() != *arity {
                        self.error(
                            *span,
                            format!(
                                "variant '{name}' expects {arity} payload argument(s), found {}",
                                args.len()
                            ),
                            "arity",
                        );
                    }
                    return LType::Named(enum_name.clone());
                }
                self.error(
                    *span,
                    format!("use of undeclared function '{name}'"),
                    "resolve",
                );
                LType::Unknown
            }
            Expr::QualifiedVariant { enum_name, variant, variant_span, .. } => {
                let variants = self
                    .enums
                    .get(enum_name)
                    .or_else(|| self.merged_enums.get(enum_name))
                    .cloned();
                if let Some(vs) = variants {
                    if let Some((_, arity, _)) = vs.iter().find(|(n, _, _)| n == variant) {
                        self.record_use(
                            *variant_span,
                            variant,
                            UseTarget::EnumVariant {
                                uri: Some(self.uri.clone()),
                                enum_name: enum_name.clone(),
                                variant: variant.clone(),
                                name_span: *variant_span,
                            },
                            false,
                        );
                        if args.len() != *arity {
                            self.error(
                                *variant_span,
                                format!(
                                    "variant '{enum_name}::{variant}' expects {arity} payload argument(s), found {}",
                                    args.len()
                                ),
                                "arity",
                            );
                        }
                        return LType::Named(enum_name.clone());
                    }
                    self.error(
                        *variant_span,
                        format!("enum '{enum_name}' has no variant '{variant}'"),
                        "variant",
                    );
                }
                LType::Unknown
            }
            other => {
                let _ = self.infer(other);
                LType::Unknown
            }
        }
    }

    fn check_args(
        &mut self,
        callee: &str,
        params: &[ParamInfo],
        args: &[Expr],
        span: Span,
        callee_type_params: &[String],
    ) {
        if params.len() != args.len() {
            self.error(
                span,
                format!(
                    "function '{callee}' expects {} argument(s), found {}",
                    params.len(),
                    args.len()
                ),
                "arity",
            );
            return;
        }
        for (p, a) in params.iter().zip(args.iter()) {
            let at = self.infer(a);
            let pty = self.relax_tp(&p.ty, callee_type_params);
            if pty.is_known() && at.is_known() && !self.types_compatible(&at, &pty) {
                self.error(
                    a.span(),
                    format!(
                        "argument '{}' of '{callee}' expects '{}', found '{}'",
                        p.name,
                        pty.name(),
                        at.name()
                    ),
                    "type",
                );
            }
        }
    }

    /// Replace callee-side type parameter names with Unknown so they bind
    /// to any concrete argument.
    fn relax_tp(&self, ty: &LType, tps: &[String]) -> LType {
        match ty {
            LType::Named(n) if tps.contains(n) => LType::Unknown,
            LType::Array(Some(e)) => LType::Array(Some(Box::new(self.relax_tp(e, tps)))),
            other => other.clone(),
        }
    }

    fn infer_ident(&mut self, name: &str, span: Span) -> LType {
        // 1. local scope
        if let Some(v) = self.lookup_var(name).cloned() {
            self.record_use(
                span,
                name,
                UseTarget::Symbol {
                    uri: Some(self.uri.clone()),
                    name: name.to_string(),
                    name_span: v.decl_span,
                },
                false,
            );
            return v.ty.clone();
        }
        // 2. top-level symbol
        if let Some(sym) = self.index.find(name).cloned() {
            match sym.kind {
                SymKind::Fn => {
                    // function referenced as a value (e.g. http_route("/x", handler))
                    self.record_use(
                        span,
                        name,
                        UseTarget::Symbol {
                            uri: Some(self.uri.clone()),
                            name: name.to_string(),
                            name_span: sym.name_span,
                        },
                        false,
                    );
                    return LType::Unknown;
                }
                _ => {
                    self.record_use(
                        span,
                        name,
                        UseTarget::Symbol {
                            uri: Some(self.uri.clone()),
                            name: name.to_string(),
                            name_span: sym.name_span,
                        },
                        false,
                    );
                    return sym.ty.clone();
                }
            }
        }
        // 3. merged globals
        if let Some(sym) = self.merged_fns.get(name) {
            self.record_use(
                span,
                name,
                UseTarget::Symbol {
                    uri: None,
                    name: name.to_string(),
                    name_span: sym.name_span,
                },
                false,
            );
            return LType::Unknown;
        }
        // 4. builtins
        if let Some(b) = builtins::lookup(name) {
            self.record_use(span, name, UseTarget::Builtin(b.name), false);
            return LType::Unknown;
        }
        // 5. bare enum variant (file-local + merged; later wins per SPEC §3.5)
        let mut found: Option<(String, usize, Span)> = None;
        let enum_names: Vec<String> = self
            .enums
            .keys()
            .chain(self.merged_enums.keys())
            .cloned()
            .collect();
        for e in &enum_names {
            let vs = self
                .enums
                .get(e)
                .or_else(|| self.merged_enums.get(e))
                .cloned()
                .unwrap_or_default();
            if let Some((n, a, s)) = vs.iter().find(|(n, _, _)| n == name) {
                found = Some((n.clone(), *a, *s));
            }
        }
        if let Some((_, arity, vspan)) = found {
            let enum_name = enum_names
                .iter()
                .find(|e| {
                    self.enums
                        .get(*e)
                        .or_else(|| self.merged_enums.get(*e))
                        .map(|vs| vs.iter().any(|(n, _, _)| n == name))
                        .unwrap_or(false)
                })
                .cloned();
            self.record_use(
                span,
                name,
                UseTarget::EnumVariant {
                    uri: Some(self.uri.clone()),
                    enum_name: enum_name.unwrap_or_default(),
                    variant: name.to_string(),
                    name_span: vspan,
                },
                false,
            );
            if arity > 0 {
                self.warning(
                    span,
                    format!(
                        "variant '{name}' carries a payload and cannot be used as a value"
                    ),
                    "enum",
                );
            }
            return LType::Unknown;
        }
        // 6. module alias used as a bare value — record, stay silent
        // (aliases are not values, but flagging every occurrence risks noise)
        if self.aliases.contains_key(name) {
            self.record_use(span, name, UseTarget::ModuleAlias(name.to_string()), false);
            return LType::Unknown;
        }
        self.error(span, format!("use of undeclared variable '{name}'"), "resolve");
        LType::Unknown
    }

    fn infer_match(&mut self, m: &MatchExpr) -> LType {
        let _scrutinee = self.infer(&m.scrutinee);
        let mut result: Option<LType> = None;
        for arm in &m.arms {
            let arm_start = arm.pattern.span().start;
            let arm_end = match &arm.body {
                ArmBody::Expr(e) => e.span().end,
                ArmBody::Block(b) => b.span.end,
                ArmBody::Stmt(s) => sp_end(s),
            };
            self.push_scope((arm_start, arm_end));
            self.check_pattern_bindings(&arm.pattern);
            if let Some(g) = &arm.guard {
                let gt = self.infer(g);
                if gt == LType::Void {
                    self.error(g.span(), "void expression used in a 'when' guard".into(), "type");
                }
            }
            let body_ty = match &arm.body {
                ArmBody::Expr(e) => self.infer(e),
                ArmBody::Block(b) => {
                    for s in &b.stmts {
                        self.check_stmt(s);
                    }
                    LType::Unknown
                }
                ArmBody::Stmt(s) => match s {
                    Stmt::Return(Some(v), _) => {
                        let _t = self.infer(v);
                        LType::Unknown
                    }
                    Stmt::Return(None, _) | Stmt::Break(_) | Stmt::Continue(_) => LType::Unknown,
                    other => {
                        self.check_stmt(other);
                        LType::Unknown
                    }
                },
            };
            self.pop_scope();
            match (&result, body_ty) {
                (None, t) => result = Some(t),
                (Some(prev), t) => {
                    if prev.is_known() && t.is_known() && prev != &t {
                        if prev.is_numeric() && t.is_numeric() {
                            result = Some(LType::Float);
                        } else if prev.compatible_with(&t) {
                            // keep prev
                        } else if t.compatible_with(prev) {
                            result = Some(t);
                        } else {
                            result = Some(LType::Unknown);
                        }
                    }
                }
            }
        }
        result.unwrap_or(LType::Unknown)
    }

    fn check_pattern_bindings(&mut self, pattern: &Pattern) {
        match pattern {
            Pattern::Binding(name, span) => {
                self.declare(name, *span, LType::Unknown);
            }
            Pattern::Variant { payloads, .. } => {
                for p in payloads {
                    self.check_pattern_bindings(p);
                }
            }
            _ => {}
        }
    }

    fn check_annotation(&mut self, ann: &TypeAnn) {
        match &ann.kind {
            TypeKind::Array(Some(elem)) => self.check_annotation(elem),
            TypeKind::Array(None) => {
                self.warning(
                    ann.span,
                    "bare 'array' is deprecated — use array<T> with an explicit element type"
                        .into(),
                    "deprecated",
                );
            }
            TypeKind::Named(name, args) => {
                for a in args {
                    self.check_annotation(a);
                }
                if name == "<error>" {
                    return;
                }
                if crate::types::BUILTIN_TYPE_NAMES.contains(&name.as_str()) {
                    return;
                }
                if self.type_params.contains(name) {
                    return;
                }
                if self.find_struct(name).is_some()
                    || self.enums.contains_key(name)
                    || self.merged_enums.contains_key(name)
                    || self.merged_structs.contains_key(name)
                {
                    return;
                }
                self.error(ann.span, format!("unknown type '{name}'"), "type");
            }
            _ => {}
        }
    }

    /// Type-param-aware compatibility: `Named("T")` where T is an in-scope
    /// type parameter binds to any concrete type.
    fn types_compatible(&self, a: &LType, b: &LType) -> bool {
        let relax = |t: &LType| match t {
            LType::Named(n) if self.type_params.contains(n) => LType::Unknown,
            other => other.clone(),
        };
        let a = relax(a);
        let b = relax(b);
        a.compatible_with(&b)
    }

    fn record_use(&mut self, span: Span, name: &str, target: UseTarget, is_write: bool) {
        self.uses.push(Use { span, name: name.to_string(), target, is_write });
    }
}

// Additional field holder — `owned_modules` is defined as part of Checker
// below via a secondary impl block (Rust requires all fields on the struct).

fn sp_end(s: &Stmt) -> usize {
    match s {
        Stmt::Return(_, sp) => sp.end,
        Stmt::Break(sp) | Stmt::Continue(sp) => sp.end,
        Stmt::Let(l) => l.span.end,
        Stmt::If(i) => i.span.end,
        Stmt::While(w) => w.span.end,
        Stmt::For(f) => f.span.end,
        Stmt::Match(m) => m.span.end,
        Stmt::Assign(a) => a.span.end,
        Stmt::Expr(e) => e.span().end,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tower_lsp::lsp_types::Url;

    fn analyze_src(src: &str) -> Analysis {
        let uri = Url::parse("file:///test/main.lamo").unwrap();
        analyze(&uri, src)
    }

    fn errors(a: &Analysis) -> Vec<String> {
        a.file
            .diags
            .iter()
            .filter(|d| d.severity == Severity::Error)
            .map(|d| d.message.clone())
            .collect()
    }

    #[test]
    fn clean_program_has_no_diagnostics() {
        let a = analyze_src("fn main() {\n    let x = 5\n    print(x)\n}\n");
        assert_eq!(errors(&a), Vec::<String>::new());
    }

    #[test]
    fn undeclared_variable() {
        let a = analyze_src("fn main() {\n    print(undefined_var)\n}\n");
        let errs = errors(&a);
        assert!(
            errs.iter().any(|m| m.contains("undeclared variable 'undefined_var'")),
            "{errs:?}"
        );
    }

    #[test]
    fn assignment_without_let() {
        let a = analyze_src("fn main() {\n    x = 5\n}\n");
        assert!(
            errors(&a)
                .iter()
                .any(|m| m.contains("assignment to undeclared variable 'x'"))
        );
    }

    #[test]
    fn redeclaration_same_scope() {
        let a = analyze_src("fn main() {\n    let x = 1\n    let x = 2\n}\n");
        assert!(errors(&a).iter().any(|m| m.contains("redeclaration of 'x'")));
    }

    #[test]
    fn shadowing_across_scopes_ok() {
        let a = analyze_src(
            "fn main() {\n    let x = 1\n    if (1 < 2) {\n        let x = 2\n        print(x)\n    }\n}\n",
        );
        assert_eq!(errors(&a), Vec::<String>::new());
    }

    #[test]
    fn break_outside_loop() {
        let a = analyze_src("fn main() {\n    break\n}\n");
        assert!(errors(&a).iter().any(|m| m.contains("break outside")));
    }

    #[test]
    fn arity_mismatch() {
        let a = analyze_src("fn add(a, b) { return a + b }\nfn main() {\n    print(add(1))\n}\n");
        assert!(errors(&a).iter().any(|m| m.contains("expects 2 argument(s)")));
    }

    #[test]
    fn unknown_type_annotation() {
        let a = analyze_src("fn f(x: Flurb) { }\n");
        assert!(errors(&a).iter().any(|m| m.contains("unknown type 'Flurb'")));
    }

    #[test]
    fn unknown_struct_field() {
        let a = analyze_src(
            "struct P { x: int }\nfn main() {\n    let p = P { x: 1, y: 2 }\n}\n",
        );
        assert!(errors(&a).iter().any(|m| m.contains("no field 'y'")));
    }

    #[test]
    fn unknown_method() {
        let a = analyze_src(
            "struct P { x: int }\nfn main() {\n    let p = P { x: 1 }\n    p.nonexistent()\n}\n",
        );
        assert!(errors(&a).iter().any(|m| m.contains("no method 'nonexistent'")));
    }

    #[test]
    fn string_plus_int_coerces() {
        // The real runtime coerces (see tests/valid/concat_mixed.lamo upstream).
        let a = analyze_src("fn main() {\n    print(\"a\" + 5)\n}\n");
        assert_eq!(errors(&a), Vec::<String>::new());
    }

    #[test]
    fn string_minus_int_is_error() {
        let a = analyze_src("fn main() {\n    print(\"a\" - 5)\n}\n");
        assert!(errors(&a).iter().any(|m| m.contains("invalid operands")));
    }

    #[test]
    fn array_methods_work() {
        let a = analyze_src(
            "fn main() {\n    let arr = [1, 2, 3]\n    arr.push(4)\n    print(arr.len())\n    print(arr.pop())\n}\n",
        );
        assert_eq!(errors(&a), Vec::<String>::new());
    }

    #[test]
    fn type_mismatch_annotation() {
        let a = analyze_src("fn main() {\n    let x: int = \"hello\"\n}\n");
        assert!(errors(&a).iter().any(|m| m.contains("does not match")));
    }

    #[test]
    fn globals_visible_in_fns() {
        let a = analyze_src("let counter = 100\nfn get() {\n    return counter\n}\n");
        assert_eq!(errors(&a), Vec::<String>::new());
    }

    #[test]
    fn enums_and_variants() {
        let src = r#"
enum Option<T> {
    Some(T),
    None
}

fn main() {
    let o = Some(42)
    match o {
        Some(x) => print(x),
        None => print(0)
    }
    let q = Option::Some(5)
    print(q)
}
"#;
        let a = analyze_src(src);
        assert_eq!(errors(&a), Vec::<String>::new(), "{:?}", errors(&a));
    }

    #[test]
    fn unknown_variant() {
        let a = analyze_src("enum Color { Red, Green }\nfn main() {\n    print(Blue)\n}\n");
        assert!(errors(&a).iter().any(|m| m.contains("undeclared")));
    }

    #[test]
    fn traits_ok() {
        let src = r#"
trait Shape {
    fn area() -> float
    fn name() -> string
}

struct Circle { r: float }

impl Shape for Circle {
    fn area() -> float { return 3.14 * self.r * self.r }
    fn name() -> string { return "circle" }
}

fn main() {
    let c = Circle { r: 2.0 }
    print(c.area())
}
"#;
        let a = analyze_src(src);
        assert_eq!(errors(&a), Vec::<String>::new(), "{:?}", errors(&a));
    }

    #[test]
    fn trait_missing_method() {
        let src = r#"
trait Shape {
    fn area() -> float
    fn name() -> string
}

struct Circle { r: float }

impl Shape for Circle {
    fn area() -> float { return 1.0 }
}
"#;
        let a = analyze_src(src);
        assert!(
            errors(&a)
                .iter()
                .any(|m| m.contains("missing required method 'name'"))
        );
    }

    #[test]
    fn uses_recorded_for_goto_def() {
        let src = "fn helper() { return 1 }\nfn main() {\n    print(helper())\n}\n";
        let a = analyze_src(src);
        let call_use = a
            .file
            .uses
            .iter()
            .find(|u| u.name == "helper" && u.target != UseTarget::None);
        assert!(call_use.is_some());
    }
}
