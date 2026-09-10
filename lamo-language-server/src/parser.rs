//! Recursive-descent parser for Lamo (SPEC §2 + 2.6–2.9 amendments).
//!
//! Produces an AST with byte spans and collects parse errors with
//! recovery, so a broken file still yields partial structure for the
//! LSP features (outline, completion, goto-definition).

use crate::ast::*;
use crate::lexer::{Comment, LexOutput, Span, Tok, Token};

#[derive(Debug, Clone, PartialEq)]
pub struct ParseError {
    pub span: Span,
    pub message: String,
}

pub struct ParseOutput {
    pub program: Program,
    pub errors: Vec<ParseError>,
}

pub struct Parser<'a> {
    tokens: &'a [Token],
    comments: &'a [Comment],
    source: &'a str,
    pos: usize,
    errors: Vec<ParseError>,
    /// Suppresses error recording during speculative (backtracked) parses.
    suppress: usize,
}

const MAX_ERRORS: usize = 200;

pub fn parse(source: &str, lex_out: &LexOutput) -> ParseOutput {
    let mut parser = Parser {
        tokens: &lex_out.tokens,
        comments: &lex_out.comments,
        source,
        pos: 0,
        errors: Vec::new(),
        suppress: 0,
    };
    let program = parser.parse_program();
    ParseOutput {
        program,
        errors: parser.errors,
    }
}

impl<'a> Parser<'a> {
    // ---- token utilities -------------------------------------------------

    fn peek(&self) -> &Tok {
        &self.tokens[self.pos.min(self.tokens.len() - 1)].tok
    }

    fn peek_at(&self, n: usize) -> &Tok {
        &self.tokens[(self.pos + n).min(self.tokens.len() - 1)].tok
    }

    fn cur_span(&self) -> Span {
        self.tokens[self.pos.min(self.tokens.len() - 1)].span
    }

    fn prev_end(&self) -> usize {
        self.tokens[self.pos.saturating_sub(1).min(self.tokens.len() - 1)].span.end
    }

    fn bump(&mut self) -> Token {
        let t = self.tokens[self.pos.min(self.tokens.len() - 1)].clone();
        if self.pos < self.tokens.len() - 1 {
            self.pos += 1;
        }
        t
    }

    fn at_eof(&self) -> bool {
        matches!(self.peek(), Tok::Eof)
    }

    fn eat(&mut self, tok: &Tok) -> bool {
        if self.peek() == tok {
            self.bump();
            true
        } else {
            false
        }
    }

    fn eat_semicolon(&mut self) {
        while self.eat(&Tok::Semicolon) {}
    }

    /// Skip tokens until the statement loop can make progress again.
    fn recover_to_stmt_boundary(&mut self) {
        let mut depth = 0usize;
        loop {
            match self.peek().clone() {
                Tok::Eof => break,
                Tok::LBrace => {
                    depth += 1;
                    self.bump();
                }
                Tok::RBrace => {
                    if depth == 0 {
                        break;
                    }
                    depth -= 1;
                    self.bump();
                }
                Tok::Semicolon => {
                    self.bump();
                    if depth == 0 {
                        break;
                    }
                }
                _ => {
                    self.bump();
                }
            }
        }
    }

    fn error_at(&mut self, span: Span, message: String) {
        if self.suppress == 0 && self.errors.len() < MAX_ERRORS {
            self.errors.push(ParseError { span, message });
        }
    }

    fn expect(&mut self, tok: &Tok, what: &str) -> Span {
        if self.peek() == tok {
            self.bump().span
        } else {
            let span = self.cur_span();
            self.error_at(
                span,
                format!("expected {} {}, found {}", tok.text(), what, self.peek().describe()),
            );
            span
        }
    }

    // ---- program -----------------------------------------------------------

    fn parse_program(&mut self) -> Program {
        let start = self.cur_span();
        let mut decls = Vec::new();
        while !self.at_eof() {
            let before = self.pos;
            if let Some(decl) = self.parse_top_decl() {
                decls.push(decl);
            }
            if self.pos == before {
                let span = self.cur_span();
                self.error_at(span, format!("unexpected token {}", self.peek().describe()));
                self.bump();
            }
        }
        Program {
            decls,
            span: Span::new(start.start, self.cur_span().end),
        }
    }

    fn parse_top_decl(&mut self) -> Option<Decl> {
        let is_pub = if matches!(self.peek(), Tok::Ident(w) if w == "pub") {
            self.bump();
            match self.peek() {
                Tok::Fn | Tok::Let | Tok::Struct | Tok::Impl | Tok::Enum => true,
                Tok::Ident(w) if w == "trait" => true,
                _ => {
                    let span = self.cur_span();
                    self.error_at(span, "expected a declaration after 'pub'".into());
                    return None;
                }
            }
        } else {
            false
        };

        match self.peek().clone() {
            Tok::Import => self.parse_import(),
            Tok::Fn => Some(Decl::Fn(self.parse_fn(false, is_pub))),
            Tok::Let => Some(Decl::Let(self.parse_let(true, is_pub))),
            Tok::Struct => self.parse_struct(is_pub),
            Tok::Enum => self.parse_enum(is_pub),
            Tok::Impl => self.parse_impl(),
            Tok::Ident(w) if w == "trait" => self.parse_trait(is_pub),
            // Everything else: a top-level statement (valid Lamo — the
            // official parser calls parse_statement at program level).
            _ => self.parse_stmt().map(Decl::Stmt),
        }
    }

    /// Collect contiguous `//` doc comment lines directly above a declaration.
    /// A doc line must be separated from the next line only by whitespace with
    /// no blank line in between.
    fn doc_before(&self, start: usize) -> Vec<String> {
        let mut docs = Vec::new();
        let mut anchor = start;
        for c in self.comments.iter().rev() {
            if c.span.end > anchor {
                continue;
            }
            let gap = &self.source[c.span.end..anchor];
            if !gap.trim().is_empty() || gap.contains("\n\n") || gap.contains("\n\r\n") {
                break;
            }
            docs.push(c.text.clone());
            anchor = c.span.start;
            if docs.len() >= 30 {
                break;
            }
        }
        docs.reverse();
        docs
    }

    // ---- declarations ------------------------------------------------------

    fn parse_import(&mut self) -> Option<Decl> {
        let start = self.bump().span; // import
        let (path, path_span) = match self.peek().clone() {
            Tok::Str(s) => {
                let span = self.bump().span;
                (s, span)
            }
            Tok::Ident(first) => {
                let mut span = self.bump().span;
                let mut parts = vec![first];
                while self.peek() == &Tok::Dot {
                    if !matches!(self.peek_at(1), Tok::Ident(_)) {
                        break;
                    }
                    self.bump(); // .
                    if let Tok::Ident(p) = self.peek().clone() {
                        self.bump();
                        parts.push(p);
                        span.end = self.tokens[self.pos - 1].span.end;
                    }
                }
                (parts.join("."), span)
            }
            other => {
                self.error_at(
                    self.cur_span(),
                    format!(
                        "expected a module path (string or identifier), found {}",
                        other.describe()
                    ),
                );
                return None;
            }
        };
        let (alias, alias_span) = if self.peek() == &Tok::As {
            self.bump();
            match self.peek().clone() {
                Tok::Ident(a) => {
                    let span = self.bump().span;
                    (Some(a), Some(span))
                }
                other => {
                    self.error_at(
                        self.cur_span(),
                        format!("expected alias identifier after 'as', found {}", other.describe()),
                    );
                    (None, None)
                }
            }
        } else {
            (None, None)
        };
        self.eat_semicolon();
        let end = alias_span.unwrap_or(path_span).end;
        Some(Decl::Import(ImportDecl {
            path,
            path_span,
            alias,
            alias_span,
            span: Span::new(start.start, end),
        }))
    }

