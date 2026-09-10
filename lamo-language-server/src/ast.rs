//! AST for the Lamo language, with byte spans on every node.
//!
//! Mirrors SPEC §2 (informal grammar) plus the 2.6–2.9 amendments:
//! tagged enums, `Enum::Variant` qualification, `when` guards, literal
//! patterns, match-as-expression, traits, and generic type parameters.

use crate::lexer::Span;

#[derive(Debug, Clone, PartialEq)]
pub struct TypeAnn {
    pub span: Span,
    pub kind: TypeKind,
}

#[derive(Debug, Clone, PartialEq)]
pub enum TypeKind {
    Int,
    Float,
    Bool,
    String,
    Void,
    /// `array<T>` — `None` element means the deprecated bare `array` form.
    Array(Option<Box<TypeAnn>>),
    /// User struct, enum, or in-scope type parameter.
    Named(String, Vec<TypeAnn>),
}

#[derive(Debug, Clone)]
pub struct Param {
    pub name: String,
    pub name_span: Span,
    pub ty: Option<TypeAnn>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct TypeParam {
    pub name: String,
    pub name_span: Span,
    /// Constraint: catalogue name (`Num`, `Ord`, ...) or user trait.
    pub constraint: Option<String>,
    pub constraint_span: Option<Span>,
}

#[derive(Debug, Clone)]
pub struct Field {
    pub name: String,
    pub name_span: Span,
    pub ty: TypeAnn,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct Variant {
    pub name: String,
    pub name_span: Span,
    /// Payload types (empty for unit variants).
    pub payloads: Vec<TypeAnn>,
    pub payload_paren_span: Option<Span>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum Decl {
    Import(ImportDecl),
    Fn(FnDecl),
    Let(LetDecl),
    Struct(StructDecl),
    Enum(EnumDecl),
    Impl(ImplDecl),
    Trait(TraitDecl),
    /// Top-level statements are valid Lamo (the official parser calls
    /// parse_statement at program level — examples like `print("hi")`
    /// outside any fn are real, working code).
    Stmt(Stmt),
}

#[derive(Debug, Clone)]
pub struct ImportDecl {
    /// Path as written: `"math.lamo"`, `math`, `std.io`.
    pub path: String,
    pub path_span: Span,
    pub alias: Option<String>,
    pub alias_span: Option<Span>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct FnDecl {
    pub name: String,
    pub name_span: Span,
    pub type_params: Vec<TypeParam>,
    pub params: Vec<Param>,
    pub ret: Option<TypeAnn>,
    pub body: Block,
    pub is_pub: bool,
    pub is_method: bool,
    /// Doc comment lines (without `//`), if any directly precede the decl.
    pub doc: Vec<String>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct LetDecl {
    pub name: String,
    pub name_span: Span,
    pub ty: Option<TypeAnn>,
    pub init: Expr,
    pub is_pub: bool,
    pub doc: Vec<String>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct StructDecl {
    pub name: String,
    pub name_span: Span,
    pub type_params: Vec<TypeParam>,
    pub fields: Vec<Field>,
    pub is_pub: bool,
    pub doc: Vec<String>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct EnumDecl {
    pub name: String,
    pub name_span: Span,
    pub type_params: Vec<TypeParam>,
    pub variants: Vec<Variant>,
    pub is_pub: bool,
    pub doc: Vec<String>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct ImplDecl {
    /// Trait name for `impl Trait for Type` (2.9.0).
    pub trait_name: Option<String>,
    pub trait_name_span: Option<Span>,
    pub type_name: String,
    pub type_name_span: Span,
    pub type_params: Vec<TypeParam>,
    pub methods: Vec<FnDecl>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct TraitDecl {
    pub name: String,
    pub name_span: Span,
    /// Required method signatures.
    pub methods: Vec<TraitFn>,
    pub is_pub: bool,
    pub doc: Vec<String>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct TraitFn {
    pub name: String,
    pub name_span: Span,
    pub params: Vec<Param>,
    pub ret: Option<TypeAnn>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct Block {
    pub stmts: Vec<Stmt>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum Stmt {
    Let(LetStmt),
    Return(Option<Expr>, Span),
    Break(Span),
    Continue(Span),
    If(IfStmt),
    While(WhileStmt),
    For(ForStmt),
    Match(MatchExpr),
    /// Assignment `lvalue = expr`, `+=`, etc.
    Assign(AssignStmt),
    Expr(Expr),
}

#[derive(Debug, Clone)]
pub struct LetStmt {
    pub name: String,
    pub name_span: Span,
    pub ty: Option<TypeAnn>,
    pub init: Expr,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct IfStmt {
    pub cond: Expr,
    pub then_body: Block,
    /// `else if` chains are nested Ifs (SPEC §4.1).
    pub else_body: Option<Block>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct WhileStmt {
    pub cond: Expr,
    pub body: Block,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct ForStmt {
    /// `let i = 0` or an assignment; both optional (C-style, SPEC §4.3).
    pub init: Option<Box<Stmt>>,
    pub cond: Option<Expr>,
    pub update: Option<Box<Stmt>>,
    pub body: Block,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct AssignStmt {
    pub target: Expr,
    pub op: AssignOp,
    pub value: Expr,
    pub span: Span,
}

impl AssignStmt {
    /// True when the assignment operator reads the old value (`+=`, `++`, ...).
    pub fn reads_target(&self) -> bool {
        !matches!(self.op, AssignOp::Eq)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AssignOp {
    Eq,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    PercentEq,
    PlusPlus,
    MinusMinus,
}

#[derive(Debug, Clone)]
pub struct MatchExpr {
    pub scrutinee: Box<Expr>,
    pub arms: Vec<MatchArm>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct MatchArm {
    pub pattern: Pattern,
    pub guard: Option<Expr>,
    pub body: ArmBody,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum ArmBody {
    Expr(Expr),
    Block(Block),
    /// `Some(x) => return x` — return/break/continue in an arm body.
    Stmt(Stmt),
}

#[derive(Debug, Clone)]
pub enum Pattern {
    Wildcard(Span),
    Int(String, Span),
    Float(String, Span),
    Str(Span),
    Bool(bool, Span),
    /// Variant constructor pattern with payload patterns, possibly nested.
    /// `kind` is the (possibly qualified) variant name; `None` qualifier
    /// means bare.
    Variant {
        enum_name: Option<String>,
        name: String,
        payloads: Vec<Pattern>,
        span: Span,
        name_span: Span,
    },
    /// A bare identifier inside a payload list = binding (SPEC §4.6).
    Binding(String, Span),
}

impl Pattern {
    pub fn span(&self) -> Span {
        match self {
            Pattern::Wildcard(s)
            | Pattern::Int(_, s)
            | Pattern::Float(_, s)
            | Pattern::Str(s)
            | Pattern::Bool(_, s)
            | Pattern::Binding(_, s)
            | Pattern::Variant { span: s, .. } => *s,
        }
    }
}

#[derive(Debug, Clone)]
pub enum Expr {
    Int(Span),
    Float(Span),
    Str(Span),
    Bool(bool, Span),
    Ident {
        name: String,
        span: Span,
    },
    Array(Vec<Expr>, Span),
    /// `Name { field: value, ... }` or `Name<T> { ... }`
    StructLit {
        name: String,
        name_span: Span,
        type_args: Vec<TypeAnn>,
        fields: Vec<FieldValue>,
        span: Span,
    },
    Unary {
        op: UnaryOp,
        expr: Box<Expr>,
        span: Span,
    },
    Binary {
        op: BinOp,
        lhs: Box<Expr>,
        rhs: Box<Expr>,
        span: Span,
    },
    Call {
        callee: Box<Expr>,
        args: Vec<Expr>,
        /// Type args written at the call site: `pick<int>(1, 2)`.
        type_args: Vec<TypeAnn>,
        span: Span,
    },
    Method {
        receiver: Box<Expr>,
        name: String,
        name_span: Span,
        args: Vec<Expr>,
        type_args: Vec<TypeAnn>,
        span: Span,
    },
    Index {
        base: Box<Expr>,
        index: Box<Expr>,
        span: Span,
    },
    Field {
        base: Box<Expr>,
        name: String,
        name_span: Span,
        span: Span,
    },
    /// `Enum::Variant` qualification in expression position.
    QualifiedVariant {
        enum_name: String,
        enum_span: Span,
        variant: String,
        variant_span: Span,
        span: Span,
    },
    Match(MatchExpr),
    /// Malformed expression produced during error recovery.
    Error(Span),
}

#[derive(Debug, Clone)]
pub struct FieldValue {
    pub name: String,
    pub name_span: Span,
    pub value: Expr,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnaryOp {
    Neg,
    Not,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
}

impl BinOp {
    pub fn text(&self) -> &'static str {
        match self {
            BinOp::Add => "+",
            BinOp::Sub => "-",
            BinOp::Mul => "*",
            BinOp::Div => "/",
            BinOp::Mod => "%",
            BinOp::Eq => "==",
            BinOp::Ne => "!=",
            BinOp::Lt => "<",
            BinOp::Le => "<=",
            BinOp::Gt => ">",
            BinOp::Ge => ">=",
            BinOp::And => "&&",
            BinOp::Or => "||",
        }
    }
}

impl Expr {
    pub fn span(&self) -> Span {
        match self {
            Expr::Int(s) | Expr::Float(s) | Expr::Str(s) | Expr::Error(s) => *s,
            Expr::Bool(_, s) => *s,
            Expr::Ident { span, .. } => *span,
            Expr::Array(_, s) => *s,
            Expr::StructLit { span, .. } => *span,
            Expr::Unary { span, .. } => *span,
            Expr::Binary { span, .. } => *span,
            Expr::Call { span, .. } => *span,
            Expr::Method { span, .. } => *span,
            Expr::Index { span, .. } => *span,
            Expr::Field { span, .. } => *span,
            Expr::QualifiedVariant { span, .. } => *span,
            Expr::Match(m) => m.span,
        }
    }
}

#[derive(Debug, Clone)]
pub struct Program {
    pub decls: Vec<Decl>,
    pub span: Span,
}
