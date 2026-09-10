//! Lightweight Lamo type model for the analyzer (SPEC §7).

use crate::ast::{TypeAnn, TypeKind};
use crate::lexer::Span;

/// Primitive type names usable in annotations (SPEC §1.6/§7.2).
pub const BUILTIN_TYPE_NAMES: &[&str] = &[
    "int", "float", "bool", "string", "void", "array",
];

#[derive(Debug, Clone, PartialEq)]
pub enum LType {
    Int,
    Float,
    Bool,
    String,
    Void,
    /// `array<T>` — `None` element means the deprecated bare `array` form.
    Array(Option<Box<LType>>),
    /// Struct or enum by name.
    Named(String),
    /// Generic type parameter in scope (`T`).
    TypeParam(String),
    /// Unknown — used when nothing can be inferred (no diagnostics reported
    /// against unknown).
    Unknown,
}

impl LType {
    pub fn name(&self) -> String {
        match self {
            LType::Int => "int".into(),
            LType::Float => "float".into(),
            LType::Bool => "bool".into(),
            LType::String => "string".into(),
            LType::Void => "void".into(),
            LType::Array(Some(t)) => format!("array<{}>", t.name()),
            LType::Array(None) => "array".into(),
            LType::Named(n) => n.clone(),
            LType::TypeParam(n) => n.clone(),
            LType::Unknown => "unknown".into(),
        }
    }

    pub fn is_numeric(&self) -> bool {
        matches!(self, LType::Int | LType::Float)
    }

    pub fn is_known(&self) -> bool {
        !matches!(self, LType::Unknown)
    }

    /// Is `self` assignable to `target`? Numeric widening int -> float.
    /// Unknowns are compatible (no diagnostics without confidence).
    pub fn compatible_with(&self, target: &LType) -> bool {
        match (self, target) {
            (LType::Unknown, _) | (_, LType::Unknown) => true,
            // Type parameters bind to any concrete argument at call sites.
            (LType::TypeParam(_), _) | (_, LType::TypeParam(_)) => true,
            (a, b) if a == b => true,
            (LType::Int, LType::Float) => true,
            // bool is a C int (0/1) at runtime — bool/int comparisons are legal
            (LType::Bool, LType::Int) | (LType::Int, LType::Bool) => true,
            // `array<any>` (deprecated bare form) accepts everything
            (LType::Array(_), LType::Array(None)) => true,
            (LType::Array(None), LType::Array(_)) => true,
            (LType::Array(a), LType::Array(b)) => match (a, b) {
                (None, _) | (_, None) => true,
                (Some(x), Some(y)) => x.compatible_with(y),
            },
            _ => false,
        }
    }

    /// Result type of a binary arithmetic/comparison operation.
    pub fn binary_result(op: &crate::ast::BinOp, lhs: &LType, rhs: &LType) -> LType {
        use crate::ast::BinOp::*;
        match op {
            Add | Sub | Mul | Div | Mod => {
                if lhs == &LType::String && rhs == &LType::String && op == &Add {
                    LType::String
                } else if lhs.is_numeric() && rhs.is_numeric() {
                    if lhs == &LType::Float || rhs == &LType::Float {
                        LType::Float
                    } else {
                        LType::Int
                    }
                } else {
                    LType::Unknown
                }
            }
            Eq | Ne | Lt | Le | Gt | Ge => {
                if lhs.is_known() && rhs.is_known() {
                    // SPEC §7.3: mixed-type comparison (int == string) is an error
                    let ok = match (lhs, rhs) {
                        (a, b) if a.compatible_with(b) || b.compatible_with(a) => true,
                        _ => false,
                    };
                    if ok {
                        LType::Bool
                    } else {
                        LType::Unknown
                    }
                } else {
                    LType::Bool
                }
            }
            And | Or => {
                // Result type is the operand type per SPEC §7.3
                if lhs.is_known() {
                    lhs.clone()
                } else {
                    rhs.clone()
                }
            }
        }
    }
}

/// Parse an AST type annotation into the LType model.
pub fn resolve_type_ann(ann: &TypeAnn) -> LType {
    match &ann.kind {
        TypeKind::Int => LType::Int,
        TypeKind::Float => LType::Float,
        TypeKind::Bool => LType::Bool,
        TypeKind::String => LType::String,
        TypeKind::Void => LType::Void,
        TypeKind::Array(elem) => {
            LType::Array(elem.as_ref().map(|e| Box::new(resolve_type_ann(e))))
        }
        TypeKind::Named(name, _) => LType::Named(name.clone()),
    }
}

/// Rewrite a resolved type so that references to the given generic parameter
/// names become `LType::TypeParam` (bindable to any concrete argument).
/// Used when indexing generic struct/enum members, where a field annotation
/// like `value: T` must not be confused with a struct named `T`.
pub fn bind_type_params(ty: &LType, params: &[String]) -> LType {
    match ty {
        LType::Named(n) if params.iter().any(|p| p == n) => LType::TypeParam(n.clone()),
        LType::Array(Some(inner)) => {
            LType::Array(Some(Box::new(bind_type_params(inner, params))))
        }
        other => other.clone(),
    }
}

/// A type annotation's head name + span (for "unknown type" diagnostics).
pub fn ann_head(ann: &TypeAnn) -> Option<(&str, Span)> {
    match &ann.kind {
        TypeKind::Named(name, _) => Some((name.as_str(), ann.span)),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numeric_widening() {
        assert!(LType::Int.compatible_with(&LType::Float));
        assert!(!LType::Float.compatible_with(&LType::Int));
        assert!(LType::Int.compatible_with(&LType::Int));
    }

    #[test]
    fn unknown_is_compatible() {
        assert!(LType::Unknown.compatible_with(&LType::Int));
        assert!(LType::String.compatible_with(&LType::Unknown));
    }

    #[test]
    fn string_concat() {
        use crate::ast::BinOp;
        let t = LType::binary_result(&BinOp::Add, &LType::String, &LType::String);
        assert_eq!(t, LType::String);
        let t = LType::binary_result(&BinOp::Add, &LType::Int, &LType::Float);
        assert_eq!(t, LType::Float);
        let t = LType::binary_result(&BinOp::Add, &LType::String, &LType::Int);
        assert_eq!(t, LType::Unknown);
    }

    #[test]
    fn array_types() {
        let arr = LType::Array(Some(Box::new(LType::Int)));
        let bare = LType::Array(None);
        assert!(arr.compatible_with(&bare));
        assert!(bare.compatible_with(&arr));
    }
}
