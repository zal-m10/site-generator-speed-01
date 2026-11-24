// JS-semantics helpers. See value.h.
#include "value.h"
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

const Value* Value::find(const std::string& key) const {
  if (type != kObject) return nullptr;
  for (const auto& kv : obj) if (kv.first == key) return &kv.second;
  return nullptr;
}
Value* Value::find(const std::string& key) {
  if (type != kObject) return nullptr;
  for (auto& kv : obj) if (kv.first == key) return &kv.second;
  return nullptr;
}
void Value::set(const std::string& key, Value v) {
  if (type != kObject) return;
  for (auto& kv : obj) if (kv.first == key) { kv.second = std::move(v); return; }
  obj.emplace_back(key, std::move(v));
}

uint32_t utf8Decode(const std::string& s, size_t& i) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  size_t n = s.size();
  if (i >= n) return 0xFFFD;
  unsigned char c = p[i];
  if (c < 0x80) { i += 1; return c; }
  auto cont = [&](size_t k) { return k < n && (p[k] & 0xC0) == 0x80; };
  if (c >= 0xC2 && c <= 0xDF && cont(i + 1)) {
    uint32_t cp = ((c & 0x1F) << 6) | (p[i + 1] & 0x3F);
    i += 2; return cp;
  }
  if (c >= 0xE0 && c <= 0xEF && cont(i + 1) && cont(i + 2)) {
    uint32_t cp = ((c & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
    if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) { i += 3; return cp; }
  }
  if (c >= 0xF0 && c <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
    uint32_t cp = ((c & 0x07) << 18) | ((p[i + 1] & 0x3F) << 12) |
                  ((p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F);
    if (cp >= 0x10000 && cp <= 0x10FFFF) { i += 4; return cp; }
  }
  i += 1;
  return 0xFFFD;
}

void utf8Encode(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

size_t utf16Length(const std::string& s) {
  size_t n = 0;
  size_t i = 0;
  while (i < s.size()) {
    uint32_t cp = utf8Decode(s, i);
    n += (cp >= 0x10000) ? 2 : 1;
  }
  return n;
}

// Next UTF-16 code unit of s at byte offset i, with a pending low surrogate.
struct Utf16Iter {
  const std::string& s;
  size_t i = 0;
  int pending = -1;  // low surrogate waiting to be yielded, or -1
  explicit Utf16Iter(const std::string& str, size_t start = 0) : s(str), i(start) {}
  // Returns false when exhausted.
  bool next(uint32_t& unit) {
    if (pending >= 0) { unit = static_cast<uint32_t>(pending); pending = -1; return true; }
    if (i >= s.size()) return false;
    uint32_t cp = utf8Decode(s, i);
    if (cp >= 0x10000) {
      uint32_t v = cp - 0x10000;
      unit = 0xD800 + (v >> 10);
      pending = static_cast<int>(0xDC00 + (v & 0x3FF));
      return true;
    }
    unit = cp;
    return true;
  }
};

int compareCodeUnits(const std::string& a, const std::string& b) {
  // Fast path: while both bytes are ASCII and equal, byte order is UTF-16
  // unit order. Stops at the first difference or the first non-ASCII byte;
  // i is then a code-point boundary in both strings.
  size_t n = a.size() < b.size() ? a.size() : b.size();
  size_t i = 0;
  while (i < n) {
    unsigned char ca = static_cast<unsigned char>(a[i]);
    unsigned char cb = static_cast<unsigned char>(b[i]);
    if (ca != cb) {
      if (ca < 0x80 && cb < 0x80) return ca < cb ? -1 : 1;
      break;
    }
    if (ca >= 0x80) break;
    i++;
  }
  if (i == n) {
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
  }
  Utf16Iter ia(a, i), ib(b, i);
  for (;;) {
    uint32_t ua, ub;
    bool ha = ia.next(ua), hb = ib.next(ub);
    if (!ha && !hb) return 0;
    if (!ha) return -1;
    if (!hb) return 1;
    if (ua != ub) return ua < ub ? -1 : 1;
  }
}

bool isWhiteSpace(uint32_t cp) {
  return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85 || cp == 0xA0 ||
         cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
         cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 ||
         cp == 0xFEFF;
}

bool hasWhitespace(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) if (isWhiteSpace(utf8Decode(s, i))) return true;
  return false;
}

std::string jsTrim(const std::string& s) {
  size_t a = 0, b = s.size();
  // Trimming is by code points; ASCII whitespace is single-byte so byte
  // offsets stay valid (White_Space code points >= 0x80 are multi-byte).
  size_t i = 0;
  while (i < b) {
    size_t save = i;
    uint32_t cp = utf8Decode(s, i);
    if (!isWhiteSpace(cp)) { i = save; break; }
    a = i;
  }
  // Trim the tail: walk from the start to find the last non-whitespace end.
  size_t end = a;
  i = a;
  while (i < b) {
    size_t save = i;
    uint32_t cp = utf8Decode(s, i);
    if (!isWhiteSpace(cp)) end = i;
  }
  return s.substr(a, end - a);
}

std::string jsNumber(double v) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
  if (v == 0) return "0";  // covers -0
  double av = std::fabs(v);
  if (av < 1e21 && std::trunc(v) == v) {
    // Integral: print exactly (exact for |v| < 2^53; %.0f is exact for any
    // integral double, it just prints the integer value).
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", v);
    return buf;
  }
  // Shortest round-trip digits via std::to_chars (general format).
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof buf, v);
  std::string t(buf, res.ptr);
  // Parse t into digits and E, where v = 0.digits * 10^E (digits[0] != '0').
  std::string digits;
  int E = 0;
  bool neg = false;
  size_t p = 0;
  if (p < t.size() && (t[p] == '-' || t[p] == '+')) { neg = (t[p] == '-'); p++; }
  size_t epos = t.find('e', p);
  int exp10 = 0;
  std::string mant = (epos == std::string::npos) ? t.substr(p) : t.substr(p, epos - p);
  if (epos != std::string::npos) exp10 = static_cast<int>(parseInt(std::string_view(t).substr(epos + 1)));
  size_t dot = mant.find('.');
  std::string intpart = (dot == std::string::npos) ? mant : mant.substr(0, dot);
  std::string fracpart = (dot == std::string::npos) ? "" : mant.substr(dot + 1);
  // Value = intpart.fracpart * 10^exp10. Collect digits, drop leading zeros.
  std::string all = intpart + fracpart;
  size_t lead = 0;
  while (lead < all.size() && all[lead] == '0') lead++;
  digits = all.substr(lead);
  // Trailing zeros cannot occur in a shortest representation except "0".
  E = static_cast<int>(intpart.size()) - static_cast<int>(lead) + exp10;
  // Now v = 0.digits * 10^E. JS Number::toString:
  std::string out;
  if (neg) out.push_back('-');
  if (E >= 1 && E <= 21) {
    // Plain, decimal point after E digits.
    if (static_cast<int>(digits.size()) >= E) {
      out += digits.substr(0, E);
      if (digits.size() > static_cast<size_t>(E)) { out.push_back('.'); out += digits.substr(E); }
    } else {
      out += digits;
      out.append(E - digits.size(), '0');
    }
  } else if (E <= 0 && E > -6) {
    // 1e-6 <= |v| < 1: 0.000<digits>
    out += "0.";
    out.append(-E, '0');
    out += digits;
  } else {
    // Exponential: d1[.rest]e(+/-)(E-1)
    out.push_back(digits[0]);
    if (digits.size() > 1) { out.push_back('.'); out += digits.substr(1); }
    out.push_back('e');
    int e = E - 1;
    out.push_back(e >= 0 ? '+' : '-');
    out += std::to_string(e >= 0 ? e : -e);
  }
  return out;
}
