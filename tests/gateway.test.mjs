import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { createHash } from 'node:crypto';
import { Gateway, parseSSE, collectOpenAI, anthropicEvents, openaiToAnthropic, anthropicToOpenAI, toAnthropicEvents, validateBase } from '../runtime/gateway.mjs';

const list = async stream => { const result = []; for await (const x of stream) result.push(x); return result; };
test('SSE preserves fragmented Chinese, multiline data and final line', async () => {
  const raw = Buffer.from('data: {"text":"你好"}\r\n\r\ndata: {"done":true}\n\ndata: [DONE]\n\n');
  const chunks = [...raw].map(n => Buffer.from([n]));
  assert.deepEqual(await list(parseSSE(chunks)), [{ text: '你好' }, { done: true }]);
  assert.deepEqual(await list(parseSSE([Buffer.from('data: {"last":true}')])), [{ last: true }]);
});
test('OpenAI accumulation preserves tools, thinking, usage and finish', async () => {
  const completion = await collectOpenAI([
    { choices: [{ delta: { reasoning_content: 'Compare', tool_calls: [{ index: 0, id: 'call-1', function: { name: 'read', arguments: '{"path":' } }] } }] },
    { choices: [{ delta: { tool_calls: [{ index: 0, function: { arguments: '"test.py"}' } }] }, finish_reason: 'tool_calls' }], usage: { prompt_tokens: 12, completion_tokens: 3 } },
  ]);
  assert.equal(completion.choices[0].message.reasoning_content, 'Compare');
  assert.deepEqual(JSON.parse(completion.choices[0].message.tool_calls[0].function.arguments), { path: 'test.py' });
  assert.equal(completion.choices[0].finish_reason, 'tool_calls');
  const anthropic = await list(toAnthropicEvents([completion], 'fixture', 'id'));
  assert.equal(anthropic.at(-2).delta.stop_reason, 'tool_use');
  assert.equal(anthropic.at(-1).type, 'message_stop');
});
test('Anthropic conversion retains images and tool results', () => {
  const messages = [{ role: 'user', content: [{ type: 'text', text: 'Inspect' }, { type: 'image', source: { type: 'base64', media_type: 'image/png', data: 'YWJj' } }] },
    { role: 'assistant', content: [{ type: 'tool_use', id: 'one', name: 'read', input: { path: 'notes.md' } }] },
    { role: 'user', content: [{ type: 'tool_result', tool_use_id: 'one', content: 'Notes' }] }];
  const openai = anthropicToOpenAI(messages, 'You are a fixture');
  assert.equal(openai[1].content[1].image_url.url, 'data:image/png;base64,YWJj');
  const back = openaiToAnthropic(openai, [{ name: 'read', parameters: { type: 'object' } }], 100);
  assert.equal(back.system, 'You are a fixture');
  assert.equal(back.messages[2].content[0].tool_use_id, 'one');
});
test('Anthropic streams preserve thinking and tool argument fragments', async () => {
  const events = await list(anthropicEvents([
    { type: 'message_start', message: { usage: { input_tokens: 7 } } },
    { type: 'content_block_start', index: 0, content_block: { type: 'thinking', thinking: '' } },
    { type: 'content_block_delta', index: 0, delta: { type: 'thinking_delta', thinking: 'Think' } },
    { type: 'content_block_start', index: 1, content_block: { type: 'tool_use', id: 'tool', name: 'write' } },
    { type: 'content_block_delta', index: 1, delta: { type: 'input_json_delta', partial_json: '{"path":"a"}' } },
    { type: 'message_delta', delta: { stop_reason: 'tool_use' }, usage: { output_tokens: 3 } },
  ]));
  const result = await collectOpenAI(events);
  assert.equal(result.choices[0].message.reasoning_content, 'Think');
  assert.equal(result.choices[0].message.tool_calls[0].function.name, 'write');
  assert.equal(result.usage.prompt_tokens, 7);
});
test('reject credentials and query parameters in provider base URLs', () => {
  assert.throws(() => validateBase('https://user:secret@example.invalid/v1'));
  assert.throws(() => validateBase('https://example.invalid/v1?secret=abc'));
  assert.throws(() => validateBase('file:///tmp/model'));
  assert.equal(validateBase('http://127.0.0.1:1234/v1/'), 'http://127.0.0.1:1234/v1');
});
test('real HTTP proxy: streams, keys, revocation, private gateway and malformed input', async t => {
  const upstream = http.createServer(async (req, res) => {
    let raw = ''; for await (const part of req) raw += part;
    const body = JSON.parse(raw); assert.equal(body.model, 'native-model');
    res.writeHead(200, { 'Content-Type': 'text/event-stream' });
    res.write('data: '+JSON.stringify({ choices: [{ delta: { content: 'Hello 你好' }, finish_reason: null }] })+'\n\n');
    res.end('data: '+JSON.stringify({ choices: [{ delta: {}, finish_reason: 'stop' }], usage: { prompt_tokens: 3, completion_tokens: 2 } })+'\n\ndata: [DONE]\n\n');
  });
  await new Promise(resolve => upstream.listen(0, '127.0.0.1', resolve));
  const events = [], gateway = new Gateway(value => events.push(value));
  t.after(async () => { await gateway.close(); upstream.closeAllConnections(); await new Promise(resolve => upstream.close(resolve)); });
  const port = await gateway.start();
  const key = 'fixture-key-abcdefghijklmnop';
  const settings = { port: 0, lan: false, openai: true, anthropic: true, keys: [{ id: 'fixture', enabled: true, digest: createHash('sha256').update(key).digest('hex') }] };
  const config = { token: 'private-fixture-token', providers: [{ id: 'provider', api: 'openai', baseUrl: `http://127.0.0.1:${upstream.address().port}/v1`, models: [{ alias: 'alias', upstream: 'native-model', vision: false }] }], models: [{ id: 'alias' }], settings };
  gateway.configure(config); await gateway.setPublic(true, settings);
  const base = `http://127.0.0.1:${gateway.publicServer.address().port}`;
  assert.equal((await fetch(base+'/v1/models')).status, 401);
  const headers = { Authorization: 'Bearer '+key, 'Content-Type': 'application/json' };
  assert.equal((await fetch(base+'/v1/models', { headers })).status, 200);
  const response = await fetch(base+'/v1/chat/completions', { method: 'POST', headers, body: JSON.stringify({ model: 'alias', messages: [{ role: 'user', content: 'Fixture' }], stream: true }) });
  const collected = await collectOpenAI(parseSSE(response.body));
  assert.equal(collected.choices[0].message.content, 'Hello 你好');
  const anth = await fetch(base+'/v1/messages', { method: 'POST', headers, body: JSON.stringify({ model: 'alias', messages: [{ role: 'user', content: 'Fixture' }], max_tokens: 100 }) });
  assert.equal((await anth.json()).content[0].text, 'Hello 你好');
  assert.equal((await fetch(base+'/v1/chat/completions', { method: 'POST', headers, body: '{' })).status, 400);
  assert.equal((await fetch(base+'/v1/chat/completions', { method: 'POST', headers, body: JSON.stringify({ model: 'alias', messages: [{ role: 'user', content: [{ type: 'image_url', image_url: { url: 'data:image/png;base64,YWJj' } }] }] }) })).status, 400);
  assert.equal((await fetch(`http://127.0.0.1:${port}/v1/models`, { headers })).status, 401);
  assert.equal((await fetch(`http://127.0.0.1:${port}/v1/models`, { headers: { Authorization: 'Bearer private-fixture-token' } })).status, 200);
  assert.equal((await fetch(base+'/v1/models', { headers: { ...headers, Origin: 'https://untrusted.example' } })).status, 403);
  gateway.configure({ ...config, settings: { ...settings, keys: [] } });
  assert.equal((await fetch(base+'/v1/models', { headers })).status, 401);
  assert(events.some(e => e.type === 'key-used' && e.id === 'fixture'));
});
test('provider bridge forwards streamed events through a private queue', async t => {
  let gateway;
  gateway = new Gateway(event => {
    if (event.type !== 'provider') return;
    queueMicrotask(() => {
      gateway.accept({ type: 'provider-event', id: event.id, event: { choices: [{ delta: { content: 'Python fixture' }, finish_reason: 'stop' }] } });
      gateway.accept({ type: 'provider-result', id: event.id, error: '' });
    });
  });
  t.after(() => gateway.close()); const port = await gateway.start();
  gateway.configure({ token: 'private', providers: [{ id: 'python', api: 'plugin', models: [{ alias: 'echo', upstream: 'native-echo' }] }], models: [] });
  const result = await fetch(`http://127.0.0.1:${port}/v1/chat/completions`, { method: 'POST', headers: { Authorization: 'Bearer private' }, body: JSON.stringify({ model: 'echo', messages: [{ role: 'user', content: 'hello' }] }) });
  assert.equal((await result.json()).choices[0].message.content, 'Python fixture');
});
