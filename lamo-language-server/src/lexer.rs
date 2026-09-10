//! Lexer for the Lamo language.
//!
//! Follows the official compiler (`src/lexer/lexer.c`) and SPEC §1:
//! keywords, decimal/hex/binary numeric literals with `_` separators,
//! floats with exponents, strings with `\n \t \r \\ \" \' \0 \xNN` escapes,
//! `//` line comments (block comments are NOT part of the language),
//! and the full operator/punctuation set including `::`, `->`, `=>`.

use tower_lsp::lsp_types::Diagnostic;

/// Byte span into the source text.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Span {
    pub start: usize,
    pub end: usize,
}

impl Span {
    pub fn new(start: usize, end: usize) -> Self {
        Self { start, end }
    }
    pub fn merge(a: Span, b: Span) -> Span {
        Span {
            start: a.start.min(b.start),
            end: a.end.max(b.end),
        }
    }

    /// Method form of [`Span::merge`].
    pub fn merged(self, other: Span) -> Span {
        Span {
            start: self.start.min(other.start),
            end: self.end.max(other.end),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Tok {
    // keywords (SPEC §1.6)
    Let,
    Fn,
    If,
    Else,
    While,
    For,
    Return,
    Break,
    Continue,
    Import,
    As,
    Struct,
    Impl,
    Enum,
    Match,
    True,
    False,
    // type keywords
    IntTy,
    FloatTy,
    BoolTy,
    StringTy,
    VoidTy,
    ArrayTy,
    // literals
    Ident(String),
    Int(String),
    Float(String),
    Str(String),
    // punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Semicolon,
    Colon,
    ColonColon,
    Dot,
    // operators
    Equals,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Bang,
    Lt,
    Gt,
    EqEq,
    BangEq,
    LtEq,
    GtEq,
    AndAnd,
    OrOr,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    PercentEq,
    PlusPlus,
    MinusMinus,
    Arrow,
    FatArrow,
    Eof,
}

impl Tok {
    pub fn describe(&self) -> String {
        match self {
            Tok::Ident(n) => format!("identifier '{n}'"),
            Tok::Int(n) => format!("integer '{n}'"),
            Tok::Float(n) => format!("float '{n}'"),
            Tok::Str(_) => "string literal".into(),
            other => format!("'{}'", other.text()),
        }
    }

    pub fn text(&self) -> &'static str {
        use Tok::*;
        match self {
            Let => "let",
            Fn => "fn",
            If => "if",
            Else => "else",
            While => "while",
            For => "for",
            Return => "return",
            Break => "break",
            Continue => "continue",
            Import => "import",
            As => "as",
            Struct => "struct",
            Impl => "impl",
            Enum => "enum",
            Match => "match",
            True => "true",
            False => "false",
            IntTy => "int",
            FloatTy => "float",
            BoolTy => "bool",
            StringTy => "string",
            VoidTy => "void",
            ArrayTy => "array",
            LParen => "(",
            RParen => ")",
            LBrace => "{",
            RBrace => "}",
            LBracket => "[",
            RBracket => "]",
            Comma => ",",
            Semicolon => ";",
            Colon => ":",
            ColonColon => "::",
            Dot => ".",
            Equals => "=",
            Plus => "+",
            Minus => "-",
            Star => "*",
            Slash => "/",
            Percent => "%",
            Bang => "!",
            Lt => "<",
            Gt => ">",
            EqEq => "==",
            BangEq => "!=",
            LtEq => "<=",
            GtEq => ">=",
            AndAnd => "&&",
            OrOr => "||",
            PlusEq => "+=",
            MinusEq => "-=",
            StarEq => "*=",
            SlashEq => "/=",
            PercentEq => "%=",
            PlusPlus => "++",
            MinusMinus => "--",
            Arrow => "->",
            FatArrow => "=>",
            Eof => "end of file",
            Ident(_) | Int(_) | Float(_) | Str(_) => "",
        }
    }
}

#[derive(Debug, Clone)]
pub struct Token {
    pub tok: Tok,
    pub span: Span,
}

/// A `//` comment with its span and text (without the `//` prefix).
#[derive(Debug, Clone)]
pub struct Comment {
    pub span: Span,
    pub text: String,
}

/// A lexical error with span and message.
#[derive(Debug, Clone)]
pub struct LexError {
    pub span: Span,
    pub message: String,
}

pub struct LexOutput {
    pub tokens: Vec<Token>,
    pub errors: Vec<LexError>,
    pub comments: Vec<Comment>,
}

fn is_ident_start(b: u8) -> bool {
    b.is_ascii_alphabetic() || b == b'_'
}
fn is_ident_cont(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

fn keyword_or_ident(word: &str) -> Tok {
    match word {
        "let" => Tok::Let,
        "fn" => Tok::Fn,
        "if" => Tok::If,
        "else" => Tok::Else,
        "while" => Tok::While,
        "for" => Tok::For,
        "return" => Tok::Return,
        "break" => Tok::Break,
        "continue" => Tok::Continue,
        "import" => Tok::Import,
        "as" => Tok::As,
        "struct" => Tok::Struct,
        "impl" => Tok::Impl,
        "enum" => Tok::Enum,
        "match" => Tok::Match,
        "true" => Tok::True,
        "false" => Tok::False,
        // NOTE: type names (int/float/bool/string/void/array) are NOT
        // keywords in the official lexer — they lex as plain identifiers
        // (std/random.lamo declares `pub fn int(...)`). The parser treats
        // them as types only in annotation position.
        _ => Tok::Ident(word.to_string()),
    }
}

pub fn lex(source: &str) -> LexOutput {
    let bytes = source.as_bytes();
    let mut pos = 0usize;
    let mut tokens = Vec::new();
    let mut errors = Vec::new();
    let mut comments = Vec::new();

    macro_rules! push {
        ($tok:expr, $start:expr) => {{
            tokens.push(Token {
                tok: $tok,
                span: Span::new($start, pos),
            });
        }};
    }

    while pos < bytes.len() {
        let b = bytes[pos];
        let start = pos;

        // whitespace
        if b == b' ' || b == b'\t' || b == b'\n' || b == b'\r' {
            pos += 1;
            continue;
        }

        // line comments (SPEC §1.2: only //; block comments are not supported)
        if b == b'/' && pos + 1 < bytes.len() && bytes[pos + 1] == b'/' {
            let comment_start = pos;
            pos += 2;
            let text_start = pos;
            while pos < bytes.len() && bytes[pos] != b'\n' && bytes[pos] != b'\r' {
                pos += 1;
            }
            comments.push(Comment {
                span: Span::new(comment_start, pos),
                text: source[text_start..pos].to_string(),
            });
            continue;
        }

        // numbers
        if b.is_ascii_digit() || (b == b'.' && pos + 1 < bytes.len() && bytes[pos + 1].is_ascii_digit())
        {
            lex_number(bytes, &mut pos, &mut tokens, &mut errors);
            continue;
        }

        // identifiers / keywords
        if is_ident_start(b) {
            pos += 1;
            while pos < bytes.len() && is_ident_cont(bytes[pos]) {
                pos += 1;
            }
            let word = &source[start..pos];
            push!(keyword_or_ident(word), start);
            continue;
        }

        // strings
        if b == b'"' {
            pos += 1; // opening quote
            let mut terminated = false;
            while pos < bytes.len() {
                let c = bytes[pos];
                if c == b'"' {
                    pos += 1;
                    terminated = true;
                    break;
                }
                if c == b'\n' || c == b'\r' {
                    // SPEC §1.4: raw newlines inside strings are a lexical error.
                    errors.push(LexError {
                        span: Span::new(start, pos),
                        message: "unterminated string literal (raw newline in string; use \\n)"
                            .into(),
                    });
                    break;
                }
                if c == b'\\' {
                    pos += 1;
                    if pos >= bytes.len() {
                        break;
                    }
                    let esc = bytes[pos];
                    match esc {
                        b'n' | b't' | b'r' | b'\\' | b'"' | b'\'' | b'0' => pos += 1,
                        b'x' => {
                            // \xNN requires exactly two hex digits (SPEC §1.4)
                            let ok = pos + 2 < bytes.len()
                                && bytes[pos + 1].is_ascii_hexdigit()
                                && bytes[pos + 2].is_ascii_hexdigit();
                            if ok {
                                pos += 3;
                            } else {
                                errors.push(LexError {
                                    span: Span::new(pos.saturating_sub(1), (pos + 3).min(bytes.len())),
                                    message: "invalid \\x escape: expected two hex digits".into(),
                                });
                                pos += 1;
                            }
                        }
                        _ => {
                            errors.push(LexError {
                                span: Span::new(pos.saturating_sub(1), pos + 1),
                                message: format!(
                                    "unknown escape sequence '\\{}' (valid: \\n \\t \\r \\\\ \\\" \\0 \\xNN)",
                                    esc as char
                                ),
                            });
                            pos += 1;
                        }
                    }
                    continue;
                }
                pos += 1;
            }
            if !terminated {
                errors.push(LexError {
                    span: Span::new(start, pos.min(bytes.len())),
                    message: "unterminated string literal".into(),
                });
            }
            // Decode escapes for the stored value (import paths need the text).
            let raw = &source[(start + 1).min(source.len())..pos.min(source.len()).saturating_sub(if terminated { 1 } else { 0 })];
            let decoded = decode_escapes(raw);
            push!(Tok::Str(decoded), start);
            continue;
        }

        // multi-char operators first, then single-char
        pos += 1;
        let next = |p: usize| if p < bytes.len() { Some(bytes[p]) } else { None };
        match b {
            b'(' => push!(Tok::LParen, start),
            b')' => push!(Tok::RParen, start),
            b'{' => push!(Tok::LBrace, start),
            b'}' => push!(Tok::RBrace, start),
            b'[' => push!(Tok::LBracket, start),
            b']' => push!(Tok::RBracket, start),
            b',' => push!(Tok::Comma, start),
            b';' => push!(Tok::Semicolon, start),
            b':' => {
                if next(pos) == Some(b':') {
                    pos += 1;
                    push!(Tok::ColonColon, start);
                } else {
                    push!(Tok::Colon, start);
                }
            }
            b'.' => push!(Tok::Dot, start),
            b'=' => {
                if next(pos) == Some(b'=') {
                    pos += 1;
                    push!(Tok::EqEq, start);
                } else if next(pos) == Some(b'>') {
                    pos += 1;
                    push!(Tok::FatArrow, start);
                } else {
                    push!(Tok::Equals, start);
                }
            }
            b'+' => {
                if next(pos) == Some(b'=') {
                    pos += 1;
                    push!(Tok::PlusEq, start);
                } else if next(pos) == Some(b'+') {
                    pos += 1;
                    push!(Tok::PlusPlus, start);
                } else {
                    push!(Tok::Plus, start);
                }
            }
            b'-' => {
                if next(pos) == Some(b'=') {
                    pos += 1;
                    push!(Tok::MinusEq, start);
                } else if next(pos) == Some(b'-') {
                    pos += 1;
                    push!(Tok::MinusMinus, start);
                } else if next(pos) == Some(b'>') {
                    pos += 1;
                    push!(Tok::Arrow, start);
                } else {
                    push!(Tok::Minus, start);
                }
            }
            b'*' => {
                if next(pos) == Some(b'=') {
                    pos += 1;
                    push!(Tok::StarEq, start);
                } else {
                    push!(Tok::Star, start);
                }
            }
            b'/' => {
                if next(pos) == Some(b'=') {
                    pos += 1;
                    push!(Tok::SlashEq, start);
                } else {
                    push!(Tok::Slash, start);
                }
            }
            b'%' => {
                if next(pos) == Some(b'=') {
                    pos += 1;
                    push!(Tok::PercentEq, start);
                } else {
                    push!(Tok::Percent, start);
                }
            }
            b'!' => {
                if next(pos) == Some(b'=') {
                    pos += 1;
                    push!(Tok::BangEq, start);
                } else {
                    push!(Tok::Bang, start);
                }
            }
            b'<' => {
                if next(pos) == Some(b'=') {
                    pos += 1;
                    push!(Tok::LtEq, start);
                } else {
                    push!(Tok::Lt, start);
                }
            }
            b'>' => {
                if next(pos) == Some(b'=') {
                    pos += 1;
                    push!(Tok::GtEq, start);
                } else {
                    push!(Tok::Gt, start);
                }
            }
            b'&' => {
                if next(pos) == Some(b'&') {
                    pos += 1;
                    push!(Tok::AndAnd, start);
                } else {
                    errors.push(LexError {
                        span: Span::new(start, pos),
                        message: "unexpected character '&' (did you mean '&&'?)".into(),
                    });
                }
            }
            b'|' => {
                if next(pos) == Some(b'|') {
                    pos += 1;
                    push!(Tok::OrOr, start);
                } else {
                    errors.push(LexError {
                        span: Span::new(start, pos),
                        message: "unexpected character '|' (did you mean '||'?)".into(),
                    });
                }
            }
            other => {
                errors.push(LexError {
                    span: Span::new(start, pos),
                    message: format!("unexpected character '{}'", other as char),
                });
            }
        }
    }

    tokens.push(Token {
        tok: Tok::Eof,
        span: Span::new(bytes.len(), bytes.len()),
    });

    LexOutput { tokens, errors, comments }
}

/// Decode string escape sequences (SPEC §1.4) into the actual value.
pub fn decode_escapes(raw: &str) -> String {
    let mut out = String::with_capacity(raw.len());
    let bytes = raw.as_bytes();
    let mut i = 0usize;
    while i < bytes.len() {
        if bytes[i] != b'\\' {
            // Copy the raw byte (works for UTF-8 continuation bytes too).
            let start = i;
            while i < bytes.len() && bytes[i] != b'\\' {
                i += 1;
            }
            out.push_str(&raw[start..i]);
            continue;
        }
        if i + 1 >= bytes.len() {
            out.push('\\');
            break;
        }
        match bytes[i + 1] {
            b'n' => {
                out.push('\n');
                i += 2;
            }
            b't' => {
                out.push('\t');
                i += 2;
            }
            b'r' => {
                out.push('\r');
                i += 2;
            }
            b'\\' => {
                out.push('\\');
                i += 2;
            }
            b'"' => {
                out.push('"');
                i += 2;
            }
            b'\'' => {
                out.push('\'');
                i += 2;
            }
            b'0' => {
                out.push('\0');
                i += 2;
            }
            b'x' => {
                if i + 3 < bytes.len()
                    && bytes[i + 2].is_ascii_hexdigit()
                    && bytes[i + 3].is_ascii_hexdigit()
                {
                    let hi = (bytes[i + 2] as char).to_digit(16).unwrap() as u32;
                    let lo = (bytes[i + 3] as char).to_digit(16).unwrap() as u32;
                    out.push(((hi << 4) | lo) as u8 as char);
                    i += 4;
                } else {
                    out.push('\\');
                    out.push('x');
                    i += 2;
                }
            }
            other => {
                out.push('\\');
                out.push(other as char);
                i += 2;
            }
        }
    }
    out
}

fn lex_number(bytes: &[u8], pos: &mut usize, tokens: &mut Vec<Token>, _errors: &mut Vec<LexError>) {
    let start = *pos;
    let mut is_float = false;

    let next = |p: usize| if p < bytes.len() { Some(bytes[p]) } else { None };

    // hex / binary
    if bytes[*pos] == b'0' && matches!(next(*pos + 1), Some(b'x') | Some(b'X')) {
        *pos += 2;
        while *pos < bytes.len() && (bytes[*pos].is_ascii_hexdigit() || bytes[*pos] == b'_') {
            *pos += 1;
        }
        tokens.push(Token {
            tok: Tok::Int(String::new()),
            span: Span::new(start, *pos),
        });
        return;
    }
    if bytes[*pos] == b'0' && matches!(next(*pos + 1), Some(b'b') | Some(b'B')) {
        *pos += 2;
        while *pos < bytes.len() && (bytes[*pos] == b'0' || bytes[*pos] == b'1' || bytes[*pos] == b'_') {
            *pos += 1;
        }
        tokens.push(Token {
            tok: Tok::Int(String::new()),
            span: Span::new(start, *pos),
        });
        return;
    }

    while *pos < bytes.len() && (bytes[*pos].is_ascii_digit() || bytes[*pos] == b'_') {
        *pos += 1;
    }
    // fraction requires a digit after the dot (mirrors the official lexer)
    if *pos < bytes.len() && bytes[*pos] == b'.' && next(*pos + 1).map_or(false, |c| c.is_ascii_digit())
    {
        is_float = true;
        *pos += 1;
        while *pos < bytes.len() && (bytes[*pos].is_ascii_digit() || bytes[*pos] == b'_') {
            *pos += 1;
        }
    }
    // exponent
    if *pos < bytes.len() && (bytes[*pos] == b'e' || bytes[*pos] == b'E') {
        let mut lookahead = *pos + 1;
        if next(lookahead) == Some(b'+') || next(lookahead) == Some(b'-') {
            lookahead += 1;
        }
        if next(lookahead).map_or(false, |c| c.is_ascii_digit()) {
            is_float = true;
            *pos = lookahead;
            while *pos < bytes.len() && (bytes[*pos].is_ascii_digit() || bytes[*pos] == b'_') {
                *pos += 1;
            }
        }
    }
    let tok = if is_float { Tok::Float(String::new()) } else { Tok::Int(String::new()) };
    tokens.push(Token {
        tok,
        span: Span::new(start, *pos),
    });
}

/// Convert a lexical error to an LSP diagnostic.
pub fn lex_error_diagnostic(err: &LexError) -> (Span, String) {
    (err.span, err.message.clone())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn kinds(src: &str) -> Vec<Tok> {
        lex(src).tokens.into_iter().map(|t| t.tok).collect()
    }

    fn norm(toks: Vec<Tok>) -> Vec<Tok> {
        toks.into_iter()
            .map(|t| match t {
                Tok::Str(_) => Tok::Str(String::new()),
                Tok::Int(_) => Tok::Int(String::new()),
                Tok::Float(_) => Tok::Float(String::new()),
                other => other,
            })
            .collect()
    }

    #[test]
    fn hello_world() {
        let toks = kinds("fn main() { print(\"Hello, World!\") }");
        assert_eq!(
            norm(toks),
            vec![
                Tok::Fn,
                Tok::Ident("main".into()),
                Tok::LParen,
                Tok::RParen,
                Tok::LBrace,
                Tok::Ident("print".into()),
                Tok::LParen,
                Tok::Str(String::new()),
                Tok::RParen,
                Tok::RBrace,
                Tok::Eof,
            ]
        );
    }

    #[test]
    fn keywords() {
        let toks = kinds("let fn if else while for return break continue import as struct impl enum match true false");
        let expected = vec![
            Tok::Let, Tok::Fn, Tok::If, Tok::Else, Tok::While, Tok::For,
            Tok::Return, Tok::Break, Tok::Continue, Tok::Import, Tok::As,
            Tok::Struct, Tok::Impl, Tok::Enum, Tok::Match, Tok::True, Tok::False,
            Tok::Eof,
        ];
        assert_eq!(toks, expected);
    }

    #[test]
    fn type_names_lex_as_identifiers() {
        // Matches the official lexer: `fn int(...)` in std/random.lamo.
        let toks = kinds("fn int(lo, hi)");
        assert_eq!(toks[0], Tok::Fn);
        assert_eq!(toks[1], Tok::Ident("int".into()));
    }

    #[test]
    fn numbers() {
        let toks = kinds("42 0xFF 0b1010 1_000_000 3.14 0.5 1e10 2.5E-3");
        assert_eq!(
            norm(toks),
            vec![
                Tok::Int(String::new()), Tok::Int(String::new()), Tok::Int(String::new()),
                Tok::Int(String::new()), Tok::Float(String::new()), Tok::Float(String::new()),
                Tok::Float(String::new()), Tok::Float(String::new()), Tok::Eof
            ]
        );
    }

    #[test]
    fn operators() {
        let toks = kinds("= + - * / % ! < > == != <= >= && || += -= ++ -- -> => *= /= %= :: , ; : .");
        assert_eq!(
            toks,
            vec![
                Tok::Equals, Tok::Plus, Tok::Minus, Tok::Star, Tok::Slash, Tok::Percent,
                Tok::Bang, Tok::Lt, Tok::Gt, Tok::EqEq, Tok::BangEq, Tok::LtEq, Tok::GtEq,
                Tok::AndAnd, Tok::OrOr, Tok::PlusEq, Tok::MinusEq, Tok::PlusPlus, Tok::MinusMinus,
                Tok::Arrow, Tok::FatArrow, Tok::StarEq, Tok::SlashEq, Tok::PercentEq,
                Tok::ColonColon, Tok::Comma, Tok::Semicolon, Tok::Colon, Tok::Dot, Tok::Eof
            ]
        );
    }

    #[test]
    fn comments_are_skipped() {
        let toks = kinds("// line comment\nlet x = 1 // trailing");
        assert_eq!(norm(toks), vec![Tok::Let, Tok::Ident("x".into()), Tok::Equals, Tok::Int(String::new()), Tok::Eof]);
    }

    #[test]
    fn unterminated_string_error() {
        let out = lex("let s = \"abc");
        assert_eq!(out.errors.len(), 1);
        assert!(out.errors[0].message.contains("unterminated"));
    }

    #[test]
    fn newline_in_string_error() {
        let out = lex("\"ab\ncd\"");
        assert!(out.errors.iter().any(|e| e.message.contains("raw newline")));
    }

    #[test]
    fn bad_escape_error() {
        let out = lex("\"a\\qb\"");
        assert!(out.errors.iter().any(|e| e.message.contains("unknown escape")));
    }

    #[test]
    fn lone_ampersand_error() {
        let out = lex("a & b");
        assert!(out.errors.iter().any(|e| e.message.contains('&')));
    }

    #[test]
    fn spans_are_byte_offsets() {
        let out = lex("let x = 5");
        assert_eq!(out.tokens[0].span, Span::new(0, 3));
        assert_eq!(out.tokens[1].span, Span::new(4, 5));
        assert_eq!(out.tokens[3].span, Span::new(8, 9));
        assert_eq!(out.tokens[4].tok, Tok::Eof);
    }

    #[test]
    fn string_value_decoded() {
        let out = lex("\"a\\n\\x41b\"");
        match &out.tokens[0].tok {
            Tok::Str(s) => assert_eq!(s, "a\nAb"),
            other => panic!("expected string, got {other:?}"),
        }
    }

    #[test]
    fn import_path_string_roundtrip() {
        let out = lex("import \"utils.lamo\" as utils");
        match &out.tokens[1].tok {
            Tok::Str(s) => assert_eq!(s, "utils.lamo"),
            other => panic!("expected string, got {other:?}"),
        }
    }

    #[test]
    fn float_requires_digit_after_dot() {
        // `1.` lexes as INT then DOT (method-call shape), like the official compiler
        let toks = norm(kinds("1.foo"));
        assert_eq!(toks[0], Tok::Int(String::new()));
        assert_eq!(toks[1], Tok::Dot);
    }
}