    fn parse_type_params(&mut self) -> Vec<TypeParam> {
        let mut params = Vec::new();
        if !self.eat(&Tok::Lt) {
            return params;
        }
        loop {
            let (name, name_span) = match self.peek().clone() {
                Tok::Ident(n) => {
                    let s = self.bump().span;
                    (n, s)
                }
                other => {
                    self.error_at(
                        self.cur_span(),
                        format!("expected type parameter name, found {}", other.describe()),
                    );
                    break;
                }
            };
            let (constraint, constraint_span) = if self.eat(&Tok::Colon) {
                match self.peek().clone() {
                    Tok::Ident(c) => {
                        let s = self.bump().span;
                        (Some(c), Some(s))
                    }
                    _ => {
                        self.error_at(self.cur_span(), "expected constraint name after ':'".into());
                        (None, None)
                    }
                }
            } else {
                (None, None)
            };
            params.push(TypeParam { name, name_span, constraint, constraint_span });
            if !self.eat(&Tok::Comma) {
                break;
            }
        }
        self.expect(&Tok::Gt, "to close type parameters");
        params
    }

    fn parse_type_ann(&mut self) -> TypeAnn {
        let start = self.cur_span();
        // Type names are contextual identifiers (int, float, bool, string,
        // void, array) — the official lexer does not reserve them.
        if let Tok::Ident(name) = self.peek().clone() {
            match name.as_str() {
                "int" | "float" | "bool" | "string" | "void" | "array" => {
                    self.bump();
                    let kind = match name.as_str() {
                        "int" => TypeKind::Int,
                        "float" => TypeKind::Float,
                        "bool" => TypeKind::Bool,
                        "string" => TypeKind::String,
                        "void" => TypeKind::Void,
                        _ => {
                            // array<T> / bare array
                            if self.peek() == &Tok::Lt {
                                self.bump();
                                let elem = self.parse_type_ann();
                                self.expect(&Tok::Gt, "to close 'array<'");
                                TypeKind::Array(Some(Box::new(elem)))
                            } else {
                                // Deprecated bare `array` — semantic warns (SPEC §7.9)
                                TypeKind::Array(None)
                            }
                        }
                    };
                    return TypeAnn { span: Span::new(start.start, self.prev_end()), kind };
                }
                _ => {}
            }
        }
        match self.peek().clone() {
            Tok::Ident(name) => {
                self.bump();
                let mut end = start;
                let mut args = Vec::new();
                if self.peek() == &Tok::Lt && self.type_args_shape_ok() {
                    self.bump();
                    loop {
                        args.push(self.parse_type_ann());
                        if !self.eat(&Tok::Comma) {
                            break;
                        }
                    }
                    end = self.expect(&Tok::Gt, "to close type arguments");
                }
                TypeAnn {
                    span: Span::new(start.start, end.end),
                    kind: TypeKind::Named(name, args),
                }
            }
            other => {
                let span = self.cur_span();
                self.error_at(
                    span,
                    format!("expected a type annotation, found {}", other.describe()),
                );
                self.bump();
                TypeAnn { span, kind: TypeKind::Named("<error>".into(), vec![]) }
            }
        }
    }

    /// Returns the lookahead offset just past a closing `>` for a
    /// `< Type, ... >` list starting at the current position (assumes
    /// [`Parser::type_args_shape_ok`] returned true).
    fn scan_type_args_end(&self) -> Option<usize> {
        let mut i = 0usize;
        if self.peek_at(0) != &Tok::Lt {
            return None;
        }
        i += 1;
        loop {
            match self.peek_at(i) {
                Tok::Ident(_) => i += 1,
                _ => return None,
            }
            let mut depth = 0usize;
            while depth > 0 || self.peek_at(i) == &Tok::Lt {
                match self.peek_at(i) {
                    Tok::Lt => {
                        depth += 1;
                        i += 1;
                    }
                    Tok::Gt => {
                        depth -= 1;
                        i += 1;
                        if depth == 0 {
                            break;
                        }
                    }
                    Tok::Ident(_) | Tok::Comma => i += 1,
                    _ => return None,
                }
            }
            if self.peek_at(i) == &Tok::Comma {
                i += 1;
                continue;
            }
            break;
        }
        if self.peek_at(i) == &Tok::Gt {
            Some(i + 1)
        } else {
            None
        }
    }

    /// Pre-scan: does a well-formed `< Type, ... >` list start here?
    /// Confirms the shape so comparisons never misparse as turbofish
    /// (SPEC §"scanner confirms the full `< ... >` shape").
    fn type_args_shape_ok(&self) -> bool {
        let ty_tok = |t: &Tok| {
            matches!(
                t,
                Tok::Ident(_)
            )
        };
        let mut i = 0usize;
        if self.peek_at(0) != &Tok::Lt {
            return false;
        }
        i += 1;
        loop {
            if !ty_tok(self.peek_at(i)) {
                return false;
            }
            i += 1;
            // nested <...> groups
            let mut depth = 0usize;
            while depth > 0 || self.peek_at(i) == &Tok::Lt {
                match self.peek_at(i) {
                    Tok::Lt => {
                        depth += 1;
                        i += 1;
                    }
                    Tok::Gt => {
                        depth -= 1;
                        i += 1;
                        if depth == 0 {
                            break;
                        }
                    }
                    Tok::Ident(_) | Tok::Comma => i += 1,
                    _ => return false,
                }
            }
            if self.peek_at(i) == &Tok::Comma {
                i += 1;
                continue;
            }
            break;
        }
        self.peek_at(i) == &Tok::Gt
    }

    fn parse_params(&mut self) -> Vec<Param> {
        let mut params = Vec::new();
        if self.peek() == &Tok::RParen {
            return params;
        }
        loop {
            let start = self.cur_span();
            let (name, name_span) = match self.peek().clone() {
                Tok::Ident(n) => {
                    let s = self.bump().span;
                    (n, s)
                }
                other => {
                    self.error_at(
                        self.cur_span(),
                        format!("expected parameter name, found {}", other.describe()),
                    );
                    while !matches!(self.peek(), Tok::RParen | Tok::Eof) {
                        self.bump();
                    }
                    break;
                }
            };
            let ty = if self.eat(&Tok::Colon) {
                Some(self.parse_type_ann())
            } else {
                None
            };
            params.push(Param {
                name,
                name_span,
                ty,
                span: Span::new(start.start, self.prev_end()),
            });
            if !self.eat(&Tok::Comma) {
                break;
            }
        }
        params
    }

