import test from 'node:test';
import assert from 'node:assert/strict';
import { Gateway } from '../runtime/gateway.mjs';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { once } from 'node:events';
import { mkdirSync, mkdtempSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';

const id = '@umeko/model group';
const token = 'internal-test-token';
const event = (delta, finish_reason = null) =>
  'data: ' + JSON.stringify({choices: [{index: 0, delta, finish_reason}]}) + '\n\n';
const success = res => {
  res.writeHead(200, {'content-type': 'text/event-stream'});
  res.end(event({role: 'assistant', content: 'backup answer'}) + event({}, 'stop') + 'data: [DONE]\n\n');
};
const failure = (res, status, code) => {
  res.writeHead(status, {'content-type': 'application/json'});
  res.end(JSON.stringify({error: {code, message: code}}));
};

async function fixture(t, primary, backup = success) {
  const calls = [];
  const upstream = createServer(async (req, res) => {
    const parts = [];
    for await (const chunk of req) parts.push(chunk);
    const body = JSON.parse(Buffer.concat(parts));
    calls.push({body, authorization: req.headers.authorization});
    (body.model === 'first-upstream' ? primary : backup)(res);
  });
  upstream.listen(0, '127.0.0.1');
  await once(upstream, 'listening');
  const gateway = new Gateway();
  const port = await gateway.start();
  t.after(async () => {
    await gateway.close();
    upstream.closeAllConnections();
    await new Promise(resolve => upstream.close(resolve));
  });
  const baseUrl = 'http://127.0.0.1:' + upstream.address().port;
  const providers = ['first', 'second'].map((name, index) => ({
    id: name, name, baseUrl, key: name + '-secret', api: 'openai', proxy: {mode: 'none'},
    models: [{alias: name, upstream: name + '-upstream', vision: index > 0}],
  }));
  const members = [
    {provider: 'first', model: 'first', effort: 'high'},
    {provider: 'second', model: 'second', effort: 'vendor-level'},
  ];
  gateway.configure({token, providers, modelGroup: {id, members}});
  const base = 'http://127.0.0.1:' + port;
  function request(body = {}) {
    return fetch(base + '/v1/chat/completions', {
      method: 'POST', headers: {authorization: 'Bearer ' + token, 'content-type': 'application/json'},
      body: JSON.stringify({model: id, messages: [{role: 'user', content: 'hello'}], ...body}),
    });
  }
  return {gateway, base, calls, request, members};
}

test('quota failure tries the next model with its configured effort and credentials', async t => {
  const {base, calls, request, gateway} = await fixture(t, res => failure(res, 429, 'insufficient_quota'));
  const response = await request({_umeko_reasoning_effort: 'off'});
  assert.equal(response.status, 200);
  assert.match((await response.json()).choices[0].message.content, /backup answer/);
  assert.deepEqual(calls.map(({body, authorization}) => [body.model, body.reasoning_effort, authorization]), [
    ['first-upstream', 'high', 'Bearer first-secret'],
    ['second-upstream', 'vendor-level', 'Bearer second-secret'],
  ]);
  assert.deepEqual(calls[0].body.messages, calls[1].body.messages);

  gateway.setModelGroup(undefined);
  assert.equal((await request()).status, 404);
  const models = await fetch(base + '/v1/models', {headers: {authorization: 'Bearer ' + token}});
  assert.ok(!(await models.json()).data.some(row => row.id === id));
  assert.equal(calls.length, 2);
});

test('invalid payloads are not forwarded to the next model', async t => {
  const {calls, request} = await fixture(t, res => failure(res, 400, 'invalid_request'));
  assert.equal((await request()).status, 400);
  assert.equal(calls.length, 1);
});

test('images go only to members with vision enabled', async t => {
  const {calls, request} = await fixture(t, res => failure(res, 500, 'unavailable'));
  const response = await request({messages: [{role: 'user', content: [{type: 'image_url', image_url: {url: 'data:image/png;base64,AA=='}}]}]});
  assert.equal(response.status, 200);
  assert.deepEqual(calls.map(({body}) => body.model), ['second-upstream']);
});

test('exhausting the group returns one terminal failure without another retry cycle', async t => {
  const {calls, request} = await fixture(t, res => failure(res, 429, 'insufficient_quota'),
    res => failure(res, 503, 'unavailable'));
  const response = await request();
  assert.equal(response.status, 400);
  const {error} = await response.json();
  assert.equal(error.code, 'umeko_model_group_failed');
  assert.equal(error.retryable, false);
  assert.equal(calls.length, 2);
});

test('Pi retries an interrupted stream through the backup without keeping partial output', {timeout: 30000}, async t => {
  const {base, calls} = await fixture(t, res => {
    res.writeHead(200, {'content-type': 'text/event-stream'});
    res.write(event({role: 'assistant', content: 'discard this partial answer'}));
    // Finish the transport cleanly without the required model completion event.
    res.end();
  });
  const work = mkdtempSync(join(tmpdir(), 'umeko-model-group-'));
  const agentDir = join(work, 'agent');
  mkdirSync(agentDir);
  const bridge = fileURLToPath(new URL('../runtime/releases/coding-1.5.2/bridge.mjs', import.meta.url));
  const child = spawn(process.execPath, [bridge], {cwd: work, stdio: ['pipe', 'pipe', 'pipe']});
  let stderr = '';
  child.stderr.on('data', chunk => { stderr = (stderr + chunk).slice(-3000); });
  const lines = createInterface({input: child.stdout})[Symbol.asyncIterator]();
  const exited = once(child, 'exit');
  t.after(async () => {
    child.kill();
    await exited;
    rmSync(work, {recursive: true, force: true});
  });
  child.stdin.write(JSON.stringify({type: 'prompt', text: 'model group test', model: id,
    gateway: base + '/', token, workspace: work, defaultCwd: work, agentDir, sessionDir: join(work, 'sessions'),
    models: [{id, input: ['text'], contextWindow: 128000}], agent: {autoCompaction: false},
  }) + '\n');
  const events = [];
  for (;;) {
    const line = await lines.next();
    assert.ok(!line.done, stderr);
    const row = JSON.parse(line.value);
    events.push(row);
    if (row.type === 'done') break;
  }
  assert.ok(!events.some(row => row.type === 'error'), JSON.stringify(events.filter(row => row.type === 'error')));
  assert.deepEqual(calls.map(({body}) => body.model), ['first-upstream', 'second-upstream']);
  const state = events.findLast(row => row.type === 'state');
  const messages = state.messages || state.state?.messages;
  assert.deepEqual(messages.filter(row => row.role === 'assistant').map(row => row.text), ['backup answer']);
  assert.equal(messages.filter(row => row.role === 'user').length, 1);
});
