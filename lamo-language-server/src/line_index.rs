//! UTF-8 <-> LSP position mapping.
//!
//! LSP positions are `(line, character)` pairs where `character` counts
//! UTF-16 code units. Lamo identifiers are ASCII (see SPEC §1.5) but string
//! literals may contain arbitrary UTF-8, so we do proper UTF-16 counting.

use tower_lsp::lsp_types::Position;

/// A precomputed index over a document: one entry per line.
#[derive(Debug, Clone)]
pub struct LineIndex {
    /// Byte offset of the first character of each line.
    line_starts: Vec<usize>,
    /// Byte offset of the line terminator (i.e. end of line content).
    line_content_ends: Vec<usize>,
    /// UTF-16 length of each line, excluding the line terminator.
    line_utf16_len: Vec<usize>,
    text_len: usize,
    text: String,
}

impl LineIndex {
    pub fn new(text: &str) -> Self {
        let bytes = text.as_bytes();
        let text_len = bytes.len();
        let mut line_starts = vec![0usize];
        let mut line_content_ends = Vec::new();
        let mut line_utf16_len = Vec::new();
        let mut i = 0usize;
        let mut utf16 = 0usize;
        while i < text_len {
            match bytes[i] {
                b'\n' => {
                    line_content_ends.push(i);
                    line_utf16_len.push(utf16);
                    utf16 = 0;
                    i += 1;
                    line_starts.push(i);
                }
                b'\r' => {
                    line_content_ends.push(i);
                    line_utf16_len.push(utf16);
                    utf16 = 0;
                    i += 1;
                    if i < text_len && bytes[i] == b'\n' {
                        i += 1;
                    }
                    line_starts.push(i);
                }
                b => {
                    let len = match b {
                        _ if b < 0x80 => 1,
                        _ if b >> 5 == 0b110 => 2,
                        _ if b >> 4 == 0b1110 => 3,
                        _ if b >> 3 == 0b11110 => 4,
                        _ => 1,
                    };
                    let end = (i + len).min(text_len);
                    if let Some(ch) = std::str::from_utf8(&bytes[i..end])
                        .ok()
                        .and_then(|s| s.chars().next())
                    {
                        utf16 += ch.len_utf16();
                        i += ch.len_utf8();
                    } else {
                        utf16 += 1;
                        i += 1;
                    }
                }
            }
        }
        line_content_ends.push(text_len);
        line_utf16_len.push(utf16);
        Self {
            line_starts,
            line_content_ends,
            line_utf16_len,
            text_len,
            text: text.to_string(),
        }
    }

    fn bytes(&self) -> &[u8] {
        self.text.as_bytes()
    }

    pub fn line_count(&self) -> usize {
        self.line_starts.len()
    }

    /// Convert an LSP position to a byte offset. Clamps out-of-range input.
    pub fn offset(&self, pos: Position) -> usize {
        let line = pos.line as usize;
        if line >= self.line_starts.len() {
            return self.text_len;
        }
        let start = self.line_starts[line];
        let line_end = self.line_content_ends[line];
        let target = pos.character as usize;
        if target >= self.line_utf16_len[line] {
            return line_end;
        }
        let bytes = self.bytes();
        let _ = bytes;
        let mut utf16 = 0usize;
        let mut i = start;
        for ch in self.text[start..line_end].chars() {
            if utf16 + ch.len_utf16() > target {
                break;
            }
            utf16 += ch.len_utf16();
            i += ch.len_utf8();
        }
        i
    }

    /// Convert a byte offset to an LSP position.
    pub fn position(&self, offset: usize) -> Position {
        let offset = offset.min(self.text_len);
        let line = match self.line_starts.binary_search(&offset) {
            Ok(l) => l,
            Err(next) => next.saturating_sub(1),
        };
        let start = self.line_starts[line];
        let mut utf16 = 0usize;
        for ch in self.text[start..offset].chars() {
            utf16 += ch.len_utf16();
        }
        Position::new(line as u32, utf16 as u32)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ascii_roundtrip() {
        let idx = LineIndex::new("hello\nworld\n");
        let off = idx.offset(Position::new(1, 3));
        assert_eq!(off, 9); // "hello\n" = 6 bytes + 3
        let pos = idx.position(off);
        assert_eq!(pos, Position::new(1, 3));
    }

    #[test]
    fn crlf_handling() {
        let idx = LineIndex::new("ab\r\ncd");
        assert_eq!(idx.offset(Position::new(1, 1)), 5);
        assert_eq!(idx.position(5), Position::new(1, 1));
    }

    #[test]
    fn utf16_columns() {
        // "á" is 2 bytes in UTF-8, 1 UTF-16 unit.
        let idx = LineIndex::new("\"áx\"");
        // Position (0, 3) = after 'á' and 'x' -> byte offset 4.
        assert_eq!(idx.offset(Position::new(0, 3)), 4);
        assert_eq!(idx.position(4), Position::new(0, 3));
    }

    #[test]
    fn emoji_two_units() {
        // "🎉" is 4 bytes, 2 UTF-16 units.
        let idx = LineIndex::new("a🎉b");
        assert_eq!(idx.offset(Position::new(0, 3)), 5);
        assert_eq!(idx.position(5), Position::new(0, 3));
    }

    #[test]
    fn out_of_range_clamps() {
        let idx = LineIndex::new("ab");
        assert_eq!(idx.offset(Position::new(5, 5)), 2);
        assert_eq!(idx.position(100), Position::new(0, 2));
    }
}
