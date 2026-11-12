# vcpkg overlay ports

`minizip-ng/` and `zlib/` are copied unchanged from duckdb/duckdb-excel at
`81bb84b115b97cc4edec4827714cac80da0127e1` (`vcpkg_ports/`, added there by e1964ba,
"add overlay ports to fix wasm"). duckdb-excel is MIT, Copyright Stichting DuckDB
Foundation.

They are copied, not re-derived, so that panduck and excel build the SAME minizip-ng
(4.0.7, `default-features: false`, feature `zlib`). An application that links both
extensions statically (the duckeye Android engine does) then gets one zip library,
not two. Before this, panduck used miniz, whose `mz_zip_writer_add_file` clashed
with minizip-ng's in that link (`ld.lld: error: duplicate symbol`).

When excel moves its pin, move these with it. The pinned vcpkg commit
(84bab45) has minizip-ng 4.0.10, so without this overlay the two would differ.
