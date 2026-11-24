// Fast JSON parser/serializer. See json.h.
#include "json.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {

// Append s as a JSON string literal (quotes included) directly to out.
void jsonEscapeInto(std::string& out, const std::string& s);

struct Parser {
  const char* p;
  const char* end;

  [[noreturn]] void fail(const char* what) { throw std::runtime_error(what); }

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  }

  uint32_t hex4() {
    if (end - p < 4) fail("bad \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
      char c = *p++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
      else fail("bad \\u escape");
    }
    return v;
  }

  // Parse a string starting at the opening quote. Handles all escapes.
  std::string parseString() {
    // *p == '"'
    p++;
    std::string out;
    // Fast path: copy runs that hold no '"' or '\\'. The next '"' or '\\'
    // is located with the SSE2 scan (findJsonStrSpecial).
    for (;;) {
      const char* run = p;
      p += findJsonStrSpecial(p, static_cast<size_t>(end - p));
      out.append(run, p);
      if (p >= end) fail("unterminated string");
      if (*p == '"') { p++; return out; }
      // *p == '\\'
      p++;
      if (p >= end) fail("bad escape");
      char e = *p++;
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = hex4();
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: try to pair with a following \uXXXX low surrogate.
            if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
              const char* save = p;
              p += 2;
              uint32_t lo = hex4();
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                p = save;
                cp = 0xFFFD;
              }
            } else {
              cp = 0xFFFD;  // lone surrogate (cannot occur from the generator)
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // lone low surrogate
          }
          utf8Encode(out, cp);
          break;
        }
        default: fail("bad escape");
      }
    }
  }

  double parseNumber() {
    const char* start = p;
    if (p < end && *p == '-') p++;
    while (p < end && *p >= '0' && *p <= '9') p++;
    if (p < end && *p == '.') {
      p++;
      while (p < end && *p >= '0' && *p <= '9') p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
      p++;
      if (p < end && (*p == '+' || *p == '-')) p++;
      while (p < end && *p >= '0' && *p <= '9') p++;
    }
    // strtod on the token (the grammar above is exactly JSON's number grammar).
    return std::strtod(std::string(start, p).c_str(), nullptr);
  }

  Value parseValue() {
    ws();
    if (p >= end) fail("unexpected end");
    char c = *p;
    if (c == '"') return Value::str(parseString());
    if (c == '{') {
      p++;
      Value obj = Value::object();
      ws();
      if (p < end && *p == '}') { p++; return obj; }
      for (;;) {
        ws();
        if (p >= end || *p != '"') fail("object key must be a string");
        std::string key = parseString();
        ws();
        if (p >= end || *p != ':') fail("expected ':'");
        p++;
        Value v = parseValue();
        obj.obj.emplace_back(std::move(key), std::move(v));
        ws();
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == '}') { p++; return obj; }
        fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      p++;
      Value arr = Value::array();
      ws();
      if (p < end && *p == ']') { p++; return arr; }
      for (;;) {
        arr.arr.push_back(parseValue());
        ws();
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == ']') { p++; return arr; }
        fail("expected ',' or ']'");
      }
    }
    if (c == 't') {
      if (end - p >= 4 && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') { p += 4; return Value::boolean(true); }
      fail("bad literal");
    }
    if (c == 'f') {
      if (end - p >= 5 && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') { p += 5; return Value::boolean(false); }
      fail("bad literal");
    }
    if (c == 'n') {
      if (end - p >= 4 && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') { p += 4; return Value::nullv(); }
      fail("bad literal");
    }
    if (c == '-' || (c >= '0' && c <= '9')) return Value::number(parseNumber());
    fail("unexpected character");
  }
};

void stringifyInto(const Value& v, std::string& out) {
  switch (v.type) {
    case Value::kNull: out += "null"; return;
    case Value::kUndefined: out += "null"; return;  // array element; object members skip
    case Value::kBool: out += v.b ? "true" : "false"; return;
    case Value::kNumber: out += jsNumber(v.num); return;
    case Value::kString: jsonEscapeInto(out, v.s); return;
    case Value::kArray: {
      out.push_back('[');
      for (size_t i = 0; i < v.arr.size(); i++) {
        if (i) out.push_back(',');
        stringifyInto(v.arr[i], out);
      }
      out.push_back(']');
      return;
    }
    case Value::kObject: {
      out.push_back('{');
      bool first = true;
      for (const auto& kv : v.obj) {
        if (kv.second.type == Value::kUndefined) continue;  // JS skips them
        if (!first) out.push_back(',');
        first = false;
        jsonEscapeInto(out, kv.first);
        out.push_back(':');
        stringifyInto(kv.second, out);
      }
      out.push_back('}');
      return;
    }
  }
}

// Append s as a JSON string literal (quotes included) directly to out:
// no temporary string per value — the output is megabytes of HTML.
// The next byte needing an escape is located with the SSE2 scan
// (findJsonQuoteSpecial: '"', '\\', or a byte < 0x20); the safe run before
// it is copied whole, so long plain stretches cost one scan, not one test
// per byte.
void jsonEscapeInto(std::string& out, const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  out.push_back('"');
  const char* p = s.data();
  size_t n = s.size();
  size_t run = 0;
  size_t i = 0;
  while (i < n) {
    size_t j = findJsonQuoteSpecial(p + i, n - i) + i;
    if (j == n) break;
    unsigned char c = static_cast<unsigned char>(s[j]);
    const char* esc = nullptr;
    char small[8];
    switch (c) {
      case '"': esc = "\\\""; break;
      case '\\': esc = "\\\\"; break;
      case '\b': esc = "\\b"; break;
      case '\f': esc = "\\f"; break;
      case '\n': esc = "\\n"; break;
      case '\r': esc = "\\r"; break;
      case '\t': esc = "\\t"; break;
      default:
        if (c < 0x20) {
          small[0] = '\\'; small[1] = 'u'; small[2] = '0'; small[3] = '0';
          small[4] = kHex[c >> 4]; small[5] = kHex[c & 0xF]; small[6] = '\0';
          esc = small;
        }
        break;
    }
    if (esc) {
      if (j > run) out.append(s, run, j - run);
      out += esc;
      run = j + 1;
    }
    i = j + 1;
  }
  if (n > run) out.append(s, run, n - run);
  out.push_back('"');
}

}  // namespace

Value jsonParse(const std::string& text) {
  Parser ps{text.data(), text.data() + text.size()};
  Value v = ps.parseValue();
  ps.ws();
  if (ps.p != ps.end) throw std::runtime_error("trailing characters");
  return v;
}

size_t jsonSkipWs(const std::string& text, size_t pos) {
  while (pos < text.size() &&
         (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
    pos++;
  return pos;
}

Value jsonParseOne(const std::string& text, size_t& pos) {
  Parser ps{text.data(), text.data() + text.size()};
  ps.p = text.data() + pos;
  Value v = ps.parseValue();
  pos = static_cast<size_t>(ps.p - text.data());
  return v;
}

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  jsonEscapeInto(out, s);
  return out;
}

std::string jsonStringify(const Value& v) {
  std::string out;
  out.reserve(8 << 20);  // answers are megabytes; avoid repeated regrowth
  stringifyInto(v, out);
  return out;
}
