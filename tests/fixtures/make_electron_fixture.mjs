// Builds the generated parts of the Electron fixture with the real tools:
//   - electron-app/renderer/bundle.js(.map): esbuild bundle of src/*.ts,
//     minified, with a linked source map (sourceMappingURL comment);
//   - electron-app.asar: the app packed by @electron/asar (with integrity);
//   - electron-app-expected.json: source-map lookups computed by Node's own
//     node:module SourceMap, which the Cortex decoder must reproduce.
//
//   npm install --prefix <dir> esbuild@0.25.12 @electron/asar@4.3.1
//   node tests/fixtures/make_electron_fixture.mjs <dir>/node_modules [--asar-only <out.asar>]
//
// CI runs it with --asar-only to pack a fresh archive for the E2E test.
import { createRequire } from 'node:module';
import { SourceMap } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const app = path.join(here, 'electron-app');
const modules = path.resolve(process.argv[2] ?? 'node_modules');
const require = createRequire(path.join(modules, 'noop.js'));
const asar = require('@electron/asar');

const asarOnly = process.argv.indexOf('--asar-only');
if (asarOnly !== -1) {
  await asar.createPackage(app, path.resolve(process.argv[asarOnly + 1]));
  process.exit(0);
}

const esbuild = require('esbuild');
await esbuild.build({
  entryPoints: [path.join(app, 'src', 'renderer.ts')],
  bundle: true,
  minify: true,
  format: 'iife',
  target: 'es2020',
  sourcemap: 'linked',
  sourcesContent: true,
  outfile: path.join(app, 'renderer', 'bundle.js'),
  logLevel: 'warning',
});

// Positions in the bundle of a few distinctive literals, and where Node's
// SourceMap says they come from. Lines and columns are 1-based here.
const bundle = fs.readFileSync(path.join(app, 'renderer', 'bundle.js'), 'utf8');
const map = new SourceMap(JSON.parse(fs.readFileSync(path.join(app, 'renderer', 'bundle.js.map'), 'utf8')));
const lookups = [];
for (const needle of ['"theme"', 'Hello from the ', 'e<t?t', '.log(', 'getSetting']) {
  const offset = bundle.indexOf(needle);
  if (offset < 0) throw new Error('not in bundle: ' + needle);
  const before = bundle.slice(0, offset);
  const line = before.split('\n').length;
  const column = offset - (before.lastIndexOf('\n') + 1);
  const entry = map.findEntry(line - 1, column);
  lookups.push({
    needle,
    generated: { line, column: column + 1 },
    original: entry.originalSource === undefined ? null : {
      source: entry.originalSource,
      line: entry.originalLine + 1,
      column: entry.originalColumn + 1,
      name: entry.name ?? null,
    },
  });
}
// Every column of every generated line, so the decoder is compared with
// Node on all positions, not only on a few chosen ones.
const columns = [];
bundle.split('\n').forEach((text, index) => {
  for (let column = 0; column <= text.length; ++column) {
    const entry = map.findEntry(index, column);
    columns.push(entry.originalSource === undefined
      ? [index + 1, column + 1, null]
      : [index + 1, column + 1, entry.originalSource, entry.originalLine + 1, entry.originalColumn + 1, entry.name ?? null]);
  }
});
fs.writeFileSync(path.join(here, 'electron-app-expected.json'),
  JSON.stringify({ lookups, columns: columns.map((row) => JSON.stringify(row)) }, null, 1) + '\n');
await asar.createPackage(app, path.join(here, 'electron-app.asar'));
console.log('fixture built:', lookups.length, 'lookups');
