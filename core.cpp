// C++ port of build/solve.js. See core.h. Faithful translation, function by
// function; comments quote the JS where a subtlety lives.
#include "core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "json.h"

namespace {

#ifdef NATIVE_STATS
#include <cstdio>
struct Stats {
  long long parseFrontMatter = 0, renderMarkdown = 0, inlineRange = 0, link = 0,
            slugify = 0, lookup = 0, renderNodes = 0, escapeHtmlInto = 0, textOf = 0;
  ~Stats() {
    fprintf(stderr,
      "STATS parseFrontMatter=%lld renderMarkdown=%lld inlineRange=%lld link=%lld slugify=%lld lookup=%lld renderNodes=%lld escapeHtmlInto=%lld textOf=%lld\n",
      parseFrontMatter, renderMarkdown, inlineRange, link, slugify, lookup, renderNodes, escapeHtmlInto, textOf);
  }
};
Stats g_stats;
#endif

const std::string RESERVED[3] = {"posts", "tags", "page"};
const char* REPORT_FILE = "build-report.txt";
const int INCLUDE_DEPTH = 8;
const int SLUG_MAX = 64;
const char* MONTHS[12] = {"January", "February", "March", "April", "May", "June",
                          "July", "August", "September", "October", "November", "December"};

int cmpCU(const std::string& a, const std::string& b) { return compareCodeUnits(a, b); }
struct CULess {
  bool operator()(const std::string& a, const std::string& b) const { return cmpCU(a, b) < 0; }
};

bool startsWith(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool endsWith(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlphaAscii(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isIdentStart(char c) { return isAlphaAscii(c) || c == '_'; }
bool isIdentChar(char c) { return isAlphaAscii(c) || isDigit(c) || c == '_'; }
bool isSpaceTab(char c) { return c == ' ' || c == '\t'; }

// JS /\s/ on a whole string: any Unicode White_Space code point.
bool hasJsWhitespace(const std::string& s) { return hasWhitespace(s); }

// JS String.prototype.trim on one code point basis is jsTrim (see util.cpp).

// ---------------------------------------------------------------- text

std::string escapeHtml(const std::string& text) {
  static const char* table[128] = {};
  static bool init = false;
  if (!init) {
    // ESC_TABLE[38]='&amp;' etc.; only & < > " ' ever escape.
    static const char* t38 = "&amp;";
    static const char* t60 = "&lt;";
    static const char* t62 = "&gt;";
    static const char* t34 = "&quot;";
    static const char* t39 = "&#39;";
    table[38] = t38; table[60] = t60; table[62] = t62; table[34] = t34; table[39] = t39;
    init = true;
  }
  bool special = findHtmlSpecial(text.data(), text.size()) != text.size();
  if (!special) return text;
  std::string out;
  size_t start = 0;
  size_t i = 0;
  while (i < text.size()) {
    size_t j = findHtmlSpecial(text.data() + i, text.size() - i) + i;
    if (j == text.size()) break;
    unsigned char c = static_cast<unsigned char>(text[j]);
    const char* e = c < 128 ? table[c] : nullptr;
    if (e) {
      out.append(text, start, j - start);
      out += e;
      start = j + 1;
    }
    i = j + 1;
  }
  out.append(text, start, std::string::npos);
  return out;
}

// Append `text` to `out` with the same five HTML escapes as escapeHtml,
// without building a temporary string. The next escape is located with the
// SSE2 scan (findHtmlSpecial); the run between escapes is copied whole.
void escapeHtmlInto(std::string& out, const char* p, size_t n) {
#ifdef NATIVE_STATS
  g_stats.escapeHtmlInto++;
#endif
  size_t start = 0;
  size_t i = 0;
  while (i < n) {
    size_t j = findHtmlSpecial(p + i, n - i) + i;
    if (j == n) break;
    char c = p[j];
    const char* e = nullptr;
    switch (c) {
      case '&': e = "&amp;"; break;
      case '<': e = "&lt;"; break;
      case '>': e = "&gt;"; break;
      case '"': e = "&quot;"; break;
      case '\'': e = "&#39;"; break;
      default: break;
    }
    if (e) {
      out.append(p, start, j - start);
      out += e;
      start = j + 1;
    }
    i = j + 1;
  }
  out.append(p, start, n - start);
}
void escapeHtmlInto(std::string& out, const std::string& text) {
  escapeHtmlInto(out, text.data(), text.size());
}

#include "marks.inc"

bool isMark(uint32_t cp) {
  size_t lo = 0, hi = kMarkRangeCount;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (cp < kMarkRanges[mid][0]) hi = mid;
    else if (cp > kMarkRanges[mid][1]) lo = mid + 1;
    else return true;
  }
  return false;
}

// Strip combining marks (General Category M) from a UTF-8 string.
std::string stripMarks(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t save = i;
    uint32_t cp = utf8Decode(s, i);
    if (!isMark(cp)) out.append(s, save, i - save);
  }
  return out;
}

struct Slugify {
  const NormalizeFn& normalize;
  std::unordered_map<std::string, std::string> cache;
  explicit Slugify(const NormalizeFn& n) : normalize(n) {}

  std::string slow(const std::string& text) {
    std::string plain = text;
    bool ascii = true;
    for (unsigned char c : text) {
      if (c > 127) { ascii = false; break; }
    }
    if (!ascii) plain = stripMarks(normalize(text));
    std::string out;
    bool gap = false;
    // JS iterates UTF-16 code units; iterating UTF-8 code points gives the
    // same result (a non-ASCII unit/code point is always a gap).
    size_t i = 0;
    while (i < plain.size()) {
      uint32_t cp = utf8Decode(plain, i);
      char keep = 0;
      if (cp >= 'A' && cp <= 'Z') keep = static_cast<char>(cp - 'A' + 'a');
      else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) keep = static_cast<char>(cp);
      if (keep == 0) {
        gap = !out.empty();
        continue;
      }
      if (gap) out += '-';
      gap = false;
      out += keep;
    }
    if (static_cast<int>(out.size()) > SLUG_MAX) out.resize(SLUG_MAX);
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "untitled" : out;
  }

  std::string operator()(const std::string& text) {
#ifdef NATIVE_STATS
    g_stats.slugify++;
#endif
    auto it = cache.find(text);
    if (it != cache.end()) return it->second;
    std::string out = slow(text);
    cache.emplace(text, out);
    return out;
  }
};

std::string uniqueSlug(const std::string& slug, std::unordered_set<std::string>& taken) {
  std::string out = slug;
  for (int n = 2; taken.count(out) != 0; n++) out = slug + "-" + std::to_string(n);
  taken.insert(out);
  return out;
}

std::string longDate(const std::string& date) {
  std::string y = date.substr(0, 4);
  int m = std::stoi(date.substr(5, 2));
  int d = std::stoi(date.substr(8, 2));
  return std::to_string(d) + " " + MONTHS[m - 1] + " " + y;
}

long long wordsOf(const std::string& text) {
  long long n = 0;
  bool inWord = false;
  for (unsigned char c : text) {
    bool ws = c == 32 || c == 9 || c == 10 || c == 13 || c == 12 || c == 11;
    if (ws) inWord = false;
    else if (!inWord) { inWord = true; n++; }
  }
  return n;
}

// ---------------------------------------------------------------- front matter

int daysIn(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}

struct StrResult {
  bool ok = false;
  std::string value;  // value when ok, error message when !ok
};

// A string value: "..." with \" and \\ its only escapes, or bare.
StrResult stringOf(const std::string& raw) {
  if (!raw.empty() && raw[0] == '"') {
    if (raw.size() < 2 || raw.back() != '"') return {false, "a quoted string is not closed"};
    std::string out;
    for (size_t i = 1; i + 1 < raw.size(); i++) {
      char c = raw[i];
      if (c == '\\') {
        char n = raw[i + 1];
        if ((n != '"' && n != '\\') || i + 1 == raw.size() - 1)
          return {false, "a quoted string holds an escape other than \\\" or \\\\"};
        out += n;
        i++;
      } else if (c == '"') {
        return {false, "a quoted string holds a stray quote"};
      } else {
        out += c;
      }
    }
    return {true, out};
  }
  if (!raw.empty() && raw[0] == '[') return {false, "a bare string opens on ["};
  return {true, raw};
}

// One key's value, or why it is refused. Value when ok.
struct ValResult {
  bool ok = false;
  Value value;       // valid when ok
  std::string error; // valid when !ok
};

bool matchDate(const std::string& raw, int& y, int& mo, int& d) {
  if (raw.size() != 10) return false;
  if (raw[4] != '-' || raw[7] != '-') return false;
  for (size_t i = 0; i < raw.size(); i++) {
    if (i == 4 || i == 7) continue;
    if (!isDigit(raw[i])) return false;
  }
  y = std::stoi(raw.substr(0, 4));
  mo = std::stoi(raw.substr(5, 2));
  d = std::stoi(raw.substr(8, 2));
  return true;
}

ValResult valueOf(const std::string& key, const std::string& raw) {
  if (key == "title" || key == "summary") {
    StrResult s = stringOf(raw);
    if (!s.ok) return {false, Value(), "key \"" + key + "\": " + s.value};
    return {true, Value::str(s.value), ""};
  }
  if (key == "date") {
    int y = 0, mo = 0, d = 0;
    bool m = matchDate(raw, y, mo, d);
    if (!m || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo))
      return {false, Value(), "key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)"};
    return {true, Value::str(raw), ""};
  }
  if (key == "tags") {
    if (raw.empty() || raw[0] != '[' || raw.back() != ']')
      return {false, Value(), "key \"tags\": not a list [a, b]"};
    std::string inner = raw.substr(1, raw.size() - 2);
    if (jsTrim(inner).empty()) return {true, Value::array(), ""};
    // split on ','
    std::vector<std::string> tags;
    size_t start = 0;
    for (;;) {
      size_t comma = inner.find(',', start);
      std::string part = inner.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      tags.push_back(jsTrim(part));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    for (const std::string& t : tags) {
      bool bad = t.empty();
      if (!bad) {
        for (unsigned char c : t) {
          if (c == '"' || c == '[' || c == ']') { bad = true; break; }
        }
      }
      if (bad) return {false, Value(), "key \"tags\": an empty tag, or one holding \" [ or ]"};
    }
    Value arr = Value::array();
    for (const std::string& t : tags) arr.arr.push_back(Value::str(t));
    return {true, arr, ""};
  }
  if (key == "draft") {
    if (raw == "true") return {true, Value::boolean(true), ""};
    if (raw == "false") return {true, Value::boolean(false), ""};
    return {false, Value(), "key \"draft\": true or false"};
  }
  if (key == "layout") {
    bool ok = !raw.empty();
    for (char c : raw) {
      if (!((c >= 'a' && c <= 'z') || isDigit(c) || c == '-')) { ok = false; break; }
    }
    if (ok) return {true, Value::str(raw), ""};
    return {false, Value(), "key \"layout\": not a template name"};
  }
  if (key == "slug") {
    bool ok = raw.size() <= 64 && !raw.empty();
    // SLUG_RE /^[a-z0-9]+(?:-[a-z0-9]+)*$/
    if (ok) {
      bool prevDash = false;
      if (raw[0] == '-' ) ok = false;
      for (size_t i = 0; ok && i < raw.size(); i++) {
        char c = raw[i];
        if (c == '-') {
          if (prevDash || i == raw.size() - 1) ok = false;
          prevDash = true;
        } else if ((c >= 'a' && c <= 'z') || isDigit(c)) {
          prevDash = false;
        } else {
          ok = false;
        }
      }
      if (raw.back() == '-') ok = false;
    }
    if (ok) return {true, Value::str(raw), ""};
    return {false, Value(), "key \"slug\": not a slug"};
  }
  return {false, Value(), "unknown key \"" + key + "\""};
}

bool isBlank(const std::string& line) {
  for (char c : line) {
    if (c != ' ' && c != '\t') return false;
  }
  return true;
}

struct FmResult {
  bool ok = false;
  Value data;        // object, when ok
  std::string body;  // when ok
  std::string error; // when !ok
};

FmResult parseFrontMatter(const std::string& text) {
#ifdef NATIVE_STATS
  g_stats.parseFrontMatter++;
#endif
  size_t firstNl = text.find('\n');
  std::string first = firstNl == std::string::npos ? text : text.substr(0, firstNl);
  if (first != "---") return {false, Value(), "", "no front matter"};
  std::vector<std::string> lines;
  size_t pos = firstNl == std::string::npos ? text.size() : firstNl + 1;
  long long bodyStart = -1;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    size_t end = nl == std::string::npos ? text.size() : nl;
    std::string line = text.substr(pos, end - pos);
    if (line == "---") {
      bodyStart = nl == std::string::npos ? static_cast<long long>(text.size())
                                          : static_cast<long long>(nl) + 1;
      break;
    }
    lines.push_back(line);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  if (bodyStart == -1) return {false, Value(), "", "the front matter is not closed"};
  static const char* KEYS[7] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};
  Value data = Value::object();
  for (size_t i = 0; i < lines.size(); i++) {
    const std::string& line = lines[i];
    if (isBlank(line) || (!line.empty() && line[0] == '#')) continue;
    // LINE: ^([a-z_]+):(?: (.*))?$
    size_t colon = line.find(':');
    std::string key;
    std::string raw;
    bool haveKey = false;
    if (colon != std::string::npos && colon > 0) {
      std::string k = line.substr(0, colon);
      bool ok = true;
      for (char c : k) {
        if (!((c >= 'a' && c <= 'z') || c == '_')) { ok = false; break; }
      }
      if (ok) {
        if (colon == line.size() - 1) raw = "";
        else if (line[colon + 1] == ' ') raw = line.substr(colon + 2);
        else ok = false;
        if (ok) { key = k; haveKey = true; }
      }
    }
    if (!haveKey)
      return {false, Value(), "", "line " + std::to_string(i + 2) + " is not \"key: value\""};
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();
    bool known = false;
    for (const char* k : KEYS) if (key == k) { known = true; break; }
    if (!known) return {false, Value(), "", "unknown key \"" + key + "\""};
    if (data.has(key)) return {false, Value(), "", "key \"" + key + "\" is given twice"};
    if (raw.empty()) return {false, Value(), "", "key \"" + key + "\" has no value"};
    ValResult v = valueOf(key, raw);
    if (!v.ok) return {false, Value(), "", v.error};
    data.set(key, v.value);
  }
  if (!data.has("title")) return {false, Value(), "", "no title"};
  return {true, data, text.substr(static_cast<size_t>(bodyStart)), ""};
}

