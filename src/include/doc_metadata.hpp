#pragma once

#include "panduck/doc_metadata.hpp"

namespace duckdb {

// THE METADATA FIELD TABLES AND HELPERS LIVE IN libpanduck (issue #104, L2):
// libpanduck/include/panduck/doc_metadata.hpp. Header-only there as here, so
// there is no .cpp and nothing was added to either CMake source list for it.
//
// ENUMERATE WITH A PATTERN THAT COVERS enum/using/constexpr, not just struct
// and function -- the first textile scanner shim missed `enum class LineKind`
// and the build failed 26 times. This header's surface is one struct, two
// constexpr arrays, one function and one function template; there is no enum.
//
// Leading `::` is mandatory: inside `namespace duckdb` a bare `panduck::` binds
// to `duckdb::panduck`, the compat helpers, not the library.
using ::panduck::DOCX_CORE_FIELDS;
using ::panduck::DropDuplicatedMetadataParagraphs;
using ::panduck::MetadataField;
using ::panduck::ODT_META_FIELDS;
using ::panduck::TrimMetaText;

} // namespace duckdb