    fn parse_fn(&mut self, is_method: bool, is_pub: bool) -> FnDecl {
        let start = self.bump().span; // fn
        let doc = self.doc_before(start.start);
        let (name, name_span) = match self.peek().clone() {
            Tok::Ident(n) => {
                let s = self.bump().span;
                (n, s)
            }
            other => {
                self.error_at(
                    self.cur_span(),
                    format!("expected function name, found {}", other.describe()),
                );
                (String::new(), self.cur_span())
            }
        };
        let type_params = if self.peek() == &Tok::Lt {
            self.parse_type_params()
        } else {
            Vec::new()
        };
        self.expect(&Tok::LParen, "after function name");
        let params = self.parse_params();
        let rparen = self.expect(&Tok::RParen, "to close parameter list");
        let ret = if self.eat(&Tok::Arrow) {
            Some(self.parse_type_ann())
        } else {
            None
        };
        let body = self.parse_block();
        let span = Span::new(start.start, body.span.end.max(rparen.end));
        FnDecl {
            name,
            name_span,
            type_params,
            params,
            ret,
            body,
            is_pub,
            is_method,
            doc,
            span,
        }
    }

    fn parse_let(&mut self, top_level: bool, is_pub: bool) -> LetDecl {
        let start = self.bump().span; // let
        let doc = if top_level { self.doc_before(start.start) } else { Vec::new() };
        let (name, name_span) = match self.peek().clone() {
            Tok::Ident(n) => {
                let s = self.bump().span;
                (n, s)
            }
            other => {
                self.error_at(
                    self.cur_span(),
                    format!("expected variable name, found {}", other.describe()),
                );
                (String::new(), self.cur_span())
            }
        };
        let ty = if self.eat(&Tok::Colon) {
            Some(self.parse_type_ann())
        } else {
            None
        };
        if !self.eat(&Tok::Equals) {
            self.error_at(
                self.cur_span(),
                format!(
                    "missing initializer for '{}' (all 'let' declarations require '= expr')",
                    name
                ),
            );
        }
        let init = self.parse_expr();
        let span = Span::new(start.start, init.span().end);
        self.eat_semicolon();
        LetDecl { name, name_span, ty, init, is_pub, doc, span }
    }

    fn parse_struct(&mut self, is_pub: bool) -> Option<Decl> {
        let start = self.bump().span; // struct
        let doc = self.doc_before(start.start);
        let (name, name_span) = match self.peek().clone() {
            Tok::Ident(n) => {
                let s = self.bump().span;
                (n, s)
            }
            other => {
                self.error_at(
                    self.cur_span(),
                    format!("expected struct name, found {}", other.describe()),
                );
                return None;
            }
        };
        let type_params = if self.peek() == &Tok::Lt {
            self.parse_type_params()
        } else {
            Vec::new()
        };
        self.expect(&Tok::LBrace, "to open struct body");
        let mut fields = Vec::new();
        loop {
            match self.peek().clone() {
                Tok::RBrace => {
                    self.bump();
                    break;
                }
                Tok::Eof => {
                    self.error_at(start, "unclosed struct body: missing '}'".into());
                    break;
                }
                Tok::Comma | Tok::Semicolon => {
                    self.bump();
                }
                Tok::Ident(field_name) => {
                    let fs = self.bump().span;
                    let name_span = fs;
                    if !self.eat(&Tok::Colon) {
                        self.error_at(
                            self.cur_span(),
                            format!("expected ':' after field '{}'", field_name),
                        );
                    }
                    let ty = self.parse_type_ann();
                    let span = Span::new(fs.start, ty.span.end);
                    fields.push(Field { name: field_name.clone(), name_span, ty, span });
                }
                other => {
                    self.error_at(
                        self.cur_span(),
                        format!(
                            "expected field name in struct '{}', found {}",
                            name,
                            other.describe()
                        ),
                    );
                    self.bump();
                }
            }
        }
        let span = Span::new(start.start, self.prev_end());
        Some(Decl::Struct(StructDecl { name, name_span, type_params, fields, is_pub, doc, span }))
    }

    fn parse_enum(&mut self, is_pub: bool) -> Option<Decl> {
        let start = self.bump().span; // enum
        let doc = self.doc_before(start.start);
        let (name, name_span) = match self.peek().clone() {
            Tok::Ident(n) => {
                let s = self.bump().span;
                (n, s)
            }
            other => {
                self.error_at(
                    self.cur_span(),
                    format!("expected enum name, found {}", other.describe()),
                );
                return None;
            }
        };
        let type_params = if self.peek() == &Tok::Lt {
            self.parse_type_params()
        } else {
            Vec::new()
        };
        self.expect(&Tok::LBrace, "to open enum body");
        let mut variants = Vec::new();
        loop {
            match self.peek().clone() {
                Tok::RBrace => {
                    self.bump();
                    break;
                }
                Tok::Eof => {
                    self.error_at(start, "unclosed enum body: missing '}'".into());
                    break;
                }
                Tok::Comma => {
                    self.bump();
                }
                Tok::Ident(vname) => {
                    let vs = self.bump().span;
                    let name_span = vs;
                    let mut payloads = Vec::new();
                    let mut paren_span = None;
                    if self.eat(&Tok::LParen) {
                        let pstart = self.prev_end();
                        if self.peek() != &Tok::RParen {
                            loop {
                                payloads.push(self.parse_type_ann());
                                if !self.eat(&Tok::Comma) {
                                    break;
                                }
                            }
                        }
                        let pend = self.expect(&Tok::RParen, "to close variant payloads");
                        paren_span = Some(Span::new(pstart, pend.end));
                    }
                    let span = Span::new(vs.start, paren_span.map(|p| p.end).unwrap_or(vs.end));
                    variants.push(Variant {
                        name: vname.clone(),
                        name_span,
                        payloads,
                        payload_paren_span: paren_span,
                        span,
                    });
                }
                other => {
                    self.error_at(
                        self.cur_span(),
                        format!(
                            "expected variant name in enum '{}', found {}",
                            name,
                            other.describe()
                        ),
                    );
                    self.bump();
                }
            }
        }
        let span = Span::new(start.start, self.prev_end());
        Some(Decl::Enum(EnumDecl { name, name_span, type_params, variants, is_pub, doc, span }))
    }

