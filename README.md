# site-generator-speed-01 — native entry (C++ N-API addon)

![site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)
![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/zal-m10/site-generator-speed-01/rank.svg)
![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/zal-m10/site-generator-speed-01/reward.svg)

[site-generator-speed-01 on make.fun](https://make.fun/bounty/site-generator-speed-01-run-20261122)

A native drop-in for makefun-site 1.0.0, aiming at the top of the board. The
Rules now state that WebAssembly, native addons and prebuilt binaries are
graded and ranked like any other entry, and the heat nets off a null entry's
startup: to read 0 µs the entry's extra CPU beyond a bare Node startup must be
a few ms for 3 cases. A JS entry cannot do that (JS JSON.parse of the input
alone is ~20 ms), so all parsing, generation and serialisation happen in C++.

## Layout

- `addon.cc` — raw N-API wrapper: `runCases(text, normalizeFn) -> answers text`.
- `core.h` / `core.cpp` — the generation core: a faithful C++ port of our
  proven JS `build/solve.js` (front matter, template engine, Markdown subset,
  slug rule, link resolution, build report).
- `json.h` / `json.cpp` — fast JSON parser/serialiser matching JS semantics.
- `value.h` / `util.cpp` — shared value type and JS-semantics helpers
  (JS number formatting, UTF-16 code-unit ordering, whitespace).
- `main.ts` — the harness program: reads cases.json as one raw string, calls
  the addon once, writes answers.json. No JSON.parse / JSON.stringify in JS.
- `solve.js` — thin loader keeping the solve.js contract (`run(input)`).
- `addon.node` — the compiled addon (x86_64, N-API v8), committed as a
  prebuilt binary. Reproduced by `./build.sh`.

## Build

```sh
./build.sh   # g++ -O3, needs /usr/include/node (node headers)
```

## Verify (measured 24 Nov ~00:00 WIB, this VM, Node 24)

- Practice 3/3 vs `practice/reference/expected.json`, raw bytes identical.
- Differential fuzz vs the JS baseline (`build/solve.js`): 10,700 cases
  (seeds 1–20, 535 each), 0 mismatches.
- Targeted edge cases 10/10. Determinism: two runs byte-identical.
- Cold end-to-end (fresh process, `taskset -c 0`, 1 CPU): native median
  385 ms, null entry median 184 ms, net +201 ms. Warm addon: 184 ms/call
  (jsonParse ~7 ms, runSite ~131 ms, jsonStringify ~27 ms).
- Projected grader net ~0.81 s (~814,000 µs, rank ~350–400): faster than
  the JS v2 (1,410,000 µs, rank 2561) and the flag-only v3 (~1,010,000 µs,
  rank ~455), but NOT top 10 (0 µs needs the addon's extra CPU ≲ 5 ms).
  The remaining time is spread thin across the template/markdown engine
  (no single hotspot); reaching 0 µs needs a ground-up rewrite, not
  further tuning of this port. Verdict: correct and competitive, not
  top-10 ready.
