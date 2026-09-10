//! Go-to-definition.

use super::resolve::{definition_of, resolve_at};
use crate::analyzer::Analysis;
use crate::line_index::LineIndex;
use tower_lsp::lsp_types::{Location, Position, Url};

pub fn definition(
    analysis: &Analysis,
    line_index: &LineIndex,
    offset: usize,
) -> Option<Location> {
    let (_, _, target) = resolve_at(analysis, offset)?;
    let (uri, span) = definition_of(analysis, &target)?;
    let target_index = if uri.scheme() == crate::stdlib::STD_SCHEME {
        stdlib_line_index(&uri)
    } else {
        None
    };
    let index = target_index.unwrap_or_else(|| line_index.clone());
    Some(Location {
        uri,
        range: tower_lsp::lsp_types::Range::new(
            index.position(span.start),
            index.position(span.end),
        ),
    })
}

fn stdlib_line_index(uri: &Url) -> Option<LineIndex> {
    let module = crate::stdlib::module_from_uri(uri)?;
    crate::stdlib::std_line_index(&module)
}

/// Position helper used by tests.
#[allow(dead_code)]
pub fn pos_of(index: &LineIndex, offset: usize) -> Position {
    index.position(offset)
}
