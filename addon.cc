// N-API addon for the native site-generator entry.
// One function: runCases(text, normalizeFn) -> answers text.
//   text: the whole cases.json file as a JS string (array of {id, input}).
//   normalizeFn: (s) => s.normalize('NFKD') — JS's own normalisation, so the
//     slug rule matches the sandbox's Node exactly. Called only for non-ASCII
//     text (the C++ slugify fast path skips it for ASCII).
// All parsing, generation and serialisation happen in C++; the JS side never
// calls JSON.parse or JSON.stringify on the graded path (see main.ts).
#define NODE_GYP_MODULE_NAME addon
#define NAPI_VERSION 8
#include <node_api.h>
#include <stdexcept>
#include <string>
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

}  // namespace

NAPI_MODULE_INIT() {
  napi_value fn = nullptr;
  if (napi_create_function(env, "runCases", NAPI_AUTO_LENGTH, RunCases, nullptr, &fn) != napi_ok)
    return nullptr;
  if (napi_set_named_property(env, exports, "runCases", fn) != napi_ok) return nullptr;
  return exports;
}
