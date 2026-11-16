#pragma once

#include <string>

namespace panduck {

//! EMBEDDED DATA AS A TABLE (#188). A document often carries data as a fenced code
//! block: ```csv, ```tsv, ```json, ```jsonl. This turns that text into the table
//! content every panduck reader emits for a table, {"headers":[...],"rows":[[...]]},
//! with every cell a string (block_json.hpp BuildTableJson).
//!
//! Returns true and sets `content`, or returns false and sets `error` to a reason a
//! person can act on. It never throws, so one bad block cannot fail a document.
//!
//!   csv, tsv  RFC 4180: a quoted field keeps its delimiter and its line breaks, a
//!             doubled quote is one quote, CRLF and LF both end a record, and a blank
//!             line outside quotes is not a record. The first record is the header.
//!             A data row with a different field count is an error, not a guess.
//!   json      an array of objects. Headers are every key in first-seen order. A
//!             missing key or a null is an empty cell; a number keeps its literal
//!             text ("1.50"); a nested array or object is compact JSON.
//!   jsonl     one object per line; blank lines are skipped.
//!
//! THE PADDING IS BOUNDED. N objects with disjoint keys make an N x N table: in
//! duck_block_utils, 47 KB of input gave 48 MB of content and 620 MB peak RSS. A
//! conversion that would add more than max(1,000,000, present cells) empty cells is
//! refused with an error.
//!
//! PORTED from duck_block_utils' closed PR #84 (src/parse_embedded.cpp, Convert), with
//! the same rules and the same error messages. Its lenient JSON parser is replaced by
//! yyjson, which libpanduck already uses for ipynb and which is strict on its own.
//! `format` ignores case.
bool EmbeddedToTable(const std::string &text, const std::string &format, std::string &content, std::string &error);

} // namespace panduck
