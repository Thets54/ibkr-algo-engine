# Turns INPUT (the dashboard HTML) into a C++ header with a string_view.
# Usage: cmake -DINPUT=... -DOUTPUT=... -P EmbedFile.cmake
file(READ "${INPUT}" _content HEX)
string(LENGTH "${_content}" _len)
math(EXPR _bytes "${_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1'," _array "${_content}")
get_filename_component(_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
file(WRITE "${OUTPUT}"
"// Generated from web/index.html by cmake/EmbedFile.cmake. Do not edit.\n"
"#pragma once\n#include <string_view>\n"
"namespace algo::web {\n"
"inline constexpr char kIndexHtmlData[] = {${_array} '\\0'};\n"
"inline constexpr std::string_view kIndexHtml{kIndexHtmlData, ${_bytes}};\n"
"}  // namespace algo::web\n")
