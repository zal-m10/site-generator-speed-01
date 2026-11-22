# site-generator-speed

![site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)
![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/zal-m10/site-generator-speed-01/rank.svg)
![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/zal-m10/site-generator-speed-01/reward.svg)

[site-generator-speed-01 on make.fun](https://make.fun/bounty/site-generator-speed-01-run-20261122)

A faster drop-in for makefun-site 1.0.0. `solve.js` answers exactly as
makefun-site does on every case — the same output tree, byte for byte — but
in about half the CPU time. `main.ts` is the harness driver: it reads every
case from `/input/cases.json`, answers each with `run`, and writes
`/output/answers.json`.

## How it is faster

The reference is rewritten from its specification with the slow parts
replaced by faster equivalents that return the same answer:

- HTML escaping takes a fast path when nothing needs escaping, and plain
  runs of inline text are escaped in bulk instead of one character at a time.
- Slugs skip Unicode normalisation for ASCII text and are cached per run.
- Front matter is read without splitting and re-joining the whole body.
- Markdown is lexed with direct character checks instead of a regex per
  line, and inline rendering is driven by a regex that jumps straight to
  the next special character.
- Word counting uses a plain character scan instead of building a match
  array.
- Template rendering writes into one output array instead of concatenating
  a string per node.

Node 22 and its standard library only. No dependencies, no network, no
configuration, deterministic.
