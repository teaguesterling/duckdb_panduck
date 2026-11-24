#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! Registers the `panduck` storage type: ATTACH '<doc>' AS r (TYPE panduck) makes a
//! read-only catalog of views over one document. Spec:
//! docs/superpowers/specs/2026-11-23-attach-documents-design.md
void RegisterAttachStorage(ExtensionLoader &loader);

} // namespace duckdb
