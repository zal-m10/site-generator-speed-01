// N-API addon for the native site-generator entry.
// Two functions:
//   runCases(text, normalizeFn) -> answers text. The whole cases.json file as
//     a JS string (array of {id, input}); used by solve.js run(input).
//   runBatch(inputPath, outputPath, normalizeFn) -> case count. The graded
//     batch path: C++ reads cases.json itself, processes one case at a time
//     (parse, build, serialize, write) and appends each case's answers JSON
//     to the output file as it goes, so the multi-megabyte answers text is
//     never held as one JS string and each case's Values are released when
//     its iteration ends.
//   normalizeFn: (s) => s.normalize('NFKD') — JS's own normalisation, so the
//     slug rule matches the sandbox's Node exactly. Called only for non-ASCII
//     text (the C++ slugify fast path skips it for ASCII).
// All parsing, generation and serialisation happen in C++; the JS side never
// calls JSON.parse or JSON.stringify on the graded path (see main.ts).
#define NODE_GYP_MODULE_NAME addon
#define NAPI_VERSION 8
#include <node_api.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include "core.h"
#include "json.h"

namespace {

std::string normalizeViaJs(napi_env env, napi_value fn, napi_value recv, const std::string& s) {
  napi_value jsStr;
  if (napi_create_string_utf8(env, s.data(), s.size(), &jsStr) != napi_ok) return s;
  napi_value argv[1] = {jsStr};
  napi_value result = nullptr;
  // recv must be a valid JS value: napi_call_function rejects a null recv
  // (napi_invalid_arg), which used to make every normalize call silently
  // return its input unchanged. The global object is the neutral receiver
  // for the arrow-function callback.
  if (napi_call_function(env, recv, fn, 1, argv, &result) != napi_ok) return s;
  size_t len = 0;
  if (napi_get_value_string_utf8(env, result, nullptr, 0, &len) != napi_ok) return s;
  std::string out(len, '\0');
  if (napi_get_value_string_utf8(env, result, out.data(), len + 1, &len) != napi_ok) return s;
  out.resize(len);
  return out;
}

// Read a JS string argument as a UTF-8 std::string. Returns "" on failure
// (the caller then fails on the empty path, as it would for a bad argument).
std::string napiUtf8(napi_env env, napi_value v) {
  size_t len = 0;
  if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) return "";
  std::string out(len, '\0');
  if (napi_get_value_string_utf8(env, v, out.data(), len + 1, &len) != napi_ok) return "";
  out.resize(len);
  return out;
}

napi_value RunCases(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 1) {
    napi_throw_type_error(env, nullptr, "runCases(text, normalizeFn)");
    return nullptr;
  }
  size_t len = 0;
  if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len) != napi_ok) return nullptr;
  std::string text(len, '\0');
  if (napi_get_value_string_utf8(env, argv[0], text.data(), len + 1, &len) != napi_ok) return nullptr;
  text.resize(len);

  napi_value normFn = (argc >= 2) ? argv[1] : nullptr;
  bool hasFn = false;
  if (normFn) {
    napi_valuetype t;
    if (napi_typeof(env, normFn, &t) == napi_ok && t == napi_function) hasFn = true;
  }
  napi_value global = nullptr;
  napi_get_global(env, &global);
  NormalizeFn normalize = [env, normFn, hasFn, global](const std::string& s) -> std::string {
    if (!hasFn) return s;  // no callback: identity (correct for ASCII text)
    return normalizeViaJs(env, normFn, global, s);
  };

  try {
    Value cases = jsonParse(text);
    if (!cases.isArray()) throw std::runtime_error("cases.json is not an array");
    Value answers = Value::array();
    answers.arr.reserve(cases.arr.size());
    for (const Value& c : cases.arr) {
      const Value* input = c.find("input");
      if (!input) throw std::runtime_error("case has no input");
      answers.arr.push_back(runSite(*input, normalize));
    }
    std::string out = jsonStringify(answers);
    napi_value result = nullptr;
    if (napi_create_string_utf8(env, out.data(), out.size(), &result) != napi_ok) return nullptr;
    return result;
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }
}