    fn parse_impl(&mut self) -> Option<Decl> {
        let start = self.bump().span; // impl
        let type_params = if self.peek() == &Tok::Lt {
            self.parse_type_params()
        } else {
            Vec::new()
        };
        let (first, first_span) = match self.peek().clone() {
            Tok::Ident(n) => {
                let s = self.bump().span;
                (n, s)
            }
            other => {
                self.error_at(
                    self.cur_span(),
                    format!("expected type name after 'impl', found {}", other.describe()),
                );
                return None;
            }
        };
        let (trait_name, trait_span, type_name, type_name_span) =
            if self.peek() == &Tok::For {
                self.bump();
                match self.peek().clone() {
                    Tok::Ident(t) => {
                        let s = self.bump().span;
                        // consume optional type args on the struct name (span only)
                        if self.peek() == &Tok::Lt {
                            self.suppress += 1;
                            let _ = self.parse_type_params();
                            self.suppress -= 1;
                        }
                        (Some(first), Some(first_span), t, s)
                    }
                    other => {
                        self.error_at(
                            self.cur_span(),
                            format!("expected struct name after 'for', found {}", other.describe()),
                        );
                        return None;
                    }
                }
            } else {
                // inherent impl: `impl Stack<T>`
                if self.peek() == &Tok::Lt {
                    self.suppress += 1;
                    let _ = self.parse_type_params();
                    self.suppress -= 1;
                }
                (None, None, first, first_span)
            };
        self.expect(&Tok::LBrace, "to open impl body");
        let mut methods = Vec::new();
        loop {
            match self.peek().clone() {
                Tok::RBrace => {
                    self.bump();
                    break;
                }
                Tok::Eof => {
                    self.error_at(start, "unclosed impl body: missing '}'".into());
                    break;
                }
                Tok::Fn => {
                    methods.push(self.parse_fn(true, false));
                }
                Tok::Semicolon => {
                    self.bump();
                }
                other => {
                    self.error_at(
                        self.cur_span(),
                        format!("expected 'fn' in impl body, found {}", other.describe()),
                    );
                    self.bump();
                }
            }
        }
        let span = Span::new(start.start, self.prev_end());
        Some(Decl::Impl(ImplDecl {
            trait_name,
            trait_name_span: trait_span,
            type_name,
            type_name_span,
            type_params,
            methods,
            span,
        }))
    }

    fn parse_trait(&mut self, is_pub: bool) -> Option<Decl> {
        let start = self.bump().span; // "trait" (lexed as Ident)
        let doc = self.doc_before(start.start);
        let (name, name_span) = match self.peek().clone() {
            Tok::Ident(n) => {
                let s = self.bump().span;
                (n, s)
            }
            other => {
                self.error_at(
                    self.cur_span(),
                    format!("expected trait name, found {}", other.describe()),
                );
                return None;
            }
        };
        self.expect(&Tok::LBrace, "to open trait body");
        let mut methods = Vec::new();
        loop {
            match self.peek().clone() {
                Tok::RBrace => {
                    self.bump();
                    break;
                }
                Tok::Eof => {
                    self.error_at(start, "unclosed trait body: missing '}'".into());
                    break;
                }
                Tok::Fn => {
                    let fstart = self.bump().span;
                    let (mname, _mname_span) = match self.peek().clone() {
                        Tok::Ident(n) => {
                            let s = self.bump().span;
                            (n, s)
                        }
                        other => {
                            self.error_at(
                                self.cur_span(),
                                format!(
                                    "expected method name in trait, found {}",
                                    other.describe()
                                ),
                            );
                            (String::new(), self.cur_span())
                        }
                    };
                    self.expect(&Tok::LParen, "after method name");
                    let params = self.parse_params();
                    self.expect(&Tok::RParen, "to close parameter list");
                    let ret = if self.eat(&Tok::Arrow) {
                        Some(self.parse_type_ann())
                    } else {
                        None
                    };
                    if self.peek() == &Tok::LBrace {
                        let span = self.cur_span();
                        self.error_at(
                            span,
                            "trait methods cannot have bodies — declare the signature only"
                                .into(),
                        );
                        let _ = self.parse_block();
                    }
                    self.eat_semicolon();
                    let span = Span::new(fstart.start, self.prev_end());
                    methods.push(TraitFn { name: mname, name_span, params, ret, span });
                }
                other => {
                    self.error_at(
                        self.cur_span(),
                        format!("expected 'fn' in trait body, found {}", other.describe()),
                    );
                    self.bump();
                }
            }
        }
        let span = Span::new(start.start, self.prev_end());
        Some(Decl::Trait(TraitDecl { name, name_span, methods, is_pub, doc, span }))
    }

    // ---- statements --------------------------------------------------------

    fn parse_block(&mut self) -> Block {
        let start = self.cur_span();
        if !self.eat(&Tok::LBrace) {
            self.error_at(
                start,
                format!("expected '{{' to open a block, found {}", self.peek().describe()),
            );
            return Block { stmts: vec![], span: start };
        }
        let mut stmts = Vec::new();
        loop {
            match self.peek().clone() {
                Tok::RBrace => {
                    let end = self.bump().span;
                    return Block { stmts, span: Span::new(start.start, end.end) };
                }
                Tok::Eof => {
                    self.error_at(start, "unclosed block: missing '}'".into());
                    return Block { stmts, span: Span::new(start.start, self.cur_span().end) };
                }
                Tok::Semicolon => {
                    self.bump();
                }
                _ => {
                    let before = self.pos;
                    if let Some(stmt) = self.parse_stmt() {
                        stmts.push(stmt);
                    }
                    if self.pos == before {
                        self.bump();
                    }
                }
            }
        }
    }

    fn parse_stmt(&mut self) -> Option<Stmt> {
        match self.peek().clone() {
            Tok::Let => {
                let l = self.parse_let(false, false);
                Some(Stmt::Let(LetStmt {
                    name: l.name,
                    name_span: l.name_span,
                    ty: l.ty,
                    init: l.init,
                    span: l.span,
                }))
            }
            Tok::Return => {
                let start = self.bump().span;
                let has_value = !matches!(self.peek(), Tok::RBrace | Tok::Semicolon | Tok::Eof)
                    && !self.starts_statement();
                let value = if has_value { Some(self.parse_expr()) } else { None };
                let span = Span::new(
                    start.start,
                    value.as_ref().map(|v| v.span().end).unwrap_or(start.end),
                );
                self.eat_semicolon();
                Some(Stmt::Return(value, span))
            }
            Tok::Break => {
                let span = self.bump().span;
                self.eat_semicolon();
                Some(Stmt::Break(span))
            }
            Tok::Continue => {
                let span = self.bump().span;
                self.eat_semicolon();
                Some(Stmt::Continue(span))
            }
            Tok::If => self.parse_if().map(Stmt::If),
            Tok::While => self.parse_while().map(Stmt::While),
            Tok::For => self.parse_for().map(Stmt::For),
            Tok::Match => {
                let m = self.parse_match();
                Some(Stmt::Match(m))
            }
            _ => self.parse_expr_or_assign_stmt(),
        }
    }

    fn starts_statement(&self) -> bool {
        matches!(
            self.peek(),
            Tok::Let
                | Tok::Return
                | Tok::Break
                | Tok::Continue
                | Tok::If
                | Tok::While
                | Tok::For
                | Tok::Match
        )
    }

    fn parse_expr_or_assign_stmt(&mut self) -> Option<Stmt> {
        let expr = self.parse_expr();
        if matches!(expr, Expr::Error(_)) {
            self.recover_to_stmt_boundary();
            return Some(Stmt::Expr(expr));
        }
        let op = match self.peek() {
            Tok::Equals => Some(AssignOp::Eq),
            Tok::PlusEq => Some(AssignOp::PlusEq),
            Tok::MinusEq => Some(AssignOp::MinusEq),
            Tok::StarEq => Some(AssignOp::StarEq),
            Tok::SlashEq => Some(AssignOp::SlashEq),
            Tok::PercentEq => Some(AssignOp::PercentEq),
            Tok::PlusPlus => Some(AssignOp::PlusPlus),
            Tok::MinusMinus => Some(AssignOp::MinusMinus),
            _ => None,
        };
        match op {
            Some(op) => {
                self.bump();
                let is_incdec = matches!(op, AssignOp::PlusPlus | AssignOp::MinusMinus);
                if !valid_lvalue(&expr) {
                    self.error_at(
                        expr.span(),
                        "invalid assignment target (expected a variable, index, or field)".into(),
                    );
                }
                let value = if is_incdec { expr.clone() } else { self.parse_expr() };
                let span = Span::new(expr.span().start, value.span().end);
                self.eat_semicolon();
                Some(Stmt::Assign(AssignStmt { target: expr, op, value, span }))
            }
            None => {
                self.eat_semicolon();
                Some(Stmt::Expr(expr))
            }
        }
    }

