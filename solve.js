// Native drop-in for makefun-site 1.0.0: run(input) returns exactly the output
// tree makefun-site 1.0.0 returns, byte for byte. The work is done by addon.node
// (C++, built by build.sh); this module keeps the solve.js contract the checker
// expects. The graded path (main.ts) calls the addon directly with the raw
// cases text, so it never pays a JS JSON.parse / JSON.stringify.

import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const addon = require('./addon.node');

const HAS_SPECIAL = /[&<>"']/;
const ESC_TABLE = new Array(128).fill(null);
ESC_TABLE[38] = '&amp;';
ESC_TABLE[60] = '&lt;';
ESC_TABLE[62] = '&gt;';
ESC_TABLE[34] = '&quot;';
ESC_TABLE[39] = '&#39;';

/** text with & < > " and ' escaped. Fast path when nothing needs escaping. */
export function escapeHtml(text) {
  if (!HAS_SPECIAL.test(text)) return text;
  let out = '';
  let start = 0;
  for (let i = 0; i < text.length; i++) {
    const e = ESC_TABLE[text.charCodeAt(i)];
    if (e) {
      out += text.slice(start, i) + e;
      start = i + 1;
    }
  }
  return out + text.slice(start);
}

/** One site's output tree: path -> text. */
export function run(input) {
  const text = JSON.stringify([{ id: 'x', input }]);
  const out = JSON.parse(addon.runCases(text, (s) => s.normalize('NFKD')));
  return out[0];
}
