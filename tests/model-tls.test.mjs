import test from 'node:test';
import assert from 'node:assert/strict';
import https from 'node:https';
import http from 'node:http';
import net from 'node:net';
import tls from 'node:tls';
import {readFileSync, mkdtempSync, readdirSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {Gateway} from '../runtime/gateway.mjs';
import {fetchProvider} from '../runtime/transport.mjs';
import {certificateFailure, transientFailure, upstreamFailure} from '../runtime/network-retry.mjs';
import {retryAssistantCall, completeSimple} from '../runtime/releases/coding-1.3.0/node_modules/@earendil-works/pi-ai/dist/compat.js';

const fixture = name => new URL(`fixtures/update-tls/${name}.pem`, import.meta.url);
const ca = readFileSync(fixture('ca'), 'utf8');
const body = {messages: [{role: 'user', content: 'synthetic fixture'}], tools: [], max_tokens: 10, parameters: {}};
const list = async stream => { const result = []; for await (const item of stream) result.push(item); return result; };

async function server(t, host = '127.0.0.1') {
  let requests = 0, failedHandshakes = 0;
  const instance = https.createServer({cert: readFileSync(fixture('server')), key: readFileSync(fixture('server-key'))}, (req, res) => {
    requests++;
    req.resume();
    res.setHeader('Content-Type', 'application/json');
    res.end(JSON.stringify({choices: [{message: {role: 'assistant', content: 'TLS fixture'}, finish_reason: 'stop'}]}));
  });
  instance.on('tlsClientError', () => { failedHandshakes++; });
  await new Promise(resolve => instance.listen(0, host, resolve));
  t.after(async () => { instance.closeAllConnections(); await new Promise(resolve => instance.close(resolve)); });
  return {url: `https://${host}:${instance.address().port}/v1`, port: instance.address().port,
    get requests() { return requests; }, get failedHandshakes() { return failedHandshakes; }};
}

function systemStore(t, certificates) {
  const original = tls.getCACertificates.bind(tls);
  // Only this process's reader is mocked. Never install a fixture CA on the OS.
  t.mock.method(tls, 'getCACertificates', type => type === 'system' ? certificates : original(type));
}

async function request(provider, record = {}) {
  const {response, agent} = await fetchProvider(provider, provider.baseUrl, {signal: AbortSignal.timeout(5000)}, record);
  try { return {result: await response.json(), record}; }
  finally { await agent.close(); }
}

test('trust failures expand to system CAs once, then remember successful recovery', async t => {
  const upstream = await server(t); systemStore(t, [ca]);
  const provider = {id: 'recover-system', baseUrl: upstream.url, proxy: {mode: 'direct'}};
  const first = await request(provider);
  assert.equal(first.result.choices[0].message.content, 'TLS fixture');
  assert.deepEqual(first.record.tls.attempts.map(a => a.trust), ['default', 'default+system+configured']);
  assert.ok(certificateFailure(first.record.tls.attempts[0].error));
  const second = await request(provider);
  assert.equal(second.record.tls.attempts.length, 1);
  assert.equal(second.record.tls.attempts[0].trust, 'default+system+configured');
  assert.equal(upstream.requests, 2);
});

test('custom and system proxies use the same verified trust recovery', async t => {
  const upstream = await server(t); systemStore(t, [ca]);
  let tunnels = 0;
  const sockets = new Set(), proxy = http.createServer();
  proxy.on('connect', (req, socket, head) => {
    tunnels++; sockets.add(socket);
    const target = net.connect(upstream.port, '127.0.0.1', () => {
      socket.write('HTTP/1.1 200 Connection Established\r\n\r\n');
      if (head.length) target.write(head);
      socket.pipe(target).pipe(socket);
    });
    socket.on('close', () => { sockets.delete(socket); target.destroy(); });
    socket.on('error', () => target.destroy()); target.on('error', () => socket.destroy());
  });
  await new Promise(resolve => proxy.listen(0, '127.0.0.1', resolve));
  const saved = {...process.env};
  for (const key of ['HTTP_PROXY','HTTPS_PROXY','NO_PROXY','http_proxy','https_proxy','no_proxy']) delete process.env[key];
  const address = `http://127.0.0.1:${proxy.address().port}`;
  process.env.HTTPS_PROXY = address;
  t.after(async () => {
    process.env = saved;
    for (const socket of sockets) socket.destroy();
    proxy.closeAllConnections(); await new Promise(resolve => proxy.close(resolve));
  });
  for (const mode of ['custom', 'system']) {
    const result = await request({id: 'proxy-' + mode, baseUrl: upstream.url, proxy: {mode, url: address}});
    assert.equal(result.record.tls.attempts.length, 2);
    assert.equal(result.result.choices[0].message.content, 'TLS fixture');
  }
  assert.equal(tunnels, 4);
  assert.equal(upstream.requests, 2);
});

test('provider CA files are additive, isolated, and never copied into diagnostics', async t => {
  const upstream = await server(t); systemStore(t, []);
  const directory = mkdtempSync(join(tmpdir(), 'umeko-model-ca-'));
  t.after(() => rmSync(directory, {recursive: true, force: true}));
  const file = join(directory, 'selected-ca.pem'); writeFileSync(file, ca);
  const provider = {id: 'selected-ca', name: 'Fixture', api: 'openai', baseUrl: upstream.url, key: 'synthetic-secret', proxy: {mode: 'direct', caFile: file}};
  const result = await request(provider);
  assert.equal(result.record.tls.attempts.length, 1);
  const gateway = new Gateway(); gateway.configure({diagnosticsDir: directory}); t.after(() => gateway.close());
  let calls = 0;
  const response = await retryAssistantCall(async () => {
    calls++;
    try { await list(gateway.events({...provider, id: 'without-ca', proxy: {mode: 'direct'}}, {upstream: 'fixture'}, body, new AbortController())); }
    catch (error) {
      assert.equal(error.retryable, false);
      assert.match(error.message, /TLS certificate validation failed/);
      assert.equal(upstreamFailure(error).code, 'umeko_tls_certificate');
      return {role: 'assistant', stopReason: 'error', errorMessage: error.message};
    }
    assert.fail('An untrusted certificate must fail');
  }, {enabled: true, maxRetries: 5, baseDelayMs: 1});
  assert.equal(response.stopReason, 'error'); assert.equal(calls, 1);
  const report = JSON.parse(readFileSync(join(directory, readdirSync(directory).find(name => name.endsWith('.json'))), 'utf8'));
  assert.equal(report.http[0].tls.attempts.length, 2);
  assert.ok(certificateFailure(report.error));
  assert.ok(!JSON.stringify(report).includes('synthetic-secret'));
  assert.ok(!JSON.stringify(report).includes('BEGIN CERTIFICATE'));
  assert.equal(upstream.requests, 1);
});

test('an HTTPS proxy and its target both retain certificate verification during recovery', async t => {
  const upstream = await server(t); systemStore(t, [ca]);
  const sockets = new Set();
  let tunnels = 0;
  const proxy = https.createServer({cert: readFileSync(fixture('server')), key: readFileSync(fixture('server-key'))});
  proxy.on('tlsClientError', () => {});
  proxy.on('connect', (req, socket, head) => {
    tunnels++; sockets.add(socket);
    const target = net.connect(upstream.port, '127.0.0.1', () => {
      socket.write('HTTP/1.1 200 Connection Established\r\n\r\n');
      if (head.length) target.write(head);
      socket.pipe(target).pipe(socket);
    });
    socket.on('close', () => { sockets.delete(socket); target.destroy(); });
    socket.on('error', () => target.destroy()); target.on('error', () => socket.destroy());
  });
  await new Promise(resolve => proxy.listen(0, '127.0.0.1', resolve));
  t.after(async () => {
    for (const socket of sockets) socket.destroy();
    proxy.closeAllConnections(); await new Promise(resolve => proxy.close(resolve));
  });
  const result = await request({id: 'https-proxy', baseUrl: upstream.url,
    proxy: {mode: 'custom', url: `https://127.0.0.1:${proxy.address().port}`}});
  assert.equal(result.record.tls.attempts.length, 2);
  assert.equal(result.result.choices[0].message.content, 'TLS fixture');
  assert.equal(tunnels, 1); assert.equal(upstream.requests, 1);
});

test('a stream interrupted after response headers cannot be replayed by certificate recovery', async t => {
  let calls = 0;
  const upstream = https.createServer({cert: readFileSync(fixture('server')), key: readFileSync(fixture('server-key'))}, (req, res) => {
    calls++; req.resume(); res.writeHead(200, {'Content-Type': 'text/event-stream'});
    res.write('data: {"choices":[{"delta":{"content":"partial fixture"}}]}\n\n');
    setTimeout(() => res.destroy(), 20);
  });
  await new Promise(resolve => upstream.listen(0, '127.0.0.1', resolve));
  const gateway = new Gateway();
  t.after(async () => { await gateway.close(); upstream.closeAllConnections(); await new Promise(resolve => upstream.close(resolve)); });
  const provider = {id: 'partial-stream', name: 'Fixture', api: 'openai', baseUrl: `https://127.0.0.1:${upstream.address().port}/v1`,
    proxy: {mode: 'direct', caFile: fileURLToPath(fixture('ca'))}};
  const emitted = [];
  await assert.rejects(async () => { for await (const event of gateway.events(provider, {upstream: 'fixture'}, body, new AbortController())) emitted.push(event); },
    error => error.retryable === true && !error.tlsCode);
  assert.equal(calls, 1);
  assert.equal(emitted[0].choices[0].delta.content, 'partial fixture');
});

test('hostname mismatches, invalid CA files, and cancellation do not expand trust', async t => {
  const upstream = await server(t); systemStore(t, [ca]);
  // The fixture cert covers the loopback IP, but not the localhost DNS name.
  // Avoid requiring an extra loopback address alias on macOS.
  const provider = {id: 'mismatch', baseUrl: upstream.url.replace('127.0.0.1', 'localhost'), proxy: {mode: 'direct', caFile: fileURLToPath(fixture('ca'))}};
  const record = {};
  await assert.rejects(request(provider, record), error => certificateFailure(error)?.code === 'ERR_TLS_CERT_ALTNAME_INVALID');
  assert.equal(record.tls.attempts.length, 1);
  const invalid = {};
  await assert.rejects(request({...provider, proxy: {mode: 'direct', caFile: join(tmpdir(), 'missing-umeko-ca.pem')}}, invalid),
    error => certificateFailure(error)?.code === 'UMEKO_CA_FILE_INVALID');
  assert.equal(invalid.tls.attempts.length, 1);
  const controller = new AbortController(); controller.abort();
  await assert.rejects(fetchProvider(provider, provider.baseUrl, {signal: controller.signal}, {}), {name: 'AbortError'});
  assert.equal(upstream.requests, 0);
});

test('nested and aggregate TLS errors take precedence over generic fetch/network errors', () => {
  const cause = Object.assign(new Error('issuer missing'), {code: 'SELF_SIGNED_CERT_IN_CHAIN'});
  const error = new TypeError('fetch failed', {cause: new AggregateError([cause, new Error('ECONNRESET')])});
  assert.equal(transientFailure(error), false);
  assert.equal(certificateFailure(error).recoverable, true);
  for (const code of ['CERT_HAS_EXPIRED', 'CERT_NOT_YET_VALID', 'ERR_TLS_CERT_ALTNAME_INVALID']) {
    const fatal = new Error('fetch failed', {cause: Object.assign(new Error('invalid peer'), {code})});
    assert.equal(certificateFailure(fatal).recoverable, false);
    assert.equal(transientFailure(fatal), false);
  }
  assert.equal(transientFailure(new TypeError('fetch failed', {cause: new Error('ECONNRESET')})), true);
});

test('a real Pi request through the private gateway stops on a TLS failure without scheduling retries', async t => {
  const upstream = await server(t); systemStore(t, []);
  const directory = mkdtempSync(join(tmpdir(), 'umeko-pi-tls-'));
  const gateway = new Gateway(), port = await gateway.start();
  gateway.configure({token: 'local-fixture', diagnosticsDir: directory,
    providers: [{id: 'pi-tls-fixture', name: 'Fixture', api: 'openai', baseUrl: upstream.url,
      key: 'synthetic-model-key', proxy: {mode: 'direct'}, models: [{upstream: 'fixture', alias: 'fixture'}]}]});
  t.after(async () => { await gateway.close(); rmSync(directory, {recursive: true, force: true}); });
  const model = {id: 'fixture', name: 'Fixture', api: 'openai-completions', provider: 'umeko',
    baseUrl: `http://127.0.0.1:${port}/v1`, reasoning: false, input: ['text'],
    cost: {input: 0, output: 0, cacheRead: 0, cacheWrite: 0}, contextWindow: 128000, maxTokens: 100};
  let calls = 0;
  const response = await retryAssistantCall(() => {
    calls++;
    return completeSimple(model, {messages: [{role: 'user', content: 'synthetic fixture', timestamp: Date.now()}]}, {apiKey: 'local-fixture'});
  }, {enabled: true, maxRetries: 5, baseDelayMs: 1});
  assert.equal(response.stopReason, 'error');
  assert.match(response.errorMessage, /TLS certificate validation failed/);
  assert.equal(calls, 1);
  assert.equal(readdirSync(directory).filter(name => name.endsWith('.json')).length, 1);
  assert.equal(upstream.requests, 0);
});