// ---------------------------------------------------------------- Markdown

bool isEscapable(char c) {
  switch (c) {
    case '\\': case '`': case '*': case '_': case '[': case ']': case '(': case ')':
    case '#': case '+': case '-': case '.': case '!': case '>': return true;
    default: return false;
  }
}

std::string trimLine(const std::string& line) {
  size_t a = 0, b = line.size();
  while (a < b && isSpaceTab(line[a])) a++;
  while (b > a && isSpaceTab(line[b - 1])) b--;
  return line.substr(a, b - a);
}

// A heading line's level and text, or depth 0 (a heading's text is never empty).
struct Heading {
  int depth = 0;
  std::string text;
};

Heading headingOf(const std::string& line) {
  int n = 0;
  while (n < static_cast<int>(line.size()) && line[n] == '#' && n < 6) n++;
  if (n == 0) return {};
  if (n == 6 && line.size() > 6 && line[6] == '#') return {};
  if (n >= static_cast<int>(line.size()) || !isSpaceTab(line[n])) return {};
  size_t a = static_cast<size_t>(n);
  while (a < line.size() && isSpaceTab(line[a])) a++;
  size_t b = line.size();
  while (b > a && isSpaceTab(line[b - 1])) b--;
  if (b == a) return {};
  return {n, line.substr(a, b - a)};
}

// Fence open: language ("" for a bare fence) or nullopt.
bool fenceOpen(const std::string& line, std::string& lang) {
  if (line.size() < 3 || line[0] != '`' || line[1] != '`' || line[2] != '`') return false;
  size_t i = 3;
  lang = "";
  while (i < line.size()) {
    char c = line[i];
    if (isAlphaAscii(c) || isDigit(c) || c == '_' || c == '+' || c == '-') { lang += c; i++; }
    else break;
  }
  while (i < line.size() && isSpaceTab(line[i])) i++;
  return i == line.size();
}

bool fenceClose(const std::string& line) {
  if (line.size() < 3 || line[0] != '`' || line[1] != '`' || line[2] != '`') return false;
  size_t i = 3;
  while (i < line.size() && isSpaceTab(line[i])) i++;
  return i == line.size();
}

