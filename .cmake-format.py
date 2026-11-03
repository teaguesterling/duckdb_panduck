# cmake-format configuration.
#
# enable_markup is OFF deliberately. With it on, cmake-format treats comment bodies as
# markup and REFLOWS them -- which joined a two-line list of dependencies ("pugixml -- ...",
# "miniz -- ...") into a single paragraph, destroying the structure that made the comment
# readable. The comments in CMakeLists.txt carry measured reasons, not decoration, and a
# formatter is not entitled to rewrite prose it does not understand.
#
# Layout of the CMake code itself is still checked and enforced.
#
# A .py file, not YAML: DuckDB v2.0.0's format venv has no PyYAML, and cmake-format then
# fails on a YAML config with ModuleNotFoundError (measured on panduck PR #161).
enable_markup = False
line_width = 100
