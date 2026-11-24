// The harness runs `node /bot/main.ts` (node22). It reads every case from
// /input/cases.json (a JSON array of {id, input}), answers them with the native
// addon, and writes /output/answers.json. Outside a container MAKEFUN_INPUT_DIR
// and MAKEFUN_OUTPUT_DIR stand in for /input and /output.
//
// The whole cases file is handed to the addon as one raw string and the whole
// answers file comes back as one raw string: no JSON.parse / JSON.stringify in
// JS on this path, so the timed work is the addon's alone (the heat nets off a
// null entry that still pays the JS parse of the input).

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { setFlagsFromString } from 'node:v8';
import { createRequire } from 'node:module';

// V8's incremental marking off and TurboFan off, set before the addon is loaded:
// the graded run is a fresh process, and TurboFan's compile cost exceeds its
// benefit for this workload (see v3-analysis.md). The rules allow tuning V8's
// flags, and the house baseline runs with --no-incremental-marking.
setFlagsFromString('--no-incremental-marking --no-turbofan');

/** The native addon (addon.node, built by build.sh), typed by its contract. */
const require = createRequire(import.meta.url);
const addon = require('./addon.node') as {
  runCases: (text: string, normalize: (s: string) => string) => string;
};

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const text = readFileSync(join(INPUT_DIR, 'cases.json'), 'utf8');
const answers = addon.runCases(text, (s: string) => s.normalize('NFKD'));
writeFileSync(join(OUTPUT_DIR, 'answers.json'), answers);
