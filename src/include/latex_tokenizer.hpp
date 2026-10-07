#pragma once

#include "panduck/latex_tokenizer.hpp"

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

namespace latex {

// THE LEXER LIVES IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/latex_tokenizer.hpp and
// libpanduck/src/latex_tokenizer.cpp, which name no DuckDB type. Only
// panduck_latex_tokens(), its bind, its scan and its registration stay on this
// side -- see src/latex_tokenizer.cpp, which keeps the filename so the pair
// still reads as one subject.
//
// `enum class TokenKind` IS THE TRAP-2 SHAPE. The first textile_scanner shim
// re-exported the structs and functions but not its `enum class LineKind`,
// because the grep used to enumerate the moved header's surface matched only
// `^std::|^struct` -- 26 build errors reading "'LineKind' has not been
// declared". Enumerate with a pattern covering enum/using/constexpr/class as
// well as struct and function signatures. This header exports FOUR names: the
// enum, the struct, the entry point, and the two free helpers.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers in panduck_duckdb_compat.hpp, not to
// the library. Alphabetised because clang-format requires it.
using ::panduck::latex::KindName;
using ::panduck::latex::Token;
using ::panduck::latex::Tokenize;
using ::panduck::latex::TokenKind;
using ::panduck::latex::Utf8SequenceLength;

} // namespace latex

void RegisterLatexTokensFunction(ExtensionLoader &loader);

} // namespace duckdb