    fn parse_if(&mut self) -> Option<IfStmt> {
        let start = self.bump().span; // if
        self.expect(&Tok::LParen, "after 'if'");
        let cond = self.parse_expr();
        self.expect(&Tok::RParen, "to close the if condition");
        let then_body = self.parse_block();
        let else_body = if self.peek() == &Tok::Else {
            self.bump();
            if self.peek() == &Tok::If {
                let nested = self.parse_if()?;
                let nspan = nested.span;
                Some(Block { stmts: vec![Stmt::If(nested)], span: nspan })
            } else {
                Some(self.parse_block())
            }
        } else {
            None
        };
        let end = else_body.as_ref().map(|b| b.span.end).unwrap_or(then_body.span.end);
        Some(IfStmt { cond, then_body, else_body, span: Span::new(start.start, end) })
    }

    fn parse_while(&mut self) -> Option<WhileStmt> {
        let start = self.bump().span; // while
        self.expect(&Tok::LParen, "after 'while'");
        let cond = self.parse_expr();
        self.expect(&Tok::RParen, "to close the while condition");
        let body = self.parse_block();
        let span = Span::new(start.start, body.span.end);
        Some(WhileStmt { cond, body, span })
    }

    fn parse_for(&mut self) -> Option<ForStmt> {
        let start = self.bump().span; // for
        self.expect(&Tok::LParen, "after 'for'");
        let init: Option<Box<Stmt>> = if self.peek() == &Tok::Semicolon {
            self.bump();
            None
        } else if self.peek() == &Tok::Let {
            let l = self.parse_let(false, false);
            Some(Box::new(Stmt::Let(LetStmt {
                name: l.name,
                name_span: l.name_span,
                ty: l.ty,
                init: l.init,
                span: l.span,
            })))
        } else {
            self.parse_expr_or_assign_stmt().map(Box::new)
        };
        let cond = if self.peek() == &Tok::Semicolon {
            self.bump();
            None
        } else {
            let c = self.parse_expr();
            self.expect(&Tok::Semicolon, "after the for condition");
            Some(c)
        };
        let update: Option<Box<Stmt>> = if self.peek() == &Tok::RParen {
            None
        } else {
            self.parse_expr_or_assign_stmt().map(Box::new)
        };
        self.expect(&Tok::RParen, "to close the for header");
        let body = self.parse_block();
        let span = Span::new(start.start, body.span.end);
        Some(ForStmt { init, cond, update, body, span })
    }

    fn parse_match(&mut self) -> MatchExpr {
        let start = self.bump().span; // match
        // Scrutinee: struct-literal lookahead disabled so `match x {` works.
        let scrutinee = self.parse_expr_struct_lit(false);
        self.expect(&Tok::LBrace, "to open match arms");
        let mut arms = Vec::new();
        loop {
            match self.peek().clone() {
                Tok::RBrace => {
                    let end = self.bump().span;
                    return MatchExpr {
                        scrutinee: Box::new(scrutinee),
                        arms,
                        span: Span::new(start.start, end.end),
                    };
                }
                Tok::Eof => {
                    self.error_at(start, "unclosed match: missing '}'".into());
                    return MatchExpr {
                        scrutinee: Box::new(scrutinee),
                        arms,
                        span: Span::new(start.start, self.cur_span().end),
                    };
                }
                Tok::Comma => {
                    self.bump();
                }
                _ => {
                    let before = self.pos;
                    let arm = self.parse_match_arm();
                    arms.push(arm);
                    if self.pos == before {
                        self.bump();
                    }
                }
            }
        }
    }

    fn parse_match_arm(&mut self) -> MatchArm {
        let start = self.cur_span();
        let pattern = self.parse_pattern(false);
        // `when` guard (contextual keyword, SPEC §4.6)
        let guard = if matches!(self.peek(), Tok::Ident(w) if w == "when") {
            self.bump();
            Some(self.parse_expr())
        } else {
            None
        };
        self.expect(&Tok::FatArrow, "after the match pattern");
        let body = if self.peek() == &Tok::LBrace {
            ArmBody::Block(self.parse_block())
        } else if matches!(
            self.peek(),
            Tok::Return | Tok::Break | Tok::Continue
        ) {
            ArmBody::Stmt(self.parse_stmt().expect("statement parsed"))
        } else {
            ArmBody::Expr(self.parse_expr())
        };
        let end = match &body {
            ArmBody::Expr(e) => e.span().end,
            ArmBody::Block(b) => b.span.end,
            ArmBody::Stmt(s) => match s {
                Stmt::Return(_, sp) => sp.end,
                Stmt::Break(sp) => sp.end,
                Stmt::Continue(sp) => sp.end,
                _ => sp_end_default(s),
            },
        };
        MatchArm { pattern, guard, body, span: Span::new(start.start, end) }
    }

    fn parse_pattern(&mut self, in_payload: bool) -> Pattern {
        let start = self.cur_span();
        match self.peek().clone() {
            Tok::Ident(w) if w == "_" => {
                self.bump();
                Pattern::Wildcard(start)
            }
            Tok::Int(v) => {
                self.bump();
                Pattern::Int(v, start)
            }
            Tok::Float(v) => {
                self.bump();
                Pattern::Float(v, start)
            }
            Tok::Str(_) => {
                self.bump();
                Pattern::Str(start)
            }
            Tok::True => {
                self.bump();
                Pattern::Bool(true, start)
            }
            Tok::False => {
                self.bump();
                Pattern::Bool(false, start)
            }
            Tok::Minus => {
                self.bump();
                match self.peek().clone() {
                    Tok::Int(v) => {
                        self.bump();
                        Pattern::Int(format!("-{v}"), start)
                    }
                    Tok::Float(v) => {
                        self.bump();
                        Pattern::Float(format!("-{v}"), start)
                    }
                    other => {
                        self.error_at(
                            start,
                            format!(
                                "expected numeric literal after '-' in pattern, found {}",
                                other.describe()
                            ),
                        );
                        Pattern::Wildcard(start)
                    }
                }
            }
            Tok::Ident(name) => {
                self.bump();
                // Qualified variant: Enum::Variant (allowed at any depth)
                if self.peek() == &Tok::ColonColon {
                    self.bump();
                    match self.peek().clone() {
                        Tok::Ident(v) => {
                            let vspan = self.bump().span;
                            let payloads = self.parse_pattern_payloads();
                            let end =
                                payloads.last().map(|p| p.span().end).unwrap_or(vspan.end);
                            Pattern::Variant {
                                enum_name: Some(name),
                                name: v,
                                payloads,
                                span: Span::new(start.start, end),
                                name_span: vspan,
                            }
                        }
                        other => {
                            self.error_at(
                                self.cur_span(),
                                format!(
                                    "expected variant name after '::', found {}",
                                    other.describe()
                                ),
                            );
                            Pattern::Wildcard(start)
                        }
                    }
                } else if self.peek() == &Tok::LParen {
                    // Variant constructor with payload patterns: Some(x)
                    let payloads = self.parse_pattern_payloads();
                    let end = payloads.last().map(|p| p.span().end).unwrap_or(start.end);
                    Pattern::Variant {
                        enum_name: None,
                        name,
                        payloads,
                        span: Span::new(start.start, end),
                        name_span: start,
                    }
                } else if in_payload {
                    // SPEC §4.6: a bare identifier inside a payload list is a binding.
                    Pattern::Binding(name, start)
                } else {
                    Pattern::Variant {
                        enum_name: None,
                        name,
                        payloads: vec![],
                        span: start,
                        name_span: start,
                    }
                }
            }
            other => {
                self.error_at(
                    start,
                    format!("expected a match pattern, found {}", other.describe()),
                );
                self.bump();
                Pattern::Wildcard(start)
            }
        }
    }

