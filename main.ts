// The harness runs `node /bot/main.ts` (node22). It answers every case in
// /input/cases.json and writes /output/answers.json. Outside a container
// MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for /input and /output.
//
// The batch goes to the addon as file paths: C++ reads cases.json itself,
// processes one case at a time (parse, build, serialize, write) and appends
// each case's answers JSON to the output file as it goes. No JSON.parse /
// JSON.stringify in JS on this path, and the multi-megabyte answers text is
// never held as one JS string, so the timed work is the addon's alone (the
// heat nets off a null entry that still pays the JS parse of the input).

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
  runBatch: (inputPath: string, outputPath: string, normalize: (s: string) => string) => number;
};

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

addon.runBatch(
  join(INPUT_DIR, 'cases.json'),
  join(OUTPUT_DIR, 'answers.json'),
  (s: string) => s.normalize('NFKD'),
);