bool isHr(const std::string& line) {
  if (line != "---" && line != "***") {
    if (line.size() < 3) return false;
    std::string head = line.substr(0, 3);
    if (head != "---" && head != "***") return false;
    for (size_t i = 3; i < line.size(); i++) if (!isSpaceTab(line[i])) return false;
    return true;
  }
  return true;
}

bool endsParagraph(const std::string& line) {
  std::string lang;
  return isBlank(line) || fenceOpen(line, lang) || headingOf(line).depth != 0 || isHr(line);
}

struct Token {
  enum Type { kHeading, kCode, kHr, kBlockquote, kList, kParagraph } type;
  int depth = 0;             // heading
  std::string text;          // heading text / paragraph text
  std::string lang;          // code
  std::vector<std::string> lines;  // code / blockquote
  bool ordered = false;      // list
  long long start = 1;       // list
  std::vector<std::string> items;  // list
};

std::vector<std::string> splitLines(const std::string& src) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    size_t nl = src.find('\n', start);
    if (nl == std::string::npos) { out.push_back(src.substr(start)); break; }
    out.push_back(src.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

std::vector<Token> lex(const std::string& src) {
  std::vector<std::string> lines = splitLines(src);
  std::vector<Token> tokens;
  size_t i = 0;
  size_t n = lines.size();
  while (i < n) {
    const std::string& line = lines[i];
    if (isBlank(line)) { i++; continue; }
    std::string lang;
    if (fenceOpen(line, lang)) {
      std::vector<std::string> body;
      i++;
      while (i < n && !fenceClose(lines[i])) body.push_back(lines[i++]);
      i++;
      Token t; t.type = Token::kCode; t.lang = lang; t.lines = body;
      tokens.push_back(std::move(t));
      continue;
    }
    Heading heading = headingOf(line);
    if (heading.depth != 0) {
      Token t; t.type = Token::kHeading; t.depth = heading.depth; t.text = heading.text;
      tokens.push_back(std::move(t));
      i++;
      continue;
    }
    if (isHr(line)) {
      Token t; t.type = Token::kHr;
      tokens.push_back(t);
      i++;
      continue;
    }
    if (!line.empty() && line[0] == '>') {
      std::vector<std::string> body;
      while (i < n && !lines[i].empty() && lines[i][0] == '>') {
        std::string l = lines[i++];
        l = l.substr(1);
        if (!l.empty() && l[0] == ' ') l = l.substr(1);
        body.push_back(l);
      }
      std::vector<std::string> kept;
      for (const std::string& l : body) {
        std::string t = trimLine(l);
        if (!t.empty()) kept.push_back(t);
      }
      Token t; t.type = Token::kBlockquote; t.lines = kept;
      tokens.push_back(std::move(t));
      continue;
    }
    bool hasBullet = false, hasOrdered = false;
    std::string bulletText;
    std::string orderedNum, orderedText;
    if (line.size() >= 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ') {
      hasBullet = true;
      bulletText = line.substr(2);
    } else {
      size_t j = 0;
      while (j < line.size() && isDigit(line[j]) && j < 9) j++;
      if (j > 0 && j < line.size() && line[j] == '.' && j + 1 < line.size() && line[j + 1] == ' ') {
        hasOrdered = true;
        orderedNum = line.substr(0, j);
        orderedText = line.substr(j + 2);
      }
    }
    if (hasBullet || hasOrdered) {
      bool isOrdered = hasOrdered;
      std::vector<std::vector<std::string>> items;
      while (i < n) {
        const std::string& l = lines[i];
        bool matched = false;
        std::string m;
        if (isOrdered) {
          size_t j = 0;
          while (j < l.size() && isDigit(l[j]) && j < 9) j++;
          if (j > 0 && j < l.size() && l[j] == '.' && j + 1 < l.size() && l[j + 1] == ' ') {
            matched = true;
            m = l.substr(j + 2);
          }
        } else if (l.size() >= 2 && (l[0] == '-' || l[0] == '*') && l[1] == ' ') {
          matched = true;
          m = l.substr(2);
        }
        if (matched) {
          items.push_back({trimLine(m)});
          i++;
        } else {
          // CONTINUED: two or more spaces then a non-space character
          size_t k = 0;
          while (k < l.size() && l[k] == ' ') k++;
          if (k >= 2 && k < l.size() && l[k] != ' ' && l[k] != '\t' && l[k] != '\f' && l[k] != '\v') {
            items.back().push_back(trimLine(l));
            i++;
          } else break;
        }
      }
      Token t;
      t.type = Token::kList;
      t.ordered = isOrdered;
      t.start = isOrdered ? std::stoll(orderedNum) : 1;
      for (auto& parts : items) {
        std::string joined;
        for (size_t p = 0; p < parts.size(); p++) {
          if (p) joined += '\n';
          joined += parts[p];
        }
        t.items.push_back(joined);
      }
      tokens.push_back(std::move(t));
      continue;
    }
    std::vector<std::string> body;
    body.push_back(trimLine(line));
    i++;
    while (i < n && !endsParagraph(lines[i])) body.push_back(trimLine(lines[i++]));
    std::string text;
    for (size_t p = 0; p < body.size(); p++) {
      if (p) text += '\n';
      text += body[p];
    }
    Token t; t.type = Token::kParagraph; t.text = text;
    tokens.push_back(std::move(t));
  }
  return tokens;
}

// ---------------------------------------------------------------- inline

// The first index from `from` where `s` holds `what` outside a backslash escape, or -1.
long long findUnescaped(const std::string& s, const std::string& what, size_t from, size_t end) {
  size_t wlen = what.size();
  for (size_t j = from; j + wlen <= end; j++) {
    if (s[j] == '\\' && j + 1 < end && isEscapable(s[j + 1])) { j++; continue; }
    if (s[j] == what[0] && (wlen == 1 || s.compare(j, wlen, what) == 0)) return static_cast<long long>(j);
  }
  return -1;
}

// The first single * from `from` (one whose run is not **), outside an escape, or -1.
long long findSingleStar(const std::string& s, size_t from, size_t end) {
  for (size_t j = from; j < end; j++) {
    char c = s[j];
    if (c == '\\' && j + 1 < end && isEscapable(s[j + 1])) { j++; continue; }
    if (c != '*') continue;
    if (j + 1 < end && s[j + 1] == '*') { j++; continue; }
    return static_cast<long long>(j);
  }
  return -1;
}

struct Source;  // defined in the build section below

// Link resolution context for markdown rendering: plain data pointers, no
// std::function indirection (the resolver is called per link).
struct LinkEnv {
  const Source* self = nullptr;
  const std::vector<std::string>* dirParts = nullptr;
  const std::unordered_map<std::string, Source*>* bySource = nullptr;
  const std::unordered_set<std::string>* staticPaths = nullptr;
  const std::string* base = nullptr;
  std::unordered_set<std::string>* broken = nullptr;
};
std::string resolveLink(const LinkEnv& env, const std::string& target);

void inlineRange(const std::string& s, size_t start, size_t end, const LinkEnv& env,
                 std::string& out);

void inlineRange(const std::string& s, size_t start, size_t end, const LinkEnv& env,
                 std::string& out) {
#ifdef NATIVE_STATS
  g_stats.inlineRange++;
#endif
  size_t i = start;
  size_t run = start;
  for (;;) {
    // Next byte in {\,`,*,[,<,>,&,",'} at or after i (all ASCII, so a byte
    // scan equals the JS regex scan). Located with the SSE2 scan
    // (findInlineSpecial); the run before it is copied whole.
    size_t j = findInlineSpecial(s.data() + i, end - i) + i;
    if (j >= end) break;
    char c = s[j];
    const char* ent = nullptr;
    switch (c) {
      case '&': ent = "&amp;"; break;
      case '<': ent = "&lt;"; break;
      case '>': ent = "&gt;"; break;
      case '"': ent = "&quot;"; break;
      case '\'': ent = "&#39;"; break;
      default: break;
    }
    if (ent != nullptr) {
      if (j > run) out.append(s, run, j - run);
      out += ent;
      i = j + 1;
      run = i;
      continue;
    }
    if (c == '\\' && j + 1 < end && isEscapable(s[j + 1])) {
      if (j > run) out.append(s, run, j - run);
      escapeHtmlInto(out, s.data() + j + 1, 1);
      i = j + 2;
      run = i;
      continue;
    }
    if (c == '`') {
      size_t k = s.find('`', j + 1);
      if (k != std::string::npos && k < end && k > j + 1) {
        if (j > run) out.append(s, run, j - run);
        out += "<code>";
        escapeHtmlInto(out, s.data() + j + 1, k - (j + 1));
        out += "</code>";
        i = k + 1;
        run = i;
      } else {
        i = j + 1;
      }
      continue;
    }
    if (c == '*') {
      if (j + 1 < end && s[j + 1] == '*') {
        long long k = findUnescaped(s, "**", j + 2, end);
        if (k != -1 && k > static_cast<long long>(j) + 2) {
          if (j > run) out.append(s, run, j - run);
          out += "<strong>";
          inlineRange(s, j + 2, static_cast<size_t>(k), env, out);
          out += "</strong>";
          i = static_cast<size_t>(k) + 2;
          run = i;
        } else {
          i = j + 2;
        }
        continue;
      }
      long long k = findSingleStar(s, j + 1, end);
      if (k != -1 && k > static_cast<long long>(j) + 1) {
        if (j > run) out.append(s, run, j - run);
        out += "<em>";
        inlineRange(s, j + 1, static_cast<size_t>(k), env, out);
        out += "</em>";
        i = static_cast<size_t>(k) + 1;
        run = i;
      } else {
        i = j + 1;
      }
      continue;
    }
    // c === '['
    if (c == '[') {
      long long k = findUnescaped(s, "]", j + 1, end);
      long long m2 = -1;
      std::string target;
      if (k != -1 && static_cast<size_t>(k) + 1 < end && s[static_cast<size_t>(k) + 1] == '(') {
        size_t close = s.find(')', static_cast<size_t>(k) + 2);
        if (close != std::string::npos && close < end) {
          target = s.substr(static_cast<size_t>(k) + 2, close - (static_cast<size_t>(k) + 2));
          m2 = static_cast<long long>(close);
        } else {
          m2 = -1;
        }
      }
      if (m2 != -1 && !target.empty() && !hasWhitespace(target)) {
        if (j > run) out.append(s, run, j - run);
        out += "<a href=\"";
        escapeHtmlInto(out, resolveLink(env, target));
        out += "\">";
        inlineRange(s, j + 1, static_cast<size_t>(k), env, out);
        out += "</a>";
        i = static_cast<size_t>(m2) + 1;
        run = i;
        continue;
      }
    }
    i = j + 1;
  }
  if (end > run) out.append(s, run, end - run);
}

std::string inlineStr(const std::string& s, const LinkEnv& env) {
  std::string r;
  inlineRange(s, 0, s.size(), env, r);
  return r;
}

struct TocEntry {
  int level = 0;
  std::string id;
  std::string html;
};

struct MdResult {
  std::string html;
  std::vector<TocEntry> toc;
  std::string excerpt;
};

MdResult renderMarkdown(const std::string& src, const LinkEnv& env, Slugify& slugify) {
#ifdef NATIVE_STATS
  g_stats.renderMarkdown++;
#endif
  std::unordered_set<std::string> ids;
  std::string out;
  std::vector<TocEntry> toc;
  bool hasExcerpt = false;
  std::string excerpt;
  for (const Token& t : lex(src)) {
    switch (t.type) {
      case Token::kHeading: {
        std::string id = uniqueSlug(slugify(t.text), ids);
        std::string html = inlineStr(t.text, env);
        out += "<h" + std::to_string(t.depth) + " id=\"" + id + "\">";
        out += html;
        out += "</h" + std::to_string(t.depth) + ">";
        out += '\n';
        if (t.depth == 2 || t.depth == 3) toc.push_back({t.depth, id, html});
        break;
      }
      case Token::kCode: {
        std::string cls = t.lang.empty() ? "" : " class=\"language-" + escapeHtml(t.lang) + "\"";
        out += "<pre><code" + cls + ">";
        for (const std::string& l : t.lines) {
          escapeHtmlInto(out, l);
          out += '\n';
        }
        out += "</code></pre>";
        out += '\n';
        break;
      }
      case Token::kHr:
        out += "<hr>";
        out += '\n';
        break;
      case Token::kBlockquote:
        if (t.lines.empty()) {
          out += "<blockquote>\n</blockquote>";
          out += '\n';
        } else {
          std::string bq;
          for (size_t p = 0; p < t.lines.size(); p++) {
            if (p) bq += '\n';
            bq += t.lines[p];
          }
          out += "<blockquote>\n<p>";
          inlineRange(bq, 0, bq.size(), env, out);
          out += "</p>\n</blockquote>";
          out += '\n';
        }
        break;
      case Token::kList: {
        std::string tag = t.ordered ? "ol" : "ul";
        std::string open = (t.ordered && t.start != 1)
                               ? "<ol start=\"" + std::to_string(t.start) + "\">"
                               : "<" + tag + ">";
        out += open;
        out += '\n';
        for (const std::string& item : t.items) {
          out += "<li>";
          inlineRange(item, 0, item.size(), env, out);
          out += "</li>";
          out += '\n';
        }
        out += "</" + tag + ">";
        out += '\n';
        break;
      }
      case Token::kParagraph: {
        if (!hasExcerpt) {
          std::string html = inlineStr(t.text, env);
          excerpt = html;
          hasExcerpt = true;
          out += "<p>";
          out += html;
          out += "</p>";
          out += '\n';
        } else {
          out += "<p>";
          inlineRange(t.text, 0, t.text.size(), env, out);
          out += "</p>";
          out += '\n';
        }
        break;
      }
    }
  }
  return {out, toc, excerpt};
}

// ---------------------------------------------------------------- templates

struct TemplateError : public std::runtime_error {
  explicit TemplateError(const std::string& msg) : std::runtime_error(msg) {}
};

[[noreturn]] void throwTemplate(const std::string& name, const std::string& reason) {
  throw TemplateError("template \"" + name + "\": " + reason);
}

// The path `text` names, split at its dots, or a TemplateError.
std::vector<std::string> pathOf(const std::string& name, const std::string& text) {
  std::vector<std::string> parts;
  size_t start = 0;
  bool ok = !text.empty();
  for (;;) {
    size_t dot = text.find('.', start);
    std::string seg = text.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (seg.empty() || !isIdentStart(seg[0])) ok = false;
    for (size_t i = 1; ok && i < seg.size(); i++) if (!isIdentChar(seg[i])) ok = false;
    parts.push_back(seg);
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  if (!ok) throwTemplate(name, jsonEscape(text) + " is not a path");
  return parts;
}

struct Tok {
  enum Type { kText, kVar, kRaw, kTag } t;
  std::string text;                 // kText
  std::vector<std::string> path;    // kVar / kRaw
  std::string tag;                  // kTag
};

// `src`'s tokens: text, {{ }}, {{{ }}} and {% %}, a standalone tag's line taken with it.
std::vector<Tok> tokenize(const std::string& name, const std::string& src) {
  std::vector<Tok> tokens;
  std::string text;
  size_t i = 0;
  while (i < src.size()) {
    size_t open = src.find('{', i);
    if (open == std::string::npos || open == src.size() - 1) {
      text += src.substr(i);
      break;
    }
    char next = src[open + 1];
    if (next != '{' && next != '%') {
      text += src.substr(i, open + 1 - i);
      i = open + 1;
      continue;
    }
    text += src.substr(i, open - i);
    if (next == '{') {
      bool raw = open + 2 < src.size() && src[open + 2] == '{';
      size_t close = src.find(raw ? "}}}" : "}}", open + (raw ? 3 : 2));
      if (close == std::string::npos)
        throwTemplate(name, std::string("a ") + (raw ? "{{{" : "{{") + " is not closed");
      if (!text.empty()) { Tok t; t.t = Tok::kText; t.text = text; tokens.push_back(std::move(t)); }
      text = "";
      Tok t;
      t.t = raw ? Tok::kRaw : Tok::kVar;
      t.path = pathOf(name, jsTrim(src.substr(open + (raw ? 3 : 2), close - (open + (raw ? 3 : 2)))));
      tokens.push_back(std::move(t));
      i = close + (raw ? 3 : 2);
      continue;
    }
    size_t close = src.find("%}", open + 2);
    if (close == std::string::npos) throwTemplate(name, "a {% is not closed");
    size_t end = close + 2;
    size_t lineStart;
    if (open == 0) lineStart = 0;
    else {
      size_t ls = src.rfind('\n', open - 1);
      lineStart = ls == std::string::npos ? 0 : ls + 1;
    }
    size_t nl = src.find('\n', end);
    size_t lineEnd = nl == std::string::npos ? src.size() : nl;
    auto allSpaceTab = [](const std::string& s) {
      for (char c : s) if (c != ' ' && c != '\t') return false;
      return true;
    };
    bool standalone = allSpaceTab(src.substr(lineStart, open - lineStart)) &&
                      allSpaceTab(src.substr(end, lineEnd - end));
    if (standalone) text.resize(text.size() - (open - lineStart));
    if (!text.empty()) { Tok t; t.t = Tok::kText; t.text = text; tokens.push_back(std::move(t)); }
    text = "";
    Tok t;
    t.t = Tok::kTag;
    t.tag = jsTrim(src.substr(open + 2, close - (open + 2)));
    tokens.push_back(std::move(t));
    i = standalone ? (nl == std::string::npos ? src.size() : nl + 1) : end;
  }
  if (!text.empty()) { Tok t; t.t = Tok::kText; t.text = text; tokens.push_back(std::move(t)); }
  return tokens;
}

struct Node {
  enum Kind { kRoot, kText, kVar, kRaw, kIf, kFor, kInclude } kind = kRoot;
  std::string text;                        // kText
  std::vector<std::string> path;           // kVar / kRaw / kIf / kFor
  std::string name;                        // kFor loop var / kInclude template name
  bool neg = false;                        // kIf: "not"
  std::vector<std::unique_ptr<Node>> body; // root / for body / if then-branch
  std::vector<std::unique_ptr<Node>> elseBody;  // if else-branch
  bool hasElse = false;
};

// FOR_RE: for <ident> in <path-no-spaces>; IF_RE: if [not ]<path-no-spaces>;
// INCLUDE_RE: include "<name[a-z0-9-]>". Matched by hand, no regex.
bool matchFor(const std::string& tag, std::string& varName, std::string& pathText) {
  if (!startsWith(tag, "for ")) return false;
  size_t p = 4;
  if (p >= tag.size() || !isIdentStart(tag[p])) return false;
  size_t s = p;
  while (p < tag.size() && isIdentChar(tag[p])) p++;
  varName = tag.substr(s, p - s);
  if (tag.compare(p, 4, " in ") != 0) return false;
  p += 4;
  if (p >= tag.size()) return false;
  pathText = tag.substr(p);
  if (hasJsWhitespace(pathText)) return false;
  return true;
}

bool matchIf(const std::string& tag, bool& neg, std::string& pathText) {
  if (!startsWith(tag, "if ")) return false;
  size_t p = 3;
  neg = false;
  if (tag.compare(p, 4, "not ") == 0) { neg = true; p += 4; }
  if (p >= tag.size()) return false;
  pathText = tag.substr(p);
  if (hasJsWhitespace(pathText)) return false;
  return true;
}

bool matchInclude(const std::string& tag, std::string& incName) {
  if (!startsWith(tag, "include \"")) return false;
  if (tag.size() < 11 || tag.back() != '"') return false;
  std::string mid = tag.substr(9, tag.size() - 10);
  if (mid.empty()) return false;
  for (char c : mid) {
    if (!((c >= 'a' && c <= 'z') || isDigit(c) || c == '-')) return false;
  }
  incName = mid;
  return true;
}

std::vector<std::unique_ptr<Node>> parseTemplate(const std::string& name, const std::string& src) {
  Node root;
  root.kind = Node::kRoot;
  std::vector<Node*> stack;
  stack.push_back(&root);
  std::vector<std::unique_ptr<Node>>* body = &root.body;
  for (const Tok& tok : tokenize(name, src)) {
    if (tok.t == Tok::kText) {
      auto n = std::make_unique<Node>();
      n->kind = Node::kText;
      n->text = tok.text;
      body->push_back(std::move(n));
    } else if (tok.t == Tok::kVar || tok.t == Tok::kRaw) {
      auto n = std::make_unique<Node>();
      n->kind = tok.t == Tok::kVar ? Node::kVar : Node::kRaw;
      n->path = tok.path;
      body->push_back(std::move(n));
    } else {
      const std::string& tag = tok.tag;
      Node* top = stack.back();
      std::string varName, pathText, incName;
      bool neg = false;
      if (matchFor(tag, varName, pathText)) {
        auto n = std::make_unique<Node>();
        n->kind = Node::kFor;
        n->name = varName;
        n->path = pathOf(name, pathText);
        Node* raw = n.get();
        body->push_back(std::move(n));
        stack.push_back(raw);
        body = &raw->body;
      } else if (matchIf(tag, neg, pathText)) {
        auto n = std::make_unique<Node>();
        n->kind = Node::kIf;
        n->neg = neg;
        n->path = pathOf(name, pathText);
        Node* raw = n.get();
        body->push_back(std::move(n));
        stack.push_back(raw);
        body = &raw->body;
      } else if (tag == "else") {
        if (top->kind != Node::kIf || top->hasElse)
          throwTemplate(name, "an else outside an if");
        top->hasElse = true;
        body = &top->elseBody;
      } else if (tag == "endfor" || tag == "endif") {
        const char* want = tag == "endfor" ? "for" : "if";
        Node::Kind wantKind = tag == "endfor" ? Node::kFor : Node::kIf;
        if (top->kind != wantKind)
          throwTemplate(name, "an " + tag + " with no open " + std::string(tag.substr(3)));
        (void)want;
        stack.pop_back();
        Node* parent = stack.back();
        if (parent->kind == Node::kRoot) body = &parent->body;
        else if (parent->kind == Node::kFor) body = &parent->body;
        else body = parent->hasElse ? &parent->elseBody : &parent->body;
      } else if (matchInclude(tag, incName)) {
        auto n = std::make_unique<Node>();
        n->kind = Node::kInclude;
        n->name = incName;
        body->push_back(std::move(n));
      } else {
        throwTemplate(name, "{% " + tag + " %} is not a tag");
      }
    }
  }
  if (stack.size() > 1) {
    const char* kind = stack.back()->kind == Node::kFor ? "for" : "if";
    throwTemplate(name, std::string("a ") + kind + " is not closed");
  }
  return std::move(root.body);
}

// ---------------------------------------------------------------- scope / render

struct Scope {
  std::unordered_map<std::string, Value> vars;  // std::map: lookup only, order irrelevant
  const Scope* parent = nullptr;
};

// The value at `path` in the scope chain: its first name from the nearest
// scope holding it, then own properties. Returns a pointer into the scope (or
// into the object graph) — never a copy: lookups run on hot paths and the
// values can be whole arrays (posts) or whole pages (content HTML).
// nullptr means undefined.
const Value* lookup(const Scope* scope, const std::vector<std::string>& path) {
#ifdef NATIVE_STATS
  g_stats.lookup++;
#endif
  const Scope* s = scope;
  auto it = s ? s->vars.find(path[0]) : std::unordered_map<std::string, Value>::const_iterator();
  while (s != nullptr && it == s->vars.end()) {
    s = s->parent;
    if (s != nullptr) it = s->vars.find(path[0]);
  }
  if (s == nullptr) return nullptr;
  const Value* v = &it->second;
  for (size_t k = 1; k < path.size(); k++) {
    if (v->isNull()) return nullptr;
    if (v->isArray()) {
      // JS arrays own 'length'.
      if (path[k] == "length") {
        // Length is a number, not a stored Value: stash it in a thread-local
        // holder so callers can still take its address. (Only used by textOf
        // and truthy, which read it immediately.)
        static thread_local Value lengthHolder;
        lengthHolder = Value::number(static_cast<double>(v->arr.size()));
        return &lengthHolder;
      }
      return nullptr;
    }
    if (!v->isObject()) return nullptr;
    v = v->find(path[k]);
    if (v == nullptr) return nullptr;
  }
  return v;
}

// An undefined value, for callers that need a Value reference.
static const Value kUndef = Value::undef();

std::string textOf(const Value& v) {
#ifdef NATIVE_STATS
  g_stats.textOf++;
#endif
  if (v.isString()) return v.s;
  if (v.isNumber()) return jsNumber(v.num);
  if (v.isBool()) return v.b ? "true" : "false";
  if (v.isArray()) {
    std::string out;
    for (size_t i = 0; i < v.arr.size(); i++) {
      if (i) out += ", ";
      out += textOf(v.arr[i]);
    }
    return out;
  }
  return "";
}

bool truthy(const Value& v) {
  if (v.isArray()) return !v.arr.empty();
  if (v.isUndefined() || v.isNull()) return false;
  if (v.isBool()) return v.b;
  if (v.isString()) return !v.s.empty();
  if (v.isNumber()) return v.num != 0;
  return true;  // objects (incl. empty) are truthy
}

struct RenderCtx {
  std::unordered_map<std::string, std::vector<std::unique_ptr<Node>>> templates;
};

void renderNodes(const std::vector<std::unique_ptr<Node>>& nodes, const Scope* scope,
                 const RenderCtx& ctx, int depth, std::string& out) {
#ifdef NATIVE_STATS
  g_stats.renderNodes++;
#endif
  for (const auto& np : nodes) {
    const Node& n = *np;
    switch (n.kind) {
      case Node::kText:
        out += n.text;
        break;
      case Node::kVar: {
        const Value* v = lookup(scope, n.path);
        if (v != nullptr && v->isString()) escapeHtmlInto(out, v->s);
        else escapeHtmlInto(out, textOf(v ? *v : kUndef));
        break;
      }
      case Node::kRaw: {
        const Value* v = lookup(scope, n.path);
        if (v != nullptr && v->isString()) out += v->s;
        else out += textOf(v ? *v : kUndef);
        break;
      }
      case Node::kIf: {
        const Value* v = lookup(scope, n.path);
        bool tv = truthy(v ? *v : kUndef);
        bool take = (tv != n.neg);
        if (take) {
          renderNodes(n.body, scope, ctx, depth, out);
        } else if (n.hasElse) {
          renderNodes(n.elseBody, scope, ctx, depth, out);
        }
        break;
      }
      case Node::kFor: {
        const Value* xs = lookup(scope, n.path);
        if (xs == nullptr || !xs->isArray()) break;
        std::unordered_map<std::string, Value> vars;
        Value loop = Value::object();
        loop.set("index", Value::number(0));
        loop.set("first", Value::boolean(false));
        loop.set("last", Value::boolean(false));
        loop.set("length", Value::number(static_cast<double>(xs->arr.size())));
        vars[n.name] = Value::undef();
        vars["loop"] = loop;
        Scope child;
        child.vars = vars;  // copied per iteration below via re-assignment
        child.parent = scope;
        // JS reuses ONE vars object per iteration; we keep `child.vars` as
        // that object and mutate it in place.
        for (size_t k = 0; k < xs->arr.size(); k++) {
          child.vars[n.name] = xs->arr[k];
          loop.set("index", Value::number(static_cast<double>(k) + 1));
          loop.set("first", Value::boolean(k == 0));
          loop.set("last", Value::boolean(k == xs->arr.size() - 1));
          child.vars["loop"] = loop;
          renderNodes(n.body, &child, ctx, depth, out);
        }
        break;
      }
      case Node::kInclude: {
        auto it = ctx.templates.find(n.name);
        if (it != ctx.templates.end() && depth < INCLUDE_DEPTH)
          renderNodes(it->second, scope, ctx, depth + 1, out);
        break;
      }
      case Node::kRoot:
        break;
    }
  }
}

// ---------------------------------------------------------------- the build

std::string dirOf(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? "" : path.substr(0, slash);
}

// `rel` from the directory parts `dirParts` of a source, or nullopt when it climbs out.
bool resolvePath(const std::vector<std::string>& dirParts, const std::string& rel, std::string& out) {
  std::vector<std::string> parts = dirParts;
  size_t start = 0;
  for (;;) {
    size_t slash = rel.find('/', start);
    std::string seg = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (seg.empty() || seg == ".") {
      // skip
    } else if (seg == "..") {
      if (parts.empty()) return false;
      parts.pop_back();
    } else {
      parts.push_back(seg);
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  out = "";
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) out += '/';
    out += parts[i];
  }
  return true;
}

// JS ToIntegerOrInfinity, then JS Array.prototype.slice index resolution.
long long sliceIndex(double d, long long len) {
  double rel;
  if (std::isnan(d)) rel = 0;
  else if (std::isinf(d)) rel = d;
  else rel = std::trunc(d);
  if (rel < 0) {
    double v = static_cast<double>(len) + rel;
    return v < 0 ? 0 : static_cast<long long>(v);
  }
  if (rel > static_cast<double>(len)) return len;
  return static_cast<long long>(rel);
}

template <typename T>
std::vector<T> jsSlice(const std::vector<T>& v, double startD, double endD) {
  long long len = static_cast<long long>(v.size());
  long long a = sliceIndex(startD, len);
  long long b = sliceIndex(endD, len);
  if (b < a) b = a;
  return std::vector<T>(v.begin() + a, v.begin() + b);
}

struct Tag {
  std::string name;
  std::string slug;
  std::string url;
  std::vector<struct Source*> posts;
};
struct Source {
  std::string path;
  std::string text;   // normalized (\n) source text
  Value data;         // front-matter object
  std::string body;
  bool post = false;
  std::string layout;
  std::string slug;
  std::string outDir;
  std::string url;
  std::string html;
  std::vector<TocEntry> toc;
  std::string excerpt;
  std::vector<std::shared_ptr<Tag>> tagRefs;
  Source* newer = nullptr;
  Source* older = nullptr;
  Value card;  // filled in step 7 for posts
};

std::string getStr(const Value& obj, const std::string& key, const std::string& dflt = "") {
  const Value* v = obj.find(key);
  if (v != nullptr && v->isString()) return v->s;
  return dflt;
}
double getNum(const Value& obj, const std::string& key, double dflt = 0) {
  const Value* v = obj.find(key);
  if (v != nullptr && v->isNumber()) return v->num;
  return dflt;
}

// The markdown link resolver (was a std::function lambda per source).
std::string resolveLink(const LinkEnv& env, const std::string& target) {
#ifdef NATIVE_STATS
  g_stats.link++;
#endif
  // scheme (/^[A-Za-z][A-Za-z0-9+.-]*:/) or starting '/' or '#'
  bool scheme = false;
  if (!target.empty() && isAlphaAscii(target[0])) {
    size_t i = 1;
    while (i < target.size() &&
           (isAlphaAscii(target[i]) || isDigit(target[i]) || target[i] == '+' ||
            target[i] == '.' || target[i] == '-')) i++;
    if (i < target.size() && target[i] == ':') scheme = true;
  }
  if (scheme || startsWith(target, "/") || startsWith(target, "#")) return target;
  size_t hash = target.find('#');
  std::string rel = hash == std::string::npos ? target : target.substr(0, hash);
  std::string frag = hash == std::string::npos ? "" : target.substr(hash);
  std::string path;
  bool hasPath = !rel.empty() && resolvePath(*env.dirParts, rel, path);
  // JS: path = rel === '' ? null : resolvePath(...); null when rel=='' or climbs out.
  bool pathNull = rel.empty() || !hasPath;
  if (!pathNull) {
    auto it = env.bySource->find(path);
    if (it != env.bySource->end()) return it->second->url + frag;
    if (env.staticPaths->count(path) != 0) return *env.base + "/" + path + frag;
  }
  env.broken->insert("broken-link " + env.self->path + " -> " + target);
  return target;
}

Value runImpl(const Value& input, const NormalizeFn& normalize) {
  Slugify slugify(normalize);
  RenderCtx ctx;
  std::vector<std::string> report;

  const Value* siteIn = input.find("site");
  const Value* templatesIn = input.find("templates");
  const Value* filesIn = input.find("files");
  Value siteObj = siteIn != nullptr ? *siteIn : Value::object();

  // Templates parsed in code-unit order of names; unparsable ones reported and left out.
  if (templatesIn != nullptr && templatesIn->isObject()) {
    std::vector<std::pair<std::string, std::string>> names;
    names.reserve(templatesIn->obj.size());
    for (auto& kv : templatesIn->obj) {
      if (kv.second.isString()) names.emplace_back(kv.first, kv.second.s);
    }
    std::sort(names.begin(), names.end(),
              [](const auto& a, const auto& b) { return cmpCU(a.first, b.first) < 0; });
    for (auto& kv : names) {
      try {
        ctx.templates[kv.first] = parseTemplate(kv.first, kv.second);
      } catch (const TemplateError& err) {
        report.push_back(err.what());
      }
    }
  }

  std::string base = getStr(siteObj, "base_url");
  while (!base.empty() && base.back() == '/') base.pop_back();

  Value output = Value::object();
  std::unordered_set<std::string> broken;

  // 1. Sources, in code-unit order of their paths, line ends made "\n".
  std::vector<std::pair<std::string, std::string>> files;
  if (filesIn != nullptr && filesIn->isArray()) {
    files.reserve(filesIn->arr.size());
    for (auto& f : filesIn->arr) {
      if (!f.isObject()) continue;
      files.emplace_back(getStr(f, "path"), getStr(f, "content"));
    }
  }
  std::stable_sort(files.begin(), files.end(),
                   [](const auto& a, const auto& b) { return cmpCU(a.first, b.first) < 0; });
  std::vector<Source> sources;
  sources.reserve(files.size());
  std::vector<std::pair<std::string, std::string>> statics;
  statics.reserve(files.size());
  for (auto& f : files) {
    if (endsWith(f.first, ".md")) {
      Source s;
      s.path = f.first;
      if (f.second.find('\r') == std::string::npos) {
        s.text = f.second;
      } else {
        // replace /\r\n?/g with '\n'
        std::string t;
        for (size_t i = 0; i < f.second.size(); i++) {
          if (f.second[i] == '\r') {
            t += '\n';
            if (i + 1 < f.second.size() && f.second[i + 1] == '\n') i++;
          } else {
            t += f.second[i];
          }
        }
        s.text = t;
      }
      sources.push_back(std::move(s));
    } else {
      statics.emplace_back(f.first, f.second);
    }
  }

  // 2. Front matter: a refused source is reported and dropped, and so is a draft.
  std::vector<Source> kept;
  kept.reserve(sources.size());
  for (auto& s : sources) {
    FmResult fm = parseFrontMatter(s.text);
    if (!fm.ok) {
      report.push_back("refused " + s.path + ": " + fm.error);
      continue;
    }
    s.data = fm.data;
    s.body = fm.body;
    s.post = startsWith(s.path, "posts/");
    std::string own;
    for (const std::string& d : RESERVED) {
      if (d != "posts" && startsWith(s.path, d + "/")) { own = d; break; }
    }
    if (!own.empty()) {
      report.push_back("refused " + s.path + ": " + own + "/ is the generator's own");
      continue;
    }
    if (s.post && !s.data.has("date")) {
      report.push_back("refused " + s.path + ": a post needs a date");
      continue;
    }
    const Value* layoutV = s.data.find("layout");
    std::string layout = layoutV != nullptr && layoutV->isString()
                             ? layoutV->s
                             : (s.post ? "post" : "page");
    if (ctx.templates.find(layout) == ctx.templates.end()) {
      report.push_back("refused " + s.path + ": layout \"" + layout + "\" is no template that parses");
      continue;
    }
    s.layout = layout;
    const Value* draftV = s.data.find("draft");
    if (draftV != nullptr && draftV->isBool() && draftV->b) {
      report.push_back("draft " + s.path);
      continue;
    }
    kept.push_back(std::move(s));
  }

  // 3. Permalinks: slug made unique in its directory, in source order.
  std::unordered_map<std::string, std::unordered_set<std::string>> taken;
  std::unordered_map<std::string, Source*> bySource;
  for (auto& s : kept) {
    std::string dir = s.post ? "posts" : dirOf(s.path);
    auto it = taken.find(dir);
    if (it == taken.end()) {
      std::unordered_set<std::string> fresh;
      if (dir.empty()) {
        for (const std::string& d : RESERVED) fresh.insert(d);
      }
      it = taken.emplace(dir, std::move(fresh)).first;
    }
    const Value* slugV = s.data.find("slug");
    std::string slug = slugV != nullptr && slugV->isString() ? slugV->s : slugify(getStr(s.data, "title"));
    slug = uniqueSlug(slug, it->second);
    s.slug = slug;
    s.outDir = dir.empty() ? slug : dir + "/" + slug;
    s.url = base + "/" + s.outDir + "/";
    bySource[s.path] = &s;
  }

  // 4. Markdown, its links resolved to the pages and static files they name.
  std::unordered_set<std::string> staticPaths;
  for (auto& f : statics) staticPaths.insert(f.first);
  for (auto& s : kept) {
    std::string dir = dirOf(s.path);
    std::vector<std::string> dirParts;
    if (!dir.empty()) {
      size_t start = 0;
      for (;;) {
        size_t slash = dir.find('/', start);
        dirParts.push_back(dir.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
      }
    }
    Source* self = &s;
    LinkEnv env;
    env.self = self;
    env.dirParts = &dirParts;
    env.bySource = &bySource;
    env.staticPaths = &staticPaths;
    env.base = &base;
    env.broken = &broken;
    MdResult md = renderMarkdown(s.body, env, slugify);
    s.html = md.html;
    s.toc = md.toc;
    s.excerpt = md.excerpt;
  }

  // 5. Collections: posts, newest first and then by slug; newer and older neighbours.
  std::vector<Source*> posts;
  posts.reserve(kept.size());
  for (auto& s : kept) if (s.post) posts.push_back(&s);
  std::sort(posts.begin(), posts.end(), [](Source* x, Source* y) {
    int c = cmpCU(getStr(y->data, "date"), getStr(x->data, "date"));
    if (c != 0) return c < 0;
    return cmpCU(x->slug, y->slug) < 0;
  });
  for (size_t k = 0; k < posts.size(); k++) {
    posts[k]->newer = k == 0 ? nullptr : posts[k - 1];
    posts[k]->older = k == posts.size() - 1 ? nullptr : posts[k + 1];
  }

  // 6. Tags: grouped by slug, each named as first met in the posts' order.
  std::unordered_map<std::string, std::shared_ptr<Tag>> bySlug;
  auto ownTags = [&slugify](Source& s) {
    std::vector<std::pair<std::string, std::string>> out;  // (name, slug)
    std::unordered_set<std::string> seen;
    const Value* tagsV = s.data.find("tags");
    if (tagsV != nullptr && tagsV->isArray()) {
      for (auto& tv : tagsV->arr) {
        if (!tv.isString()) continue;
        std::string slug = slugify(tv.s);
        if (seen.count(slug) != 0) continue;
        seen.insert(slug);
        out.emplace_back(tv.s, slug);
      }
    }
    return out;
  };
  for (Source* p : posts) {
    p->tagRefs.clear();
    for (auto& ns : ownTags(*p)) {
      auto it = bySlug.find(ns.second);
      std::shared_ptr<Tag> tag;
      if (it == bySlug.end()) {
        tag = std::make_shared<Tag>();
        tag->name = ns.first;
        tag->slug = ns.second;
        tag->url = base + "/tags/" + ns.second + "/";
        bySlug[ns.second] = tag;
      } else {
        tag = it->second;
      }
      tag->posts.push_back(p);
      p->tagRefs.push_back(tag);
    }
  }
  for (auto& s : kept) {
    if (s.post) continue;
    s.tagRefs.clear();
    for (auto& ns : ownTags(s)) {
      auto it = bySlug.find(ns.second);
      if (it != bySlug.end()) {
        s.tagRefs.push_back(it->second);
      } else {
        auto tag = std::make_shared<Tag>();
        tag->name = ns.first;
        tag->slug = ns.second;
        tag->url = "";
        s.tagRefs.push_back(tag);
      }
    }
  }
  std::vector<std::shared_ptr<Tag>> tags;
  for (auto& kv : bySlug) tags.push_back(kv.second);
  std::sort(tags.begin(), tags.end(), [](const std::shared_ptr<Tag>& x, const std::shared_ptr<Tag>& y) {
    if (x->posts.size() != y->posts.size()) return x->posts.size() > y->posts.size();
    return cmpCU(x->slug, y->slug) < 0;
  });

  // 7. Layouts.
  auto linkOf = [](Source* s) {
    if (s == nullptr) return Value::nullv();
    Value o = Value::object();
    o.set("title", Value::str(getStr(s->data, "title")));
    o.set("url", Value::str(s->url));
    const Value* dateV = s->data.find("date");
    o.set("date", Value::str(dateV != nullptr && dateV->isString() ? dateV->s : ""));
    o.set("date_long", Value::str(dateV != nullptr && dateV->isString() ? longDate(dateV->s) : ""));
    return o;
  };
  auto cardOf = [&linkOf](Source& s) {
    (void)linkOf;
    Value o = Value::object();
    o.set("title", Value::str(getStr(s.data, "title")));
    o.set("url", Value::str(s.url));
    const Value* dateV = s.data.find("date");
    o.set("date", Value::str(dateV != nullptr && dateV->isString() ? dateV->s : ""));
    o.set("date_long", Value::str(dateV != nullptr && dateV->isString() ? longDate(dateV->s) : ""));
    const Value* sumV = s.data.find("summary");
    o.set("summary", Value::str(sumV != nullptr && sumV->isString() ? sumV->s : ""));
    o.set("excerpt", Value::str(s.excerpt));
    long long rm = (wordsOf(s.body) + 199) / 200;
    if (rm < 1) rm = 1;
    o.set("reading_minutes", Value::number(static_cast<double>(rm)));
    Value tagArr = Value::array();
    for (auto& t : s.tagRefs) {
      Value to = Value::object();
      to.set("name", Value::str(t->name));
      to.set("slug", Value::str(t->slug));
      to.set("url", Value::str(t->url));
      tagArr.arr.push_back(to);
    }
    o.set("tags", tagArr);
    return o;
  };
  for (Source* p : posts) p->card = cardOf(*p);

  Value site = Value::object();
  site.set("title", siteIn != nullptr && siteIn->find("title") != nullptr ? *siteIn->find("title") : Value::str(""));
  site.set("base_url", Value::str(base));
  Value allTags = Value::array();
  for (auto& t : tags) {
    Value to = Value::object();
    to.set("name", Value::str(t->name));
    to.set("slug", Value::str(t->slug));
    to.set("url", Value::str(t->url));
    to.set("count", Value::number(static_cast<double>(t->posts.size())));
    allTags.arr.push_back(to);
  }
  double recentD = getNum(siteObj, "recent");
  std::vector<Source*> recentPosts = jsSlice(posts, 0, recentD);
  Value recent = Value::array();
  for (Source* p : recentPosts) recent.arr.push_back(p->card);

  Scope baseScope;
  baseScope.vars["site"] = site;
  baseScope.vars["tags"] = allTags;
  baseScope.vars["recent"] = recent;
  baseScope.parent = nullptr;

  auto render = [&](const std::string& name, std::unordered_map<std::string, Value> vars,
                    std::string& outStr) {
    auto it = ctx.templates.find(name);
    if (it == ctx.templates.end()) return false;
    Scope scope;
    scope.vars = std::move(vars);
    scope.parent = &baseScope;
    outStr = "";
    renderNodes(it->second, &scope, ctx, 0, outStr);
    return true;
  };

  for (auto& s : kept) {
    Value page;
    if (s.post) {
      page = s.card;  // spread ...card first
    } else {
      page = Value::object();
      page.set("title", Value::str(getStr(s.data, "title")));
      page.set("url", Value::str(s.url));
      const Value* dateV = s.data.find("date");
      page.set("date", Value::str(dateV != nullptr && dateV->isString() ? dateV->s : ""));
      page.set("date_long", Value::str(dateV != nullptr && dateV->isString() ? longDate(dateV->s) : ""));
      const Value* sumV = s.data.find("summary");
      page.set("summary", Value::str(sumV != nullptr && sumV->isString() ? sumV->s : ""));
      page.set("excerpt", Value::str(s.excerpt));
      long long rm = (wordsOf(s.body) + 199) / 200;
      if (rm < 1) rm = 1;
      page.set("reading_minutes", Value::number(static_cast<double>(rm)));
      Value tagArr = Value::array();
      for (auto& t : s.tagRefs) {
        Value to = Value::object();
        to.set("name", Value::str(t->name));
        to.set("slug", Value::str(t->slug));
        to.set("url", Value::str(t->url));
        tagArr.arr.push_back(to);
      }
      page.set("tags", tagArr);
    }
    page.set("slug", Value::str(s.slug));
    page.set("content", Value::str(s.html));
    Value tocArr = Value::array();
    for (auto& h : s.toc) {
      Value ho = Value::object();
      ho.set("level", Value::number(h.level));
      ho.set("id", Value::str(h.id));
      ho.set("html", Value::str(h.html));
      tocArr.arr.push_back(ho);
    }
    page.set("toc", tocArr);
    page.set("newer", linkOf(s.newer));
    page.set("older", linkOf(s.older));
    page.set("is_post", Value::boolean(s.post));
    std::string html;
    std::unordered_map<std::string, Value> vars;
    vars["page"] = page;
    if (render(s.layout, std::move(vars), html)) {
      output.set(s.outDir + "/index.html", Value::str(html));
    } else {
      output.set(s.outDir + "/index.html", Value::nullv());
    }
  }

  double per = getNum(siteObj, "per_page");
  // per_page <= 0 cannot occur from the generator (it would make JS loop
  // forever on pages = ceil(n/0)); clamp to >= 1 as a safety net.
  if (!(per > 0)) per = 1;
  double pagesD = std::ceil(static_cast<double>(posts.size()) / per);
  long long pages = 1;
  if (std::isfinite(pagesD) && pagesD > 1) pages = static_cast<long long>(pagesD);
  auto indexUrl = [&base](long long k) {
    return k == 1 ? base + "/" : base + "/page/" + std::to_string(k) + "/";
  };
  for (long long k = 1; k <= pages; k++) {
    std::vector<Source*> slice = jsSlice(posts, static_cast<double>(k - 1) * per,
                                         static_cast<double>(k) * per);
    Value postsArr = Value::array();
    for (Source* p : slice) postsArr.arr.push_back(p->card);
    Value pager = Value::object();
    pager.set("number", Value::number(static_cast<double>(k)));
    pager.set("total", Value::number(static_cast<double>(pages)));
    pager.set("url", Value::str(indexUrl(k)));
    pager.set("newer_url", Value::str(k == 1 ? "" : indexUrl(k - 1)));
    pager.set("older_url", Value::str(k == pages ? "" : indexUrl(k + 1)));
    std::unordered_map<std::string, Value> vars;
    vars["posts"] = postsArr;
    vars["pager"] = pager;
    std::string html;
    if (render("index", std::move(vars), html)) {
      output.set(k == 1 ? "index.html" : "page/" + std::to_string(k) + "/index.html",
                 Value::str(html));
    }
  }
  for (auto& t : tags) {
    Value tagObj = Value::object();
    tagObj.set("name", Value::str(t->name));
    tagObj.set("slug", Value::str(t->slug));
    tagObj.set("url", Value::str(t->url));
    tagObj.set("count", Value::number(static_cast<double>(t->posts.size())));
    Value postsArr = Value::array();
    for (Source* p : t->posts) postsArr.arr.push_back(p->card);
    tagObj.set("posts", postsArr);
    std::unordered_map<std::string, Value> vars;
    vars["tag"] = tagObj;
    std::string html;
    if (render("tag", std::move(vars), html)) {
      output.set("tags/" + t->slug + "/index.html", Value::str(html));
    }
  }
  {
    std::unordered_map<std::string, Value> vars;
    std::string html;
    if (render("tags", std::move(vars), html)) output.set("tags/index.html", Value::str(html));
  }
  for (auto& f : statics) {
    if (output.has(f.first)) {
      report.push_back("shadowed " + f.first + ": a built page has its path");
    } else {
      output.set(f.first, Value::str(f.second));
    }
  }

  // 8. The report.
  std::vector<std::string> lines = report;
  for (auto& b : broken) lines.push_back(b);
  std::sort(lines.begin(), lines.end(), [](const std::string& a, const std::string& b) {
    return cmpCU(a, b) < 0;
  });
  long long pageCount = 0;
  for (auto& s : kept) if (!s.post) pageCount++;
  lines.push_back("built " + std::to_string(pageCount) + " pages, " +
                  std::to_string(posts.size()) + " posts, " + std::to_string(tags.size()) +
                  " tags and " + std::to_string(statics.size()) + " static files");
  std::string reportText;
  for (size_t i = 0; i < lines.size(); i++) {
    if (i) reportText += '\n';
    reportText += lines[i];
  }
  reportText += '\n';
  output.set(REPORT_FILE, Value::str(reportText));
  return output;
}

}  // namespace

Value runSite(const Value& input, const NormalizeFn& normalize) {
  return runImpl(input, normalize);
}