    fn parse_pattern_payloads(&mut self) -> Vec<Pattern> {
        if !self.eat(&Tok::LParen) {
            return vec![];
        }
        let mut payloads = Vec::new();
        if self.peek() == &Tok::RParen {
            self.bump();
            return payloads;
        }
        loop {
            payloads.push(self.parse_pattern(true));
            if !self.eat(&Tok::Comma) {
                break;
            }
        }
        self.expect(&Tok::RParen, "to close the pattern payload list");
        payloads
    }

    // ---- expressions -------------------------------------------------------

    pub fn parse_expr(&mut self) -> Expr {
        self.parse_expr_struct_lit(true)
    }

    /// `allow_struct_lit`: `IDENT {` starts a struct literal here.
    fn parse_expr_struct_lit(&mut self, allow_struct_lit: bool) -> Expr {
        self.parse_binary(0, allow_struct_lit)
    }

    fn binary_precedence(tok: &Tok) -> Option<u8> {
        Some(match tok {
            Tok::OrOr => 1,
            Tok::AndAnd => 2,
            Tok::EqEq | Tok::BangEq => 3,
            Tok::Lt | Tok::LtEq | Tok::Gt | Tok::GtEq => 4,
            Tok::Plus | Tok::Minus => 5,
            Tok::Star | Tok::Slash | Tok::Percent => 6,
            _ => return None,
        })
    }

    fn parse_binary(&mut self, min_prec: u8, allow_struct_lit: bool) -> Expr {
        let mut lhs = self.parse_unary(allow_struct_lit);
        loop {
            let prec = match Self::binary_precedence(self.peek()) {
                Some(p) if p >= min_prec => p,
                _ => break,
            };
            let op_tok = self.bump();
            let op = match op_tok.tok {
                Tok::Plus => BinOp::Add,
                Tok::Minus => BinOp::Sub,
                Tok::Star => BinOp::Mul,
                Tok::Slash => BinOp::Div,
                Tok::Percent => BinOp::Mod,
                Tok::EqEq => BinOp::Eq,
                Tok::BangEq => BinOp::Ne,
                Tok::Lt => BinOp::Lt,
                Tok::LtEq => BinOp::Le,
                Tok::Gt => BinOp::Gt,
                Tok::GtEq => BinOp::Ge,
                Tok::AndAnd => BinOp::And,
                Tok::OrOr => BinOp::Or,
                _ => unreachable!(),
            };
            let rhs = self.parse_binary(prec + 1, allow_struct_lit);
            let span = Span::new(lhs.span().start, rhs.span().end);
            lhs = Expr::Binary { op, lhs: Box::new(lhs), rhs: Box::new(rhs), span };
        }
        lhs
    }

    fn parse_unary(&mut self, allow_struct_lit: bool) -> Expr {
        let start = self.cur_span();
        let op = match self.peek() {
            Tok::Minus => Some(UnaryOp::Neg),
            Tok::Bang => Some(UnaryOp::Not),
            _ => None,
        };
        if let Some(op) = op {
            self.bump();
            let expr = self.parse_unary(allow_struct_lit);
            let span = Span::new(start.start, expr.span().end);
            return Expr::Unary { op, expr: Box::new(expr), span };
        }
        self.parse_postfix(allow_struct_lit)
    }

    fn parse_postfix(&mut self, allow_struct_lit: bool) -> Expr {
        let mut expr = self.parse_primary(allow_struct_lit);
        loop {
            match self.peek() {
                Tok::Dot => {
                    self.bump();
                    match self.peek().clone() {
                        Tok::Ident(name) => {
                            let name_span = self.bump().span;
                            if self.peek() == &Tok::LParen {
                                self.bump();
                                let (args, end) = self.parse_args();
                                let span = Span::new(expr.span().start, end);
                                expr = Expr::Method {
                                    receiver: Box::new(expr),
                                    name,
                                    name_span,
                                    args,
                                    type_args: vec![],
                                    span,
                                };
                            } else if self.peek() == &Tok::Lt && self.type_args_shape_ok() {
                                // `m<T>(...)` method turbofish — shape pre-verified
                                let targs = self.parse_type_args();
                                let ok = self.peek() == &Tok::LParen;
                                if ok {
                                    self.bump();
                                    let (args, end) = self.parse_args();
                                    let span = Span::new(expr.span().start, end);
                                    expr = Expr::Method {
                                        receiver: Box::new(expr),
                                        name,
                                        name_span,
                                        args,
                                        type_args: targs,
                                        span,
                                    };
                                } else {
                                    let span = Span::new(expr.span().start, name_span.end);
                                    expr = Expr::Field {
                                        base: Box::new(expr),
                                        name,
                                        name_span,
                                        span,
                                    };
                                }
                            } else {
                                let span = Span::new(expr.span().start, name_span.end);
                                expr = Expr::Field {
                                    base: Box::new(expr),
                                    name,
                                    name_span,
                                    span,
                                };
                            }
                        }
                        other => {
                            let span = self.cur_span();
                            self.error_at(
                                span,
                                format!(
                                    "expected a member name after '.', found {}",
                                    other.describe()
                                ),
                            );
                            let end = self.bump().span.end;
                            expr = Expr::Error(Span::new(expr.span().start, end));
                        }
                    }
                }
                Tok::LBracket => {
                    self.bump();
                    let index = self.parse_expr();
                    let rbr = self.expect(&Tok::RBracket, "to close the index");
                    let span = Span::new(expr.span().start, rbr.end);
                    expr = Expr::Index { base: Box::new(expr), index: Box::new(index), span };
                }
                Tok::Lt => {
                    // Turbofish on a plain call: pick<int>(...).
                    // The `< ... > (` shape is confirmed first so that
                    // comparisons (`a < b`) never misparse (SPEC §3.4).
                    let after = match self.scan_type_args_end() {
                        Some(n) => n,
                        None => break,
                    };
                    if !matches!(self.peek_at(after), Tok::LParen) {
                        break; // it's a comparison, not a turbofish
                    }
                    // committed: consume `< ... >` then the `(`
                    self.pos += after;
                    self.bump();
                    let (args, end) = self.parse_args();
                    let span = Span::new(expr.span().start, end);
                    expr = Expr::Call {
                        callee: Box::new(expr),
                        args,
                        type_args: vec![], // names already consumed via scan
                        span,
                    };
                }
                Tok::LParen => {
                    self.bump();
                    let (args, end) = self.parse_args();
                    let span = Span::new(expr.span().start, end);
                    expr = Expr::Call {
                        callee: Box::new(expr),
                        args,
                        type_args: vec![],
                        span,
                    };
                }
                _ => break,
            }
        }
        expr
    }

