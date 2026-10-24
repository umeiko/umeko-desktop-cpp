// HTTP/SSE transport for the native desktop. Pi owns tools, agent loops and sessions.
import http from 'node:http';
import { createHash, timingSafeEqual, randomUUID } from 'node:crypto';
import { createInterface } from 'node:readline';
import { pathToFileURL } from 'node:url';
import { once } from 'node:events';
import { upstreamFailure } from './network-retry.mjs';
import { dispatcher, failureReport, providerFetch } from './transport.mjs';
import { responsesRequest, responsesEvents } from './openai-responses.mjs';

const MAX_BODY = 32 * 1024 * 1024;
const textOf = content => typeof content === 'string' ? content : (content || []).filter(c => c.type === 'text').map(c => c.text || '').join('\n');
const equal = (a, b) => { const x = Buffer.from(a || ''), y = Buffer.from(b || ''); return x.length === y.length && x.length > 0 && timingSafeEqual(x, y); };
export function validateBase(value) {
  const url = new URL(value);
  if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password || url.search || url.hash) throw new Error('Invalid provider base URL');
  return url.toString().replace(/\/+$/, '');
}
export async function* parseSSE(chunks) {
  const decoder = new TextDecoder(); let buffer = '', lines = [];
  const flush = function* () { if (lines.length) { const value = lines.join('\n'); lines = []; if (value !== '[DONE]') yield JSON.parse(value); } };
  for await (const chunk of chunks) {
    buffer += typeof chunk === 'string' ? chunk : decoder.decode(chunk, { stream: true });
    if (buffer.length > MAX_BODY) throw new Error('Oversized upstream event');
    let index;
    while ((index = buffer.indexOf('\n')) >= 0) {
      const line = buffer.slice(0, index).replace(/\r$/, ''); buffer = buffer.slice(index + 1);
      if (!line) yield* flush();
      else if (line.startsWith('data:')) lines.push(line.slice(5).replace(/^ /, ''));
    }
  }
  buffer += decoder.decode();
  if (buffer.startsWith('data:')) lines.push(buffer.slice(5).trim());
  yield* flush();
}
export function anthropicToOpenAI(messages, system) {
  const out = [];
  if (system) out.push({ role: 'system', content: textOf(system) });
  for (const message of messages) {
    const content = typeof message.content === 'string' ? [{ type: 'text', text: message.content }] : message.content || [];
    const parts = [], calls = [], results = [];
    for (const block of content) {
      if (block.type === 'text') parts.push({ type: 'text', text: block.text });
      else if (block.type === 'image') {
        const s = block.source;
        parts.push({ type: 'image_url', image_url: { url: s.type === 'base64' ? `data:${s.media_type};base64,${s.data}` : s.url } });
      } else if (block.type === 'tool_use') calls.push({ id: block.id, type: 'function', function: { name: block.name, arguments: JSON.stringify(block.input || {}) } });
      else if (block.type === 'tool_result') results.push({ role: 'tool', tool_call_id: block.tool_use_id, content: textOf(block.content) });
    }
    if (parts.length || calls.length) out.push({ role: message.role, content: parts.some(c => c.type === 'image_url') ? parts : parts.map(c => c.text).join('\n'), ...(calls.length ? { tool_calls: calls } : {}) });
    out.push(...results);
  }
  return out;
}
export function openaiToAnthropic(messages, tools, maxTokens, parameters = {}) {
  const system = [], out = [];
  for (const message of messages) {
    if (['system', 'developer'].includes(message.role)) { system.push(textOf(message.content)); continue; }
    let role = message.role === 'tool' ? 'user' : message.role;
    const content = [];
    if (message.role === 'tool') content.push({ type: 'tool_result', tool_use_id: message.tool_call_id, content: textOf(message.content) });
    else {
      for (const part of typeof message.content === 'string' ? [{ type: 'text', text: message.content }] : message.content || []) {
        if (part.type === 'text' && part.text) content.push({ type: 'text', text: part.text });
        else if (part.type === 'image_url') {
          const url = part.image_url?.url || ''; const match = /^data:([^;]+);base64,(.+)$/s.exec(url);
          content.push({ type: 'image', source: match ? { type: 'base64', media_type: match[1], data: match[2] } : { type: 'url', url } });
        }
      }
      for (const call of message.tool_calls || []) content.push({ type: 'tool_use', id: call.id, name: call.function.name, input: JSON.parse(call.function.arguments || '{}') });
    }
    if (!content.length) continue;
    if (out.at(-1)?.role === role) out.at(-1).content.push(...content); else out.push({ role, content });
  }
  const result = { messages: out, max_tokens: maxTokens, stream: true };
  if (system.length) result.system = system.join('\n\n');
  if (tools.length) result.tools = tools.map(t => ({ name: t.name, description: t.description || '', input_schema: t.parameters || { type: 'object', properties: {} } }));
  for (const key of ['temperature', 'top_p', 'top_k']) if (parameters[key] !== undefined) result[key] = parameters[key];
  if (parameters.reasoning_effort !== undefined) result.output_config = { effort: parameters.reasoning_effort };
  if (parameters.stop !== undefined) result.stop_sequences = Array.isArray(parameters.stop) ? parameters.stop : [parameters.stop];
  const choice = parameters.tool_choice;
  if (choice) result.tool_choice = typeof choice === 'object' ? { type: 'tool', name: choice.function.name } : { type: choice === 'required' ? 'any' : choice };
  if (parameters.parallel_tool_calls === false && result.tool_choice) result.tool_choice.disable_parallel_tool_use = true;
  return result;
}
export async function* anthropicEvents(events) {
  const indices = new Map(); let next = 0;
  for await (const e of events) {
    if (e.type === 'error') throw new Error(JSON.stringify(e.error || e));
    let delta = {}, usage, finish = null;
    if (e.type === 'message_start') { delta.role = 'assistant'; usage = { prompt_tokens: e.message?.usage?.input_tokens || 0 }; }
    else if (e.type === 'content_block_start' && e.content_block.type === 'tool_use') {
      indices.set(e.index, next); delta.tool_calls = [{ index: next++, id: e.content_block.id, type: 'function', function: { name: e.content_block.name, arguments: '' } }];
    } else if (e.type === 'content_block_start' && e.content_block.type === 'thinking') delta.reasoning_content = e.content_block.thinking || '';
    else if (e.type === 'content_block_delta') {
      if (e.delta.type === 'text_delta') delta.content = e.delta.text;
      else if (e.delta.type === 'thinking_delta') delta.reasoning_content = e.delta.thinking;
      else if (e.delta.type === 'input_json_delta') delta.tool_calls = [{ index: indices.get(e.index), function: { arguments: e.delta.partial_json } }];
    } else if (e.type === 'message_delta') {
      finish = ({ tool_use: 'tool_calls', max_tokens: 'length' })[e.delta.stop_reason] || 'stop'; usage = { completion_tokens: e.usage?.output_tokens || 0 };
      if (e.usage?.input_tokens !== undefined) usage.prompt_tokens = e.usage.input_tokens;
    } else if (e.content) {
      const calls = e.content.filter(c => c.type === 'tool_use').map(c => ({ id: c.id, type: 'function', function: { name: c.name, arguments: JSON.stringify(c.input) } }));
      yield { choices: [{ message: { role: 'assistant', content: textOf(e.content), reasoning_content: e.content.filter(c => c.type === 'thinking').map(c => c.thinking || '').join('\n'), ...(calls.length ? { tool_calls: calls } : {}) }, finish_reason: ({ tool_use: 'tool_calls', max_tokens: 'length' })[e.stop_reason] || 'stop' }], usage: { prompt_tokens: e.usage?.input_tokens || 0, completion_tokens: e.usage?.output_tokens || 0 } };
      continue;
    }
    if (Object.keys(delta).length || usage || finish) yield { choices: [{ index: 0, delta, finish_reason: finish }], ...(usage ? { usage } : {}) };
  }
}
export async function collectOpenAI(events) {
  let content = '', thinking = '', usage = {}, finish = 'stop'; const calls = new Map();
  for await (const event of events) {
    if (event.error) throw new Error('Upstream returned an error');
    Object.assign(usage, event.usage || {});
    for (const choice of event.choices || []) {
      const delta = choice.delta || choice.message || {};
      content += delta.content || ''; thinking += delta.reasoning_content || delta.reasoning || delta.reasoning_text || '';
      for (const [fallback, call] of (delta.tool_calls || []).entries()) {
        const index = call.index ?? fallback; let c = calls.get(index);
        if (!c) { c = { id: '', type: 'function', function: { name: '', arguments: '' } }; calls.set(index, c); }
        c.id += call.id || ''; c.function.name += call.function?.name || ''; c.function.arguments += call.function?.arguments || '';
      }
      finish = choice.finish_reason || finish;
    }
  }
  return { choices: [{ index: 0, message: { role: 'assistant', content, ...(thinking ? { reasoning_content: thinking } : {}), ...(calls.size ? { tool_calls: [...calls.values()] } : {}) }, finish_reason: finish }], usage };
}
export async function* toAnthropicEvents(events, model, id) {
  yield { type: 'message_start', message: { id, type: 'message', role: 'assistant', model, content: [], stop_reason: null, stop_sequence: null, usage: { input_tokens: 0, output_tokens: 0 } } };
  let next = 0, text, thinking, finish = 'end_turn', input = 0, output = 0; const blocks = [], tools = new Map();
  for await (const e of events) {
    if (e.error) throw new Error(JSON.stringify(e.error || e));
    input = e.usage?.prompt_tokens ?? input; output = e.usage?.completion_tokens ?? output;
    for (const choice of e.choices || []) {
      const d = choice.delta || choice.message || {}; const reasoning = d.reasoning_content || d.reasoning || d.reasoning_text;
      if (reasoning) {
        if (thinking === undefined) { thinking = next++; blocks.push(thinking); yield { type: 'content_block_start', index: thinking, content_block: { type: 'thinking', thinking: '' } }; }
        yield { type: 'content_block_delta', index: thinking, delta: { type: 'thinking_delta', thinking: reasoning } };
      }
      if (d.content) {
        if (text === undefined) { text = next++; blocks.push(text); yield { type: 'content_block_start', index: text, content_block: { type: 'text', text: '' } }; }
        yield { type: 'content_block_delta', index: text, delta: { type: 'text_delta', text: d.content } };
      }
      for (const [fallback, call] of (d.tool_calls || []).entries()) {
        const key = call.index ?? fallback;
        if (!tools.has(key)) { const index = next++; tools.set(key, index); blocks.push(index); yield { type: 'content_block_start', index, content_block: { type: 'tool_use', id: call.id, name: call.function.name, input: {} } }; }
        if (call.function?.arguments) yield { type: 'content_block_delta', index: tools.get(key), delta: { type: 'input_json_delta', partial_json: call.function.arguments } };
      }
      if (choice.finish_reason) finish = ({ length: 'max_tokens', tool_calls: 'tool_use' })[choice.finish_reason] || 'end_turn';
    }
  }
  for (const index of blocks) yield { type: 'content_block_stop', index };
  yield { type: 'message_delta', delta: { stop_reason: finish, stop_sequence: null }, usage: { input_tokens: input, output_tokens: output } };
  yield { type: 'message_stop' };
}
function anthropicResponse(completion, model, id) {
  const { message, finish_reason: finish } = completion.choices[0], content = [];
  if (message.reasoning_content) content.push({ type: 'thinking', thinking: message.reasoning_content });
  if (message.content) content.push({ type: 'text', text: message.content });
  for (const call of message.tool_calls || []) content.push({ type: 'tool_use', id: call.id, name: call.function.name, input: JSON.parse(call.function.arguments || '{}') });
  return { id, type: 'message', role: 'assistant', model, content, stop_reason: ({ length: 'max_tokens', tool_calls: 'tool_use' })[finish] || 'end_turn', stop_sequence: null, usage: { input_tokens: completion.usage.prompt_tokens || 0, output_tokens: completion.usage.completion_tokens || 0 } };
}
class EventQueue {
  items = []; waiter; ended = false; error; bytes = 0;
  push(value) { this.bytes += JSON.stringify(value).length; if (this.bytes > 8 * 1024 * 1024) return this.end(new Error('Provider output exceeded the buffer limit')); this.items.push(value); this.wake(); }
  wake() { this.waiter?.(); this.waiter = undefined; }
  end(error) { this.ended = true; this.error = error; this.wake(); }
  async *[Symbol.asyncIterator]() { while (true) { if (this.items.length) { const item = this.items.shift(); this.bytes -= JSON.stringify(item).length; yield item; } else if (this.ended) { if (this.error) throw this.error; return; } else await new Promise(resolve => { this.waiter = resolve; }); } }
}
export class Gateway {
  constructor(emit = () => {}) { this.emit = emit; this.providers = []; this.models = []; this.token = ''; this.settings = {}; this.pending = new Map(); this.controllers = new Set(); this.privateServer = this.server(true); }
  configure(input) { this.diagnosticsDir = input.diagnosticsDir; this.providers = input.providers || []; this.models = input.models || []; this.token = input.token || ''; this.settings = input.settings || this.settings; }
  server(isPrivate) {
    const server = http.createServer((req, res) => { void this.handle(req, res, isPrivate).catch(() => {
      if (!res.headersSent) { res.writeHead(400, { 'Content-Type': 'application/json' }); res.end(JSON.stringify({ error: { type: 'invalid_request_error', message: 'Invalid request structure' } })); }
      else if (!res.destroyed) res.end();
    }); });
    server.headersTimeout = 15000; server.requestTimeout = 100000; server.maxHeadersCount = 64;
    return server;
  }
  async start() { await new Promise((resolve, reject) => this.privateServer.once('error', reject).listen(0, '127.0.0.1', resolve)); return this.privateServer.address().port; }
  authorized(req, isPrivate) {
    const key = req.headers.authorization?.replace(/^Bearer\s+/i, '') || req.headers['x-api-key'] || '';
    if (isPrivate) return equal(key, this.token);
    if (typeof key !== 'string' || !/^[\x21-\x7e]{1,256}$/.test(key)) return false;
    const digest = createHash('sha256').update(key).digest('hex');
    for (const row of this.settings.keys || []) if (row.enabled && equal(digest, row.digest)) { this.emit({ type: 'key-used', id: row.id, at: new Date().toISOString() }); return true; }
    return false;
  }
  cors(req, res, isPrivate) {
    const origin = req.headers.origin;
    if (!origin) return true;
    let allowed = false;
    try { const url = new URL(origin), host = url.hostname; allowed = ['http:', 'https:'].includes(url.protocol) && (['localhost', '127.0.0.1', '[::1]'].includes(host) || (!isPrivate && this.settings.lan && /^(10\.|192\.168\.|172\.(?:1[6-9]|2\d|3[01])\.)/.test(host))); } catch { /* Invalid origins are denied. */ }
    if (allowed) { res.setHeader('Access-Control-Allow-Origin', origin); res.setHeader('Vary', 'Origin'); res.setHeader('Access-Control-Allow-Headers', 'authorization,content-type,x-api-key,anthropic-version'); res.setHeader('Access-Control-Allow-Methods', 'GET,POST,OPTIONS'); }
    return allowed;
  }
  async *events(provider, model, body, controller) {
    const http = [];
    try {
      for await (const event of this.transportEvents(provider, model, body, controller, http)) {
        if (event.error) throw new Error(JSON.stringify(event));
        yield event;
      }
    } catch (error) {
      if (controller.signal.aborted) throw error;
      const records = error.diagnostics || http;
      throw failureReport(this.diagnosticsDir, provider, { transport: provider.api, ...body, model: model.upstream },
        records.findLast(item => item.response)?.response || {}, error, records);
    }
  }
  async *transportEvents(provider, model, body, controller, http) {
    if (provider.api === 'plugin') {
      const id = randomUUID(), queue = new EventQueue(); this.pending.set(id, queue);
      const abort = () => { queue.end(new Error('Cancelled')); this.emit({ type: 'provider-cancel', id }); };
      controller.signal.addEventListener('abort', abort, { once: true });
      this.emit({ type: 'provider', id, provider: provider.id, body: { ...body, model: model.upstream } });
      try { yield* queue; } finally { this.pending.delete(id); controller.signal.removeEventListener('abort', abort); this.emit({ type: 'provider-cancel', id }); }
      return;
    }
    const anthropic = provider.api === 'anthropic', responses = provider.wireApi === 'responses', base = validateBase(provider.baseUrl);
    const headers = { 'Content-Type': 'application/json', 'User-Agent':'Umeko-Desktop', ...(anthropic ? { 'x-api-key': provider.key || '', 'anthropic-version': '2023-06-01' } : { Authorization: `Bearer ${provider.key || ''}` }) };
    const payload = anthropic ? openaiToAnthropic(body.messages, body.tools, body.max_tokens, body.parameters) : responses ? responsesRequest(body.messages, body.tools, body.max_tokens, body.parameters) : { messages: body.messages, max_tokens: body.max_tokens, stream: true, stream_options: { include_usage: true }, ...(body.tools.length ? { tools: body.tools.map(t => ({ type: 'function', function: t })) } : {}), ...body.parameters };
    payload.model = model.upstream;
    const suffix = anthropic ? '/messages' : responses ? '/responses' : '/chat/completions';
    const agent = dispatcher(provider);
    const record = { request: { url: base + suffix, method: 'POST', headers, body: payload },
      response: { status: null, headers: {}, body: '' } };
    http.push(record);
    try {
      const response = await providerFetch(base + suffix, { method: 'POST', headers, body: JSON.stringify(payload), redirect: 'error', dispatcher: agent,
        signal: AbortSignal.any([controller.signal, AbortSignal.timeout(300000)]) });
      record.response.status = response.status;
      record.response.headers = Object.fromEntries(response.headers);
      const tracked = async function* () {
        const decoder = new TextDecoder();
        for await (const chunk of response.body) { record.response.body += decoder.decode(chunk, {stream:true}); yield chunk; }
        record.response.body += decoder.decode();
      };
      if (!response.ok) { record.response.body = await response.text(); throw new Error(`Provider returned HTTP ${response.status}`); }
      let events;
      if (response.headers.get('content-type')?.includes('event-stream')) events = parseSSE(tracked());
      else { const raw = await response.text(); record.response.body = raw; if (raw.length > MAX_BODY) throw new Error('Upstream response too large'); events = [JSON.parse(raw)]; }
      if (anthropic) yield* anthropicEvents(events); else if (responses) yield* responsesEvents(events); else for await (const event of events) yield event;
    } finally { await agent.close(); }
  }
  async handle(req, res, isPrivate) {
    const json = (status, value) => { if (res.headersSent) return res.end(); res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' }); res.end(JSON.stringify(value)); };
    const error = (status, message, type = 'invalid_request_error') => json(status, { error: { message, type } });
    if (!this.cors(req, res, isPrivate)) return error(403, 'Origin is not allowed');
    if (req.method === 'OPTIONS') { res.writeHead(204); return res.end(); }
    if (!this.authorized(req, isPrivate)) return error(401, 'Invalid API key', 'authentication_error');
    const path = req.url.split('?')[0];
    if (req.method === 'GET' && ['/v1/models', '/models'].includes(path)) return json(200, { object: 'list', data: this.models });
    if (req.method === 'GET' && path === '/') return json(200, { status: 'running', name: 'Umeko C++' });
    const anthropic = ['/v1/messages', '/messages'].includes(path);
    if (req.method !== 'POST' || !['/v1/messages', '/messages', '/v1/chat/completions', '/chat/completions'].includes(path)) return error(404, 'Not found');
    if (!isPrivate && this.settings[anthropic ? 'anthropic' : 'openai'] === false) return error(404, 'Protocol disabled');
    let input;
    try {
      let bytes = 0; const parts = [];
      for await (const chunk of req) { bytes += chunk.length; if (bytes > MAX_BODY) return error(413, 'Request is too large'); parts.push(chunk); }
      input = JSON.parse(Buffer.concat(parts).toString('utf8'));
      if (!Array.isArray(input.messages) || !input.messages.length || typeof input.model !== 'string') throw new Error();
    } catch { return error(400, 'Invalid JSON request or messages'); }
    const p = this.providers.find(provider => provider.models?.some(model => model.alias === input.model));
    if (!p) return error(404, 'Model is not configured');
    const model = p.models.find(m => m.alias === input.model), messages = anthropic ? anthropicToOpenAI(input.messages, input.system) : input.messages;
    const hasImages = messages.some(m => Array.isArray(m.content) && m.content.some(c => ['image_url', 'image'].includes(c.type)));
    if (hasImages && !model.vision) return error(400, 'Vision is not enabled for this model');
    const tools = (input.tools || []).map(t => anthropic ? { name: t.name, description: t.description || '', parameters: t.input_schema } : t.function);
    if (tools.some(t => !t || typeof t.name !== 'string')) return error(400, 'Invalid tool declaration');
    const parameters = {}; for (const key of ['temperature', 'top_p', 'top_k', 'stop', 'tool_choice', 'parallel_tool_calls', 'reasoning_effort']) if (input[key] !== undefined) parameters[key] = input[key];
    if (anthropic && input.output_config?.effort !== undefined) parameters.reasoning_effort = input.output_config.effort;
    if (anthropic) { if (input.stop_sequences) parameters.stop = input.stop_sequences; const choice = input.tool_choice; if (choice) { parameters.tool_choice = choice.type === 'tool' ? { type: 'function', function: { name: choice.name } } : choice.type === 'any' ? 'required' : choice.type; if (choice.disable_parallel_tool_use !== undefined) parameters.parallel_tool_calls = !choice.disable_parallel_tool_use; } }
    const body = { messages, tools, max_tokens: Math.max(1, Math.min(32768, Number(input.max_tokens || input.max_completion_tokens || 4096))), parameters };
    if (!Number.isFinite(body.max_tokens)) return error(400, 'Invalid max_tokens');
    const controller = new AbortController(); this.controllers.add(controller); res.on('close', () => { if (!res.writableEnded) controller.abort(); });
    const id = (anthropic ? 'msg_' : 'chatcmpl-') + randomUUID(), common = { id, object: 'chat.completion.chunk', created: Math.floor(Date.now() / 1000), model: input.model };
    try {
      const events = this.events(p, model, body, controller);
      if (!input.stream) { const completion = await collectOpenAI(events); return json(200, anthropic ? anthropicResponse(completion, input.model, id) : { id, object: 'chat.completion', created: common.created, model: input.model, ...completion }); }
      res.writeHead(200, { 'Content-Type': 'text/event-stream; charset=utf-8', 'Cache-Control': 'no-cache', Connection: 'keep-alive' });
      const stream = anthropic ? toAnthropicEvents(events, input.model, id) : events;
      for await (let event of stream) {
        if (res.destroyed) break;
        if (!anthropic) event = { ...common, ...event, model: input.model, choices: (event.choices || []).map(c => c.message ? { index: c.index || 0, delta: { ...c.message, ...(c.message.tool_calls ? { tool_calls: c.message.tool_calls.map((t, index) => ({ ...t, index })) } : {}) }, finish_reason: c.finish_reason } : c) };
        const frame = (anthropic ? `event: ${event.type}\n` : '') + `data: ${JSON.stringify(event)}\n\n`;
        if (!res.write(frame)) await once(res, 'drain', { signal: controller.signal });
      }
      if (!anthropic && !res.destroyed) res.write('data: [DONE]\n\n'); res.end();
    } catch (failure) {
      if (!res.headersSent) json(failure.retryable ? 503 : 400, {error: upstreamFailure(failure)});
      else if (!res.destroyed) { const value = { error: upstreamFailure(failure) }; res.end(`data: ${JSON.stringify(value)}\n\n`); }
    } finally { this.controllers.delete(controller); }
  }
  accept(input) { const queue = this.pending.get(input.id); if (!queue) return; if (input.type === 'provider-event') queue.push(input.event); else { const error = input.error ? new Error(input.error) : undefined; if (error) error.diagnostics = input.diagnostics; queue.end(error); } }
  async setPublic(enabled, settings) {
    const oldSettings = this.settings, old = this.publicServer; this.settings = settings;
    const host = settings.lan ? '0.0.0.0' : '127.0.0.1';
    if (old && enabled && old.address()?.port === settings.port && this.publicHost === host) return { type: 'server', ok: true, running: true };
    if (old) { old.closeAllConnections(); await new Promise(resolve => old.close(resolve)); this.publicServer = undefined; }
    if (!enabled) return { type: 'server', ok: true, running: false };
    const listen = async value => { const server = this.server(false); const h = value.lan ? '0.0.0.0' : '127.0.0.1'; await new Promise((resolve, reject) => { server.once('error', reject); server.listen(value.port, h, resolve); }); this.publicServer = server; this.publicHost = h; };
    try { await listen(settings); return { type: 'server', ok: true, running: true }; }
    catch {
      this.settings = oldSettings; if (old) { try { await listen(oldSettings); } catch { /* The UI reports the stopped listener. */ } }
      return { type: 'server', ok: false, settings: this.settings, running: !!this.publicServer, error: this.publicServer ? '监听失败，原代理已恢复' : '无法启动代理，请检查端口占用或网络权限' };
    }
  }
  async close() { for (const controller of this.controllers) controller.abort(); for (const queue of this.pending.values()) queue.end(new Error('Application is closing')); for (const server of [this.publicServer, this.privateServer]) if (server?.listening) { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); } }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const emit = value => process.stdout.write(JSON.stringify(value) + '\n'); const gateway = new Gateway(emit); const rl = createInterface({ input: process.stdin }); let controls = Promise.resolve(), closing = false;
  const close = async () => { if (closing) return; closing = true; await gateway.close(); rl.close(); process.stdin.pause(); process.stdin.unref?.(); };
  rl.on('line', line => { try { const input = JSON.parse(line); if (input.type === 'configure') { gateway.configure(input); const port=gateway.privateServer.address()?.port; if(port)emit({type:'ready',port}); } else if (input.type.startsWith('provider-')) gateway.accept(input); else if (input.type === 'server') controls = controls.then(() => gateway.setPublic(input.enabled, input.settings)).then(emit); else if (input.type === 'shutdown') void close(); } catch { /* Never expose malformed requests or credentials in logs. */ } });
  rl.on('close', () => { void close(); });
  const port = await gateway.start(); emit({ type: gateway.token ? 'ready' : 'transport-ready', port });
}
