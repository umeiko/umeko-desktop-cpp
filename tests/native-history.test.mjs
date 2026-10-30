import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtempSync, mkdirSync } from 'node:fs';
import { rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { SessionManager } from '../runtime/releases/coding-1.3.0/node_modules/@earendil-works/pi-coding-agent/dist/index.js';

test('native desktop receives the entire history beyond the Python paging threshold', { timeout: 60000 }, async () => {
  const temp = mkdtempSync(join(tmpdir(), 'umeko-native-history-'));
  const workspace = join(temp, 'project'), agentDir = join(temp, 'agent'), sessionDir = join(temp, 'sessions');
  for (const folder of [workspace, agentDir, sessionDir]) mkdirSync(folder);
  const manager = SessionManager.create(workspace, sessionDir);
  const cost = { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 };
  for (let i = 0; i < 300; i++) {
    manager.appendMessage({ role: 'user', content: [{ type: 'text', text: `Question ${i}` }], timestamp: Date.now() });
    manager.appendMessage({ role: 'assistant', content: [{ type: 'text', text: `Answer ${i}` }],
      api: 'openai-completions', provider: 'umeko', model: 'fixture', stopReason: 'stop', timestamp: Date.now(),
      usage: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, totalTokens: 0, cost } });
  }
  const bridge = fileURLToPath(new URL('../runtime/releases/coding-1.3.0/bridge.mjs', import.meta.url));
  const child = spawn(process.execPath, [bridge], { cwd: workspace, stdio: ['pipe', 'pipe', 'pipe'] });
  const events = []; let stderr = '', buffer = '';
  child.stderr.on('data', data => stderr += data);
  child.stdout.on('data', data => {
    buffer += data;
    for (let end; (end = buffer.indexOf('\n')) >= 0;) {
      const line = buffer.slice(0, end); buffer = buffer.slice(end + 1);
      if (line) events.push(JSON.parse(line));
    }
  });
  try {
    child.stdin.write(JSON.stringify({ type: 'inspect', targetFile: manager.getSessionFile(), workspace, defaultCwd: workspace,
      agentDir, sessionDir, agent: {}, model: 'fixture', models: [{ id: 'fixture' }], gateway: 'http://127.0.0.1:9/', token: 'test-only' }) + '\n');
    // This is a data-integrity test, not a warm-start benchmark. On the Intel
    // macOS runner, a cold Pi import shares CPUs with other integration tests.
    const end = Date.now() + 45000;
    while (!events.some(event => event.type === 'done') && child.exitCode === null && Date.now() < end) await new Promise(done => setTimeout(done, 20));
    assert.ok(events.some(event => event.type === 'done'), stderr || JSON.stringify(events.slice(-3)));
    const state = events.filter(event => event.type === 'state').at(-1);
    assert.equal(state.messages.length, 600);
    assert.equal(state.messages[0].text, 'Question 0');
    assert.equal(state.messages.at(-1).text, 'Answer 299');
    assert.equal(state.tree.length, 600);
    assert.equal(state.indexedHistory, undefined);
  } finally {
    if (child.exitCode === null) {
      child.stdin.write('{"type":"shutdown"}\n');
      await Promise.race([new Promise(done => child.once('exit', done)), new Promise(done => setTimeout(done, 1500))]);
      if (child.exitCode === null) {
        child.kill();
        await new Promise(done => child.once('exit', done));
      }
    }
    await rm(temp, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  }
});
