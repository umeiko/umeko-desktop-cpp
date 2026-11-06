// Native host adapter around the same bounded/cancellable media transports as
// the Python release. No Qt or credentials are needed in the tool process.
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';

export async function* generateMedia(config, body, signal) {
  signal?.throwIfAborted();
  if (!['image', 'speech', 'video'].includes(body?.kind)) throw Error('Invalid generation kind');
  const selected = config.multimodal?.[body.kind];
  const provider = config.providers.find(row => row.id === selected?.providerId);
  if (!provider || provider.api !== 'openai' || !provider.key || !selected.model)
    throw Error('Configure a compatible generation provider and model first');
  if (!config.python) throw Error('Python runtime is unavailable');
  const proxy = provider.proxy || {mode:'system'};
  const account = {...provider, proxy: proxy.mode === 'system' && provider.systemProxy
    ? {...proxy,mode:'custom',url:provider.systemProxy} : proxy};
  const child = spawn(config.python, ['-B', fileURLToPath(new URL('../model-runtime/worker.py', import.meta.url))],
    {stdio:['pipe','pipe','pipe'],windowsHide:true});
  let killed, expired = false, stderrBytes = 0;
  child.stderr.on('data', chunk => { stderrBytes += chunk.length; if (stderrBytes > 65536) child.kill(); });
  const stop = () => {
    child.stdin.write('cancel\n', () => {});
    killed = setTimeout(() => child.kill(), 5000);
  };
  child.stdout.setEncoding('utf8');
  signal?.addEventListener('abort', stop, {once:true});
  const deadline = setTimeout(() => {expired=true;stop();}, 15 * 60 * 1000);
  child.stdin.on('error', () => {});
  child.stdin.write(JSON.stringify({settings:{providers:[account],multimodal:config.multimodal},body})+'\n');
  let buffer = '', finished = false, exitCode;
  const exit = new Promise((resolve,reject) => {child.once('error',reject);child.once('close',code=>{exitCode=code;resolve(code);});});
  // Keep spawn errors observed even when stdout exits first.
  exit.catch(() => {});
  try {
    for await (const chunk of child.stdout) {
      signal?.throwIfAborted();
      buffer += chunk.toString('utf8');
      if (buffer.length > 90 * 1024 * 1024) throw Error('Generation output exceeds limit');
      let split;
      while ((split=buffer.indexOf('\n'))>=0) {
        const value=JSON.parse(buffer.slice(0,split));buffer=buffer.slice(split+1);
        if (value.type === 'error') throw Error(value.error?.message || 'Generation failed');
        if (value.type === 'cancelled') throw Error('Generation cancelled');
        if (value.type === 'result') finished=true;
        yield value;
      }
    }
    await exit;
    signal?.throwIfAborted();
    if (expired || exitCode !== 0 || !finished || buffer.trim()) throw Error('Generation worker did not complete');
  } finally {
    clearTimeout(deadline);clearTimeout(killed);signal?.removeEventListener('abort',stop);
    child.stdin.end();
    if (child.exitCode === null) child.kill();
    await exit.catch(() => {});
  }
}