// Read a whole file with open/read. Throws std::runtime_error on failure.
std::string readWholeFile(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("open input: " + std::string(std::strerror(errno)));
  std::string data;
  char buf[1 << 16];
  for (;;) {
    ssize_t got = ::read(fd, buf, sizeof buf);
    if (got < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      ::close(fd);
      throw std::runtime_error("read input: " + std::string(std::strerror(e)));
    }
    if (got == 0) break;
    data.append(buf, static_cast<size_t>(got));
  }
  ::close(fd);
  return data;
}

// Write all bytes to fd, handling partial writes and EINTR. Throws on failure.
void writeAllFd(int fd, const char* data, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, data, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("write output: " + std::string(std::strerror(errno)));
    }
    if (w == 0) throw std::runtime_error("write output: zero-length write");
    data += w;
    n -= static_cast<size_t>(w);
  }
}
void writeAllFd(int fd, const std::string& s) { writeAllFd(fd, s.data(), s.size()); }

napi_value RunBatch(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 3) {
    napi_throw_type_error(env, nullptr, "runBatch(inputPath, outputPath, normalizeFn)");
    return nullptr;
  }
  std::string inputPath = napiUtf8(env, argv[0]);
  std::string outputPath = napiUtf8(env, argv[1]);

  napi_value normFn = argv[2];
  bool hasFn = false;
  if (normFn) {
    napi_valuetype t;
    if (napi_typeof(env, normFn, &t) == napi_ok && t == napi_function) hasFn = true;
  }
  napi_value global = nullptr;
  napi_get_global(env, &global);
  NormalizeFn normalize = [env, normFn, hasFn, global](const std::string& s) -> std::string {
    if (!hasFn) return s;  // no callback: identity (correct for ASCII text)
    return normalizeViaJs(env, normFn, global, s);
  };

  int fd = -1;
  try {
    std::string data = readWholeFile(inputPath);
    fd = ::open(outputPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) throw std::runtime_error("open output: " + std::string(std::strerror(errno)));
    writeAllFd(fd, "[", 1);
    size_t pos = jsonSkipWs(data, 0);
    if (pos >= data.size() || data[pos] != '[')
      throw std::runtime_error("cases.json is not an array");
    pos++;
    pos = jsonSkipWs(data, pos);
    size_t count = 0;
    if (pos < data.size() && data[pos] == ']') {
      pos++;
    } else {
      for (;;) {
        // One case at a time: parse it, build its site, serialize its
        // answers JSON, append it to the output file. The case's Values
        // (parsed case, output tree, serialized text) are locals of this
        // block, so their memory is released when the iteration ends —
        // nothing accumulates across cases.
        Value c = jsonParseOne(data, pos);
        const Value* input = c.find("input");
        if (!input) throw std::runtime_error("case has no input");
        Value out = runSite(*input, normalize);
        std::string json = jsonStringify(out);
        if (count > 0) writeAllFd(fd, ",", 1);
        writeAllFd(fd, json);
        count++;
        pos = jsonSkipWs(data, pos);
        if (pos >= data.size()) throw std::runtime_error("cases array is not closed");
        char tok = data[pos++];
        if (tok == ']') break;
        if (tok != ',') throw std::runtime_error("cases separator is invalid");
      }
    }
    pos = jsonSkipWs(data, pos);
    if (pos != data.size()) throw std::runtime_error("trailing characters");
    writeAllFd(fd, "]", 1);
    if (::close(fd) < 0) {
      fd = -1;
      throw std::runtime_error("close output failed");
    }
    fd = -1;
    napi_value result = nullptr;
    if (napi_create_uint32(env, static_cast<uint32_t>(count), &result) != napi_ok) return nullptr;
    return result;
  } catch (const std::exception& e) {
    if (fd >= 0) ::close(fd);
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }
}

}  // namespace

NAPI_MODULE_INIT() {
  napi_value fn = nullptr;
  if (napi_create_function(env, "runCases", NAPI_AUTO_LENGTH, RunCases, nullptr, &fn) != napi_ok)
    return nullptr;
  if (napi_set_named_property(env, exports, "runCases", fn) != napi_ok) return nullptr;
  napi_value fb = nullptr;
  if (napi_create_function(env, "runBatch", NAPI_AUTO_LENGTH, RunBatch, nullptr, &fb) != napi_ok)
    return nullptr;
  if (napi_set_named_property(env, exports, "runBatch", fb) != napi_ok) return nullptr;
  return exports;
}