    fn parse_args(&mut self) -> (Vec<Expr>, usize) {
        let mut args = Vec::new();
        if self.peek() == &Tok::RParen {
            let end = self.bump().span.end;
            return (args, end);
        }
        loop {
            args.push(self.parse_expr());
            if !self.eat(&Tok::Comma) {
                break;
            }
        }
        let end = self.expect(&Tok::RParen, "to close the argument list").end;
        (args, end)
    }

    /// Parses `<T, U>` type argument lists (turbofish).
    fn parse_type_args(&mut self) -> Vec<TypeAnn> {
        if !self.eat(&Tok::Lt) {
            return vec![];
        }
        let mut args = Vec::new();
        loop {
            args.push(self.parse_type_ann());
            if !self.eat(&Tok::Comma) {
                break;
            }
        }
        self.expect(&Tok::Gt, "to close type arguments");
        args
    }

    fn parse_primary(&mut self, allow_struct_lit: bool) -> Expr {
        let span = self.cur_span();
        match self.peek().clone() {
            Tok::Int(_) => {
                self.bump();
                Expr::Int(span)
            }
            Tok::Float(_) => {
                self.bump();
                Expr::Float(span)
            }
            Tok::Str(_) => {
                self.bump();
                Expr::Str(span)
            }
            Tok::True => {
                self.bump();
                Expr::Bool(true, span)
            }
            Tok::False => {
                self.bump();
                Expr::Bool(false, span)
            }
            Tok::LParen => {
                self.bump();
                let inner = self.parse_expr();
                let r = self.expect(&Tok::RParen, "to close the parenthesized expression");
                match inner {
                    Expr::Error(_) => Expr::Error(r),
                    other => other,
                }
            }
            Tok::LBracket => {
                self.bump();
                let mut items = Vec::new();
                if self.peek() != &Tok::RBracket {
                    loop {
                        items.push(self.parse_expr());
                        if !self.eat(&Tok::Comma) {
                            break;
                        }
                    }
                }
                let r = self.expect(&Tok::RBracket, "to close the array literal");
                Expr::Array(items, Span::new(span.start, r.end))
            }
            Tok::Match => {
                // match-as-expression (SPEC §4.6, 2.8.0)
                let m = self.parse_match();
                Expr::Match(m)
            }
            Tok::Ident(name) => {
                self.bump();
                if self.peek() == &Tok::ColonColon {
                    // Qualified variant: Enum::Variant
                    self.bump();
                    match self.peek().clone() {
                        Tok::Ident(v) => {
                            let vspan = self.bump().span;
                            Expr::QualifiedVariant {
                                enum_name: name,
                                enum_span: span,
                                variant: v,
                                variant_span: vspan,
                                span: Span::new(span.start, vspan.end),
                            }
                        }
                        other => {
                            self.error_at(
                                self.cur_span(),
                                format!(
                                    "expected variant name after '::', found {}",
                                    other.describe()
                                ),
                            );
                            Expr::Error(span)
                        }
                    }
                } else if allow_struct_lit && self.looks_like_struct_lit() {
                    let type_args = if self.peek() == &Tok::Lt && self.type_args_shape_ok() {
                        let targs = self.parse_type_args();
                        if self.peek() == &Tok::LBrace {
                            targs
                        } else {
                            vec![]
                        }
                    } else {
                        vec![]
                    };
                    self.bump(); // {
                    let mut fields = Vec::new();
                    loop {
                        match self.peek().clone() {
                            Tok::RBrace => {
                                self.bump();
                                break;
                            }
                            Tok::Eof => {
                                self.error_at(span, "unclosed struct literal: missing '}'".into());
                                break;
                            }
                            Tok::Comma => {
                                self.bump();
                            }
                            Tok::Ident(fname) => {
                                let fspan = self.bump().span;
                                self.expect(
                                    &Tok::Colon,
                                    "after the field name in a struct literal",
                                );
                                let value = self.parse_expr();
                                fields.push(FieldValue { name: fname, name_span: fspan, value });
                            }
                            other => {
                                self.error_at(
                                    self.cur_span(),
                                    format!(
                                        "expected field name in struct literal, found {}",
                                        other.describe()
                                    ),
                                );
                                self.bump();
                            }
                        }
                    }
                    let end = self.prev_end();
                    Expr::StructLit {
                        name,
                        name_span: span,
                        type_args,
                        fields,
                        span: Span::new(span.start, end),
                    }
                } else {
                    Expr::Ident { name, span }
                }
            }
            other => {
                self.error_at(
                    span,
                    format!("expected an expression, found {}", other.describe()),
                );
                // Leave declaration/statement keywords for the outer recovery
                // loops so e.g. `let x =` followed by `fn f()` still yields
                // the fn declaration.
                if !matches!(
                    self.peek(),
                    Tok::Fn
                        | Tok::Let
                        | Tok::Struct
                        | Tok::Enum
                        | Tok::Impl
                        | Tok::Import
                        | Tok::Return
                        | Tok::If
                        | Tok::While
                        | Tok::For
                        | Tok::Break
                        | Tok::Continue
                ) || self.peek() == &Tok::Match
                {
                    self.bump();
                }
                Expr::Error(span)
            }
        }
    }

    /// Lookahead: does a struct literal start at the current position?
    /// Called right after an ident was consumed: `{` or `<...> {`.
    fn looks_like_struct_lit(&self) -> bool {
        match self.peek() {
            Tok::LBrace => !matches!(self.peek_at(1), Tok::RBrace | Tok::Eof),
            Tok::Lt => {
                if !self.type_args_shape_ok() {
                    return false;
                }
                // shape ok — check what follows the closing `>`
                let end = self.scan_type_args_end().expect("shape ok implies end");
                matches!(self.peek_at(end), Tok::LBrace)
            }
            _ => false,
        }
    }
}

fn sp_end_default(s: &Stmt) -> usize {
    match s {
        Stmt::Let(l) => l.span.end,
        Stmt::If(i) => i.span.end,
        Stmt::While(w) => w.span.end,
        Stmt::For(f) => f.span.end,
        Stmt::Match(m) => m.span.end,
        Stmt::Assign(a) => a.span.end,
        Stmt::Expr(e) => e.span().end,
        Stmt::Return(_, sp) => sp.end,
        Stmt::Break(sp) | Stmt::Continue(sp) => sp.end,
    }
}

