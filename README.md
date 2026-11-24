# site-generator-speed-01 — native entry v5 (C++ N-API addon)

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
v5 adds three speed techniques over the v4 native entry: SSE2 scanning in the
HTML-escape and JSON string/quoting loops, LTO plus `-fvisibility=hidden`
and `-fno-plt` in the build, and a streaming batch path that passes file
paths to the addon instead of one big string.

## Layout

- `addon.cc` — raw N-API wrapper: `runCases(text, normalizeFn) -> answers text`
  and `runBatch(inputPath, outputPath, normalizeFn) -> case count`. The batch
  path reads cases.json itself (open/read), processes one case at a time
  (parse, build, serialize, append to the output file), and releases each
  case's memory when its iteration ends.
- `core.h` / `core.cpp` — the generation core: a faithful C++ port of our
  proven JS `build/solve.js` (front matter, template engine, Markdown subset,
  slug rule, link resolution, build report).
- `json.h` / `json.cpp` — fast JSON parser/serialiser matching JS semantics.
- `value.h` / `util.cpp` — shared value type and JS-semantics helpers
  (JS number formatting, UTF-16 code-unit ordering, whitespace). `value.h`
  also holds the SSE2 delimiter scans shared by the HTML-escape and JSON
  loops (scalar fallback for the tail and for non-x86_64).
- `main.ts` — the harness program: passes the cases.json and answers.json
  file paths to the addon's `runBatch`. No JSON.parse / JSON.stringify in JS,
  and the answers text is never held as one JS string.
- `solve.js` — thin loader keeping the solve.js contract (`run(input)`),
  plus `runBatch(inputFile, outputFile)` for the batch path.
- `addon.node` — the compiled addon (x86_64, N-API v8), committed as a
  prebuilt binary. Reproduced by `./build.sh`.

## Build

```sh
./build.sh   # g++ -O3 -flto -fvisibility=hidden -fno-plt, needs /usr/include/node
```

## Verify (measured 24 Nov ~02:30 WIB, this VM, Node 24)

- Practice 3/3 vs `practice/reference/expected.json`, raw bytes identical.
- Differential fuzz vs the JS baseline (`build/solve.js`): 10,700 cases
  (seeds 1–20, 535 each), 0 mismatches. Cross-check vs the kit baseline
  (`makefun-site` itself): seeds 1–5, 0 mismatches.
- Targeted edge cases 10/10. Batch-path differential test (runBatch vs
  runCases vs JS baseline, incl. empty array and single case): 10/10.
  Determinism: two runs byte-identical. Clean rebuild reproduces addon.node
  byte-identically (sha256 7dd7dd0d4d5656f6193ca31d22556e89b461d5acf4b9f3b036dc791607d9ebfe).
- Cold end-to-end (fresh process, `taskset -c 0`, 1 CPU): v5 median 308 ms,
  null entry median 184 ms, net +124 ms. Warm addon: runBatch 135 ms/call,
  runCases 165 ms/call. The three techniques together cut the local net from
  ~225 ms (v4) to ~124 ms: SSE2+LTO took it to ~178 ms, the streaming batch
  to ~124 ms.
- Projected grader net ~0.62 s (~620,000 µs, rank ~220): faster than the
  v4 native (~814,000 µs, rank ~320), the JS v2 (1,410,000 µs, rank 2561)
  and the flag-only v3 (~1,010,000 µs, rank ~455), but NOT top 10 (0 µs
  needs the addon's extra CPU ≲ 5 ms). The remaining time is spread thin
  across the template/markdown engine (no single hotspot); reaching 0 µs
  needs a ground-up rewrite, not further tuning of this port. Verdict:
  correct and competitive, not top-10 ready.
