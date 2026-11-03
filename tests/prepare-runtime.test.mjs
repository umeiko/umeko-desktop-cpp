import test from 'node:test';
import assert from 'node:assert/strict';
import {execFile} from 'node:child_process';
import {existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {promisify} from 'node:util';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const execute = promisify(execFile);
const write = (base, path, content) => {
  const file = join(base, path);
  mkdirSync(dirname(file), {recursive: true});
  writeFileSync(file, content);
};

test('existing runtime projections gain newly required dependencies and adapters', async () => {
  const directory = mkdtempSync(join(tmpdir(), 'umeko-projection-'));
  const source = join(directory, 'source'), output = join(directory, 'output');
  try {
    for (const name of ['@earendil-works/pi-coding-agent', '@gotgenes/pi-subagents', '@alexlikevibe/pi-bg', 'marked', 'shell-quote'])
      write(source, `node_modules/${name}/package.json`, JSON.stringify({name, version: '1.0.0'}));
    write(output, 'manifest.json', JSON.stringify({projection: 2, engine: 'full-coding-agent'}));
    write(output, 'node_modules/@gotgenes/pi-subagents/package.json', JSON.stringify({name: '@gotgenes/pi-subagents', version: '1.0.0'}));
    write(output, 'node_modules/@earendil-works/pi-ai/dist/utils/retry.js', 'const errorMessage = message.errorMessage;\n');
    mkdirSync(join(output, 'node_modules/@earendil-works/pi-ai/dist/providers'), {recursive: true});
    await execute(process.execPath, [join(root, 'scripts/prepare-runtime.mjs'), '--source', source, '--output', output], {cwd: root});
    for (const name of ['@gotgenes/pi-subagents', '@alexlikevibe/pi-bg', 'marked', 'shell-quote'])
      assert.equal(JSON.parse(readFileSync(join(output, 'node_modules', name, 'package.json'), 'utf8')).version, '1.0.0');
    for (const name of ['provider-errors.mjs', 'process-guard.mjs']) {
      assert.ok(existsSync(join(output, name)), name);
      assert.equal(readFileSync(join(output, name), 'utf8'), readFileSync(join(root, 'runtime', name), 'utf8'));
    }
  } finally {
    rmSync(directory, {recursive: true, force: true});
  }
});