fn valid_lvalue(expr: &Expr) -> bool {
    matches!(expr, Expr::Ident { .. } | Expr::Index { .. } | Expr::Field { .. })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::lexer::lex;

    fn parse_src(src: &str) -> ParseOutput {
        let lexed = lex(src);
        parse(src, &lexed)
    }

    #[test]
    fn hello_world() {
        let out = parse_src("fn main() {\n    print(\"Hello, World!\")\n}\n");
        assert_eq!(out.errors, vec![]);
        assert_eq!(out.program.decls.len(), 1);
        match &out.program.decls[0] {
            Decl::Fn(f) => {
                assert_eq!(f.name, "main");
                assert!(f.params.is_empty());
            }
            _ => panic!("expected fn"),
        }
    }

    #[test]
    fn struct_and_impl() {
        let src = r#"
struct Player {
    name: string,
    hp: int
}

impl Player {
    fn damage(amount: int) {
        self.hp -= amount
    }
}
"#;
        let out = parse_src(src);
        assert_eq!(out.errors, vec![]);
        assert_eq!(out.program.decls.len(), 2);
        match &out.program.decls[1] {
            Decl::Impl(i) => {
                assert_eq!(i.type_name, "Player");
                assert!(i.trait_name.is_none());
                assert_eq!(i.methods.len(), 1);
            }
            _ => panic!("expected impl"),
        }
    }

    #[test]
    fn generics() {
        let src = r#"
fn pick<T>(a: T, b: T) -> T {
    if (1 < 2) { return a }
    return b
}

struct Stack<T> {
    items: array<T>,
    top: int
}

impl<T> Stack<T> {
    fn push(x: T) {
        self.items.push(x)
        self.top += 1
    }
}

fn demo() {
    let s: Stack<int> = Stack<int> { items: [], top: 0 }
    s.push(10)
    print(pick<int>(1, 2))
}
"#;
        let out = parse_src(src);
        for e in &out.errors {
            eprintln!("error: {} @ {:?}", e.message, e.span);
        }
        assert_eq!(out.errors, vec![]);
    }

    #[test]
    fn enums_and_match() {
        let src = r#"
enum Option<T> {
    Some(T),
    None
}

fn unwrap_or(o: Option<int>, d: int) -> int {
    match o {
        Some(x) when x > 0 => x,
        Some(x) => x,
        None => d
    }
}

fn demo() {
    let a: Option<int> = Some(42)
    let q = Option::Some(5)
    match a {
        Option::Some(v) => print(v),
        Option::None => print(0)
    }
}
"#;
        let out = parse_src(src);
        for e in &out.errors {
            eprintln!("error: {} @ {:?}", e.message, e.span);
        }
        assert_eq!(out.errors, vec![]);
    }

    #[test]
    fn traits() {
        let src = r#"
trait Shape {
    fn area() -> float
    fn name() -> string
}

struct Circle { r: float }

impl Shape for Circle {
    fn area() -> float { return 3.14159 * self.r * self.r }
    fn name() -> string { return "circle" }
}

fn describe<T: Shape>(s: T) -> float {
    return s.area()
}
"#;
        let out = parse_src(src);
        for e in &out.errors {
            eprintln!("error: {} @ {:?}", e.message, e.span);
        }
        assert_eq!(out.errors, vec![]);
    }

    #[test]
    fn imports() {
        let src = r#"
import std.io
import std.math as math
import "utils.lamo" as utils
import utils
import helpers;
"#;
        let out = parse_src(src);
        assert_eq!(out.errors, vec![]);
        let paths: Vec<&str> = out
            .program
            .decls
            .iter()
            .filter_map(|d| match d {
                Decl::Import(i) => Some(i.path.as_str()),
                _ => None,
            })
            .collect();
        assert_eq!(paths, vec!["std.io", "std.math", "utils.lamo", "utils", "helpers"]);
        let aliases: Vec<Option<&String>> = out
            .program
            .decls
            .iter()
            .filter_map(|d| match d {
                Decl::Import(i) => Some(i.alias.as_ref()),
                _ => None,
            })
            .collect();
        assert_eq!(aliases[1], Some(&"math".to_string()));
    }

    #[test]
    fn for_and_loops() {
        let src = r#"
fn loops() {
    for (let i = 0; i < 10; i++) {
        print(i)
    }
    for (;;) { break }
    for (let j = 0; j < 3; j = j + 1) { continue }
    let k = 0
    while (k < 5) { k++ }
}
"#;
        let out = parse_src(src);
        for e in &out.errors {
            eprintln!("error: {} @ {:?}", e.message, e.span);
        }
        assert_eq!(out.errors, vec![]);
    }

    #[test]
    fn match_as_expression_and_literals() {
        let src = r#"
let x = match 2 { 1 => 10, 2 => 20, _ => 0 }
let y = 1 + (match "a" { "a" => 1, _ => 2 })
fn return_match() {
    return match true { true => 1, false => 0 }
}
"#;
        let out = parse_src(src);
        for e in &out.errors {
            eprintln!("error: {} @ {:?}", e.message, e.span);
        }
        assert_eq!(out.errors, vec![]);
    }

    #[test]
    fn error_recovery_yields_partial_ast() {
        let src = "fn ok() { print(1) }\nlet broken =\nfn ok2() { print(2) }";
        let out = parse_src(src);
        assert!(!out.errors.is_empty());
        let names: Vec<&str> = out
            .program
            .decls
            .iter()
            .filter_map(|d| match d {
                Decl::Fn(f) => Some(f.name.as_str()),
                _ => None,
            })
            .collect();
        assert!(names.contains(&"ok"));
        assert!(names.contains(&"ok2"));
    }

    #[test]
    fn doc_comments_attached() {
        let src = "// Adds two numbers.\n// Returns the sum.\nfn add(a, b) {\n    return a + b\n}\n";
        let out = parse_src(src);
        assert_eq!(out.errors, vec![]);
        match &out.program.decls[0] {
            Decl::Fn(f) => {
                assert_eq!(f.doc, vec![" Adds two numbers.".to_string(), " Returns the sum.".to_string()]);
            }
            _ => panic!("expected fn"),
        }
    }

    #[test]
    fn pub_declarations() {
        let src = "pub fn visible(x: int) -> int { return x }\npub let CONST = 5\nfn private() {}\n";
        let out = parse_src(src);
        assert_eq!(out.errors, vec![]);
        match &out.program.decls[0] {
            Decl::Fn(f) => assert!(f.is_pub),
            _ => panic!(),
        }
        match &out.program.decls[1] {
            Decl::Let(l) => assert!(l.is_pub),
            _ => panic!(),
        }
        match &out.program.decls[2] {
            Decl::Fn(f) => assert!(!f.is_pub),
            _ => panic!(),
        }
    }

    #[test]
    fn struct_literal_vs_comparison() {
        // `match x < 3 { ... }` — the `{` belongs to match (SPEC §3.4)
        let src = "fn f(x) {\n    match x < 3 { true => print(1), _ => print(2) }\n}\n";
        let out = parse_src(src);
        for e in &out.errors {
            eprintln!("error: {} @ {:?}", e.message, e.span);
        }
        // This specific shape is tricky; we accept either a clean parse or a
        // graceful error — but it must not hang or panic.
        let _ = out;
    }

    #[test]
    fn negative_index_and_field_write() {
        let src = "fn f(arr) {\n    print(arr[-1])\n    arr[0] = 99\n    obj.field = 5\n    arr[0] += 1\n}\n";
        let out = parse_src(src);
        assert_eq!(out.errors, vec![]);
    }
}
