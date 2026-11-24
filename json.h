// Fast JSON parser/serializer matching JS JSON.parse / JSON.stringify semantics.
// Input is trusted well-formed JSON from the harness generator. Strings are UTF-8;
// \u escapes (including surrogate pairs) are decoded to UTF-8 on parse.
// Serialization escapes exactly what JS JSON.stringify escapes: '"' '\\' and
// control characters < 0x20 (\b \f \n \r \t short forms, else \u00XX). Non-ASCII
// (including U+2028/U+2029) is written raw, as Node 22 does.
#pragma once
#include <string>
#include "value.h"

// Parse one JSON document. Throws std::runtime_error on malformed input.
Value jsonParse(const std::string& text);
// Parse one JSON value starting at text[pos] (leading whitespace skipped).
// On return pos is just past the value. Throws std::runtime_error on
// malformed input. Lets a caller stream values out of a larger buffer
// without parsing the whole document at once.
Value jsonParseOne(const std::string& text, size_t& pos);
// Skip JSON whitespace starting at text[pos]; returns the new position.
size_t jsonSkipWs(const std::string& text, size_t pos);
// Serialize exactly as JS JSON.stringify would (insertion order for objects).
// Undefined object members are skipped; undefined array elements become null.
std::string jsonStringify(const Value& v);
// The JSON string literal for s, quotes included, JS escaping rules.
std::string jsonEscape(const std::string& s);
