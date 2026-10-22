import { createRequire } from 'node:module';
import { mkdirSync, writeFileSync, readdirSync, statSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { transientFailure } from './network-retry.mjs';
import { randomUUID } from 'node:crypto';

const require = createRequire(new URL('./releases/coding-1.3.0/package.json', import.meta.url));
const { Agent, ProxyAgent, EnvHttpProxyAgent, fetch: providerFetch } = require('undici');
export { providerFetch };
export function dispatcher(provider) {
  const config = provider.proxy || {};
  if (config.mode === 'direct') return new Agent();
  if (config.mode === 'custom') return new ProxyAgent(config.url);
  return new EnvHttpProxyAgent({
    httpProxy: process.env.HTTP_PROXY || process.env.http_proxy || provider.systemProxy || '',
    httpsProxy: process.env.HTTPS_PROXY || process.env.https_proxy || provider.systemProxy || '',
    noProxy: process.env.NO_PROXY || process.env.no_proxy || ''
  });
}
export function redact(value, secrets = []) {
  if (Array.isArray(value)) return value.map(item => redact(item, secrets));
  if (value && typeof value === 'object') return Object.fromEntries(Object.entries(value).map(([key, item]) =>
    [key, /authorization|api.?key|cookie|password|token$|client.?secret/i.test(key) ? '[REDACTED]' : redact(item, secrets)]));
  if (typeof value === 'string') {
    for (const secret of secrets) if (typeof secret === 'string' && secret.length >= 3) value = value.split(secret).join('[REDACTED]');
    value = value.replace(/(https?:\/\/)[^\s/@]+:[^\s/@]+@/g, '$1[REDACTED]@');
  }
  return value;
}
export function failureReport(directory, provider, request, response, failure, http = []) {
  const id = randomUUID();
  const seen = new Set();
  const describe = error => {
    if (!error || typeof error !== 'object' || seen.has(error)) return String(error);
    seen.add(error);
    return {name:error.name, message:error.message, code:error.code, stack:error.stack,
      ...(error.cause ? {cause:describe(error.cause)} : {}),
      ...(error.errors ? {errors:error.errors.map(describe)} : {})};
  };
  const record = redact({ id, at: new Date().toISOString(), provider: provider.name, request, response,
    error: describe(failure), http }, [provider.key]);
  if (directory) {
    mkdirSync(directory, { recursive: true, mode: 0o700 });
    writeFileSync(join(directory, id + '.json'), JSON.stringify(record, null, 2), { mode: 0o600 });
    for (const file of readdirSync(directory).filter(name => /^[a-f0-9-]{36}\.json$/.test(name)).map(name => join(directory, name))
      .sort((a, b) => statSync(b).mtimeMs - statSync(a).mtimeMs).slice(30)) unlinkSync(file);
  }
  const error = new Error(`Provider request failed; open request details [Umeko:${id}]`);
  error.reportId = id; error.cause = failure; error.retryable = transientFailure(describe(failure), http);
  return error;
}
