// C++ port of build/solve.js (makefun-site 1.0.0 drop-in).
// runSite(input, normalize) mirrors JS run(input) exactly: same checks in the
// same order, same error strings, same sorting (compareCodeUnits), same
// insertion order for the output object. Strings are UTF-8; normalize is the
// NFKD callback (implemented by the caller).
#pragma once
#include <string>
#include "value.h"

// input is one parsed site object:
//   {site:{title,base_url,per_page,recent},
//    templates:{name:source}, files:[{path,content}]}
// Returns the output object (insertion-ordered path -> text).
Value runSite(const Value& input, const NormalizeFn& normalize);
