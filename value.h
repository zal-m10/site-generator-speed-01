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

// ---------------------------------------------------------------------------
// Delimiter scanning with SSE2 (x86_64) and a scalar fallback.
//
// The hot loops scan long stretches of mostly-plain text for a small set of
// delimiter bytes (HTML escapes, Markdown inline markers, JSON string
// boundaries). Each helper returns the offset of the first delimiter byte in
// [p, p+n), or n when the range holds none. The SSE2 path compares 16 bytes
// at a time and finds the first set bit of the match mask; the tail (<16
// bytes) and non-x86_64 builds use the scalar loop, so the result is
// identical on every platform.
#if defined(__x86_64__)
#include <immintrin.h>
#endif

// First byte in {&, <, >, ", '} (the five HTML escapes), or n.
inline size_t findHtmlSpecial(const char* p, size_t n) {
#if defined(__x86_64__)
  size_t i = 0;
  const __m128i amp = _mm_set1_epi8('&');
  const __m128i lt = _mm_set1_epi8('<');
  const __m128i gt = _mm_set1_epi8('>');
  const __m128i quot = _mm_set1_epi8('"');
  const __m128i apos = _mm_set1_epi8('\'');
  while (i + 16 <= n) {
    __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
    __m128i m = _mm_cmpeq_epi8(x, amp);
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, lt));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, gt));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, quot));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, apos));
    unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(m));
    if (bits != 0) return i + static_cast<size_t>(__builtin_ctz(bits));
    i += 16;
  }
  for (; i < n; i++) {
    char c = p[i];
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return i;
  }
  return n;
#else
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return i;
  }
  return n;
#endif
}

// First byte in {\, `, *, [, <, >, &, ", '} (Markdown inline delimiters), or n.
inline size_t findInlineSpecial(const char* p, size_t n) {
#if defined(__x86_64__)
  size_t i = 0;
  const __m128i bs = _mm_set1_epi8('\\');
  const __m128i tick = _mm_set1_epi8('`');
  const __m128i star = _mm_set1_epi8('*');
  const __m128i lbrack = _mm_set1_epi8('[');
  const __m128i lt = _mm_set1_epi8('<');
  const __m128i gt = _mm_set1_epi8('>');
  const __m128i amp = _mm_set1_epi8('&');
  const __m128i quot = _mm_set1_epi8('"');
  const __m128i apos = _mm_set1_epi8('\'');
  while (i + 16 <= n) {
    __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
    __m128i m = _mm_cmpeq_epi8(x, bs);
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, tick));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, star));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, lbrack));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, lt));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, gt));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, amp));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, quot));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, apos));
    unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(m));
    if (bits != 0) return i + static_cast<size_t>(__builtin_ctz(bits));
    i += 16;
  }
  for (; i < n; i++) {
    char c = p[i];
    if (c == '\\' || c == '`' || c == '*' || c == '[' || c == '<' || c == '>' ||
        c == '&' || c == '"' || c == '\'')
      return i;
  }
  return n;
#else
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    if (c == '\\' || c == '`' || c == '*' || c == '[' || c == '<' || c == '>' ||
        c == '&' || c == '"' || c == '\'')
      return i;
  }
  return n;
#endif
}

// First byte in {", \} (JSON string end or escape), or n.
inline size_t findJsonStrSpecial(const char* p, size_t n) {
#if defined(__x86_64__)
  size_t i = 0;
  const __m128i quot = _mm_set1_epi8('"');
  const __m128i bs = _mm_set1_epi8('\\');
  while (i + 16 <= n) {
    __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(x, quot), _mm_cmpeq_epi8(x, bs));
    unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(m));
    if (bits != 0) return i + static_cast<size_t>(__builtin_ctz(bits));
    i += 16;
  }
  for (; i < n; i++) {
    if (p[i] == '"' || p[i] == '\\') return i;
  }
  return n;
#else
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '"' || p[i] == '\\') return i;
  }
  return n;
#endif
}

// First byte that needs JSON escaping: {", \, any byte < 0x20}, or n.
// A byte is < 0x20 exactly when (b & 0xE0) == 0. This matches the escaping
// our serializer already does; lone-surrogate bytes (0xED) are not escaped
// because our parser normalises lone surrogates to U+FFFD at parse time, so
// they never reach the serializer.
inline size_t findJsonQuoteSpecial(const char* p, size_t n) {
#if defined(__x86_64__)
  size_t i = 0;
  const __m128i quot = _mm_set1_epi8('"');
  const __m128i bs = _mm_set1_epi8('\\');
  const __m128i hi = _mm_set1_epi8(static_cast<char>(0xE0));
  const __m128i zero = _mm_setzero_si128();
  while (i + 16 <= n) {
    __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(x, quot), _mm_cmpeq_epi8(x, bs));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(_mm_and_si128(x, hi), zero));
    unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(m));
    if (bits != 0) return i + static_cast<size_t>(__builtin_ctz(bits));
    i += 16;
  }
  for (; i < n; i++) {
    unsigned char c = static_cast<unsigned char>(p[i]);
    if (c == '"' || c == '\\' || c < 0x20) return i;
  }
  return n;
#else
  for (size_t i = 0; i < n; i++) {
    unsigned char c = static_cast<unsigned char>(p[i]);
    if (c == '"' || c == '\\' || c < 0x20) return i;
  }
  return n;
#endif
}

// NFKD normalisation callback (implemented by the caller via JS
// String.prototype.normalize, which is exactly the sandbox's Node ICU).
// Input and output are UTF-8.
using NormalizeFn = std::function<std::string(const std::string&)>;
