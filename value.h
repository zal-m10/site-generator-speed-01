// Shared value type and JS-semantics helpers for the native site-generator entry.
// Strings are UTF-8. JS string order is by UTF-16 code units; see compareCodeUnits.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct Value {
  enum Type : uint8_t { kNull, kBool, kNumber, kString, kArray, kObject, kUndefined };
  Type type = kNull;
  bool b = false;
  double num = 0;
  std::string s;
  std::vector<Value> arr;
  // Object: insertion-ordered key/value pairs (JS object order for our keys,
  // which are never integer-like, is insertion order).
  std::vector<std::pair<std::string, Value>> obj;

  static Value nullv() { return Value(); }
  static Value boolean(bool v) { Value x; x.type = kBool; x.b = v; return x; }
  static Value number(double v) { Value x; x.type = kNumber; x.num = v; return x; }
  static Value str(std::string v) { Value x; x.type = kString; x.s = std::move(v); return x; }
  static Value array() { Value x; x.type = kArray; return x; }
  static Value object() { Value x; x.type = kObject; return x; }
  static Value undef() { Value x; x.type = kUndefined; return x; }

  bool isNull() const { return type == kNull; }
  bool isBool() const { return type == kBool; }
  bool isNumber() const { return type == kNumber; }
  bool isString() const { return type == kString; }
  bool isArray() const { return type == kArray; }
  bool isObject() const { return type == kObject; }
  bool isUndefined() const { return type == kUndefined; }

  // Object lookup by key (own properties only). nullptr when absent or not an object.
  const Value* find(const std::string& key) const;
  Value* find(const std::string& key);
  bool has(const std::string& key) const { return find(key) != nullptr; }
  // Object set: replace the existing pair or append (insertion order preserved).
  void set(const std::string& key, Value v);
};

// JavaScript String(number): shortest round-trip digits, JS exponent rules.
// Handles NaN, +/-Infinity, -0 (all print as JS does).
std::string jsNumber(double v);

// Compare two UTF-8 strings the way JS `<` compares strings: by UTF-16 code
// units. Returns <0, 0, >0. (UTF-8 byte order differs from UTF-16 order only
// where non-BMP characters meet BMP characters >= U+E000.)
int compareCodeUnits(const std::string& a, const std::string& b);

// Decode one UTF-8 code point starting at byte offset i; advances i past it.
// Invalid bytes are treated as U+FFFD (one byte consumed). Never throws.
uint32_t utf8Decode(const std::string& s, size_t& i);
// Append the UTF-8 encoding of cp.
void utf8Encode(std::string& out, uint32_t cp);
// Number of UTF-16 code units in the UTF-8 string s.
size_t utf16Length(const std::string& s);

// True when the UTF-8 string holds any Unicode White_Space code point
// (JS /\s/ test: 0x09-0x0D, 0x20, 0x85, 0xA0, 0x1680, 0x2000-0x200A,
// 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF).
bool hasWhitespace(const std::string& s);
// JS String.prototype.trim: strip White_Space and line terminators at both ends.
std::string jsTrim(const std::string& s);
// True when cp is a Unicode White_Space code point.
bool isWhiteSpace(uint32_t cp);

// NFKD normalisation callback (implemented by the caller via JS
// String.prototype.normalize, which is exactly the sandbox's Node ICU).
// Input and output are UTF-8.
using NormalizeFn = std::function<std::string(const std::string&)>;
