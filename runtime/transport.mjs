import { createRequire } from 'node:module';
import { mkdirSync, writeFileSync, readFileSync, readdirSync, statSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { transientFailure, certificateFailure, describeFailure } from './network-retry.mjs';
import { randomUUID, X509Certificate } from 'node:crypto';
import tls from 'node:tls';

const require = createRequire(new URL('./releases/coding-1.3.0/package.json', import.meta.url));
const { Agent, ProxyAgent, EnvHttpProxyAgent, fetch: providerFetch } = require('undici');
export { providerFetch };
function trustedCertificates(provider) {
  const defaults = tls.getCACertificates?.('default') || tls.rootCertificates;
  let system;
  try { system = tls.getCACertificates?.('system') || []; }
  catch (cause) { throw Object.assign(new Error('Could not load the system CA store', {cause}), {code: 'UMEKO_CA_STORE_FAILED'}); }
  const certificates = [...defaults, ...system];
  for (const file of new Set([process.env.UMEKO_MODEL_CA_FILE, provider.proxy?.caFile].filter(Boolean))) {
    try {
      if (!statSync(file).isFile() || statSync(file).size > 4 * 1024 * 1024) throw new Error('Expected a PEM file up to 4 MiB');
      const text = readFileSync(file, 'utf8');
      const blocks = text.match(/-----BEGIN CERTIFICATE-----[\s\S]+?-----END CERTIFICATE-----/g);
      if (!blocks?.length) throw new Error('No PEM certificates found');
      for (const block of blocks) { new X509Certificate(block); certificates.push(block); }
    } catch (cause) {
      throw Object.assign(new Error('The configured CA file is missing, unreadable or invalid', {cause}), {code: 'UMEKO_CA_FILE_INVALID'});
    }
  }
  return [...new Set(certificates)];
}

export function dispatcher(provider, {systemCa = false} = {}) {
  const config = provider.proxy || {};
  // Always validate the peer, even if an unrelated parent process disabled TLS
  // verification. Custom roots extend the default bundle instead of replacing it.
  const secure = {rejectUnauthorized: true, ...(systemCa ? {ca: trustedCertificates(provider)} : {})};
  const options = {connect: secure, requestTls: secure, proxyTls: secure};
  if (config.mode === 'direct') return new Agent(options);
  if (config.mode === 'custom') return new ProxyAgent({...options, uri: config.url});
  return new EnvHttpProxyAgent({
    ...options,
    httpProxy: process.env.HTTP_PROXY || process.env.http_proxy || provider.systemProxy || '',
    httpsProxy: process.env.HTTPS_PROXY || process.env.https_proxy || provider.systemProxy || '',
    noProxy: process.env.NO_PROXY || process.env.no_proxy || ''
  });
}

// Remember a successful trust expansion, without changing process-wide TLS
// defaults or another provider's CA policy. No per-token work is involved.
const recoveredProviders = new Set();
export async function fetchProvider(provider, url, options, record) {
  const key = JSON.stringify([provider.id, provider.baseUrl, provider.proxy, process.env.UMEKO_MODEL_CA_FILE]);
  let expanded = !!(provider.proxy?.caFile || process.env.UMEKO_MODEL_CA_FILE || recoveredProviders.has(key));
  record.tls = {attempts: []};
  for (let attempt = 0; attempt < 2; attempt++) {
    options.signal?.throwIfAborted();
    const trust = expanded ? 'default+system+configured' : 'default';
    const status = {trust}; record.tls.attempts.push(status);
    let agent;
    try {
      agent = dispatcher(provider, {systemCa: expanded});
      const response = await providerFetch(url, {...options, dispatcher: agent});
      if (expanded) {
        recoveredProviders.delete(key); recoveredProviders.add(key);
        if (recoveredProviders.size > 64) recoveredProviders.delete(recoveredProviders.values().next().value);
      }
      return {response, agent};
    } catch (error) {
      status.error = describeFailure(error);
      await agent?.destroy();
      // Only a failed handshake may be replayed. Once headers/body have been
      // returned, the caller owns the stream and no TLS fallback can resend it.
      if (expanded || attempt || options.signal?.aborted || !certificateFailure(error)?.recoverable) throw error;
      expanded = true;
    }
  }
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
  const record = redact({ id, at: new Date().toISOString(), provider: provider.name, request, response,
    error: describeFailure(failure), http }, [provider.key]);
  if (directory) {
    mkdirSync(directory, { recursive: true, mode: 0o700 });
    writeFileSync(join(directory, id + '.json'), JSON.stringify(record, null, 2), { mode: 0o600 });
    for (const file of readdirSync(directory).filter(name => /^[a-f0-9-]{36}\.json$/.test(name)).map(name => join(directory, name))
      .sort((a, b) => statSync(b).mtimeMs - statSync(a).mtimeMs).slice(30)) unlinkSync(file);
  }
  const certificate = certificateFailure(failure);
  const error = new Error(certificate
    ? `TLS certificate validation failed (${certificate.code}); check the provider's Network settings and CA file, then open request details [Umeko:${id}]`
    : `Provider request failed; open request details [Umeko:${id}]`);
  error.reportId = id; error.cause = failure; error.retryable = transientFailure(failure, http);
  if (certificate) error.tlsCode = certificate.code;
  return error;
}
