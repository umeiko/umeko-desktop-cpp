import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import net from 'node:net';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { Gateway, parseSSE, collectOpenAI, anthropicEvents, openaiToAnthropic, anthropicToOpenAI, toAnthropicEvents, validateBase } from '../runtime/gateway.mjs';

const list = async stream => { const result = []; for await (const x of stream) result.push(x); return result; };

test('provider proxies are isolated and failed requests retain complete redacted diagnostics', async t => {
  const directory = mkdtempSync(join(tmpdir(), 'umeko-network-'));
  const upstream = http.createServer(async (req, res) => {
    let raw = ''; for await (const chunk of req) raw += chunk;
    const body = JSON.parse(raw), failed = body.messages[0].content === 'fail';
    const content = failed ? 'detail-'.repeat(12000) + 'model-secret' : JSON.stringify({choices:[{message:{role:'assistant',content:'ok'},finish_reason:'stop'}]});
    res.writeHead(failed ? 429 : 200, {'Content-Type':'application/json'});res.end(content);
  });
  await new Promise(resolve => upstream.listen(0,'127.0.0.1',resolve));
  const sockets = new Set(); let tunnels = 0;
  const proxy = http.createServer((req, res) => {
    tunnels++;
    const request = http.request({hostname:'127.0.0.1',port:upstream.address().port,
      method:req.method,path:new URL(req.url).pathname,headers:req.headers},reply=>{
      res.writeHead(reply.statusCode,reply.headers);reply.pipe(res);
    });
    request.on('error',()=>res.destroy());req.pipe(request);
  });
  proxy.on('connect', (req, socket, head) => {
    tunnels++; sockets.add(socket);
    const target = net.connect(upstream.address().port, '127.0.0.1', () => {
      socket.write('HTTP/1.1 200 Connection Established\r\n\r\n');
      if (head.length) target.write(head);
      socket.pipe(target).pipe(socket);
    });
    socket.on('close',()=>{sockets.delete(socket);target.destroy();});
    target.on('error',()=>socket.destroy());socket.on('error',()=>target.destroy());
  });
  await new Promise(resolve => proxy.listen(0,'127.0.0.1',resolve));
  const saved = Object.fromEntries(['HTTP_PROXY','HTTPS_PROXY','NO_PROXY','http_proxy','https_proxy','no_proxy'].map(k=>[k,process.env[k]]));
  const url = `http://127.0.0.1:${proxy.address().port}`;
  for (const key of Object.keys(saved)) delete process.env[key];
  Object.assign(process.env,{HTTP_PROXY:url,HTTPS_PROXY:url,NO_PROXY:''});
  const gateway = new Gateway(); gateway.configure({diagnosticsDir:directory});
  t.after(async () => {
    for (const [key,value] of Object.entries(saved)) if(value===undefined) delete process.env[key];else process.env[key]=value;
    await gateway.close();for(const socket of sockets) socket.destroy();upstream.closeAllConnections();proxy.closeAllConnections();
    await Promise.all([new Promise(resolve=>proxy.close(resolve)),new Promise(resolve=>upstream.close(resolve))]);
    rmSync(directory,{recursive:true,force:true});
  });
  const provider = mode=>({id:mode,name:mode,api:'openai',baseUrl:`http://127.0.0.1:${upstream.address().port}/v1`,key:'model-secret',proxy:{mode,url}});
  const body = text=>({messages:[{role:'user',content:text}],tools:[],max_tokens:10,parameters:{}});
  const call = mode=>list(gateway.events(provider(mode),{upstream:'fixture'},body('hello'),new AbortController()));
  await Promise.all([call('custom'),call('direct')]); assert.equal(tunnels,1);
  await call('system'); assert.equal(tunnels,2);
  process.env.NO_PROXY='*';await call('system');assert.equal(tunnels,2);
  await assert.rejects(async()=>list(gateway.events(provider('custom'),{upstream:'fixture'},body('fail'),new AbortController())),error=>{
    assert.ok(error.reportId);const raw=readFileSync(join(directory,error.reportId+'.json'),'utf8'),report=JSON.parse(raw);
    assert.ok(!raw.includes('model-secret'));
    assert.equal(report.response.status,429);
    assert.equal(report.response.body,'detail-'.repeat(12000)+'[REDACTED]');
    assert.equal(report.http[0].request.body.messages[0].content,'fail');
    assert.equal(report.http[0].request.headers.Authorization,'[REDACTED]');return true;
  });
});
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
