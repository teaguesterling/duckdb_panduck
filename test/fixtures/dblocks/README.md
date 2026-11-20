# Compact-format golden corpus

Files that every implementation of the compact line format must agree on: duck_block_utils'
SQL macros (`src/doc_macros.cpp`) and the C++ codec in `vendor/duck_block_compact.hpp`, as
vendored by duckdb_panduck. duck_block_utils itself never calls the C++ codec, so this
corpus is what ties the two together.

| File | What an implementation must do with it |
|---|---|
| `<name>.dblocks` + `<name>.expected.jsonl` | Read the `.dblocks` to exactly the rows in `.expected.jsonl`; write those rows back to the `.dblocks` byte for byte |
| `read_only_unsorted_keys.dblocks` + `.expected.jsonl` | Read only: its maps were not key-sorted when written, and the reader returns keys sorted, so the rows do not write back to these bytes |
| `concat.dblocks` | Read two concatenated documents (a second header, an empty line) as one: `0:one 1:two` |
| `error_type_code.dblocks` | Refuse: `unknown type code` |
| `error_attribute_code.dblocks` | Refuse: `unknown attribute code` |
| `error_missing_level.dblocks` | Refuse: `missing level` |

`hard` holds the cases two encoders most easily disagree on: a tab, a newline, `"`, `\`,
`\x01` (written `\u0001`), non-ASCII text, an emoji and U+2028 (written raw); NULL against
`""` content; the `kind:element_type` escape, including an element_type with a colon; all
21 attribute codes; unknown keys, one of them literally `c`; the `json` and `yaml`
encodings. `read_only_unsorted_keys` holds maps whose keys were not sorted: the writer kept
the map's order, and the reader returns them sorted. `every_type` holds every type in the spec 1.4 vocabulary. The other
three are measured duckdb_panduck reader output.

The `.dblocks` files were written by `duck_blocks_to_compact`, from the rows in their
`.expected.jsonl`; `compact.test` checks the writer still produces them byte for byte.
