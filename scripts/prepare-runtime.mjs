// Build-time projection only. The complete Pi coding implementation is retained.
import { cp, mkdir, readFile, writeFile, access } from 'node:fs/promises';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const argument = name => { const index = process.argv.indexOf(name); return index < 0 ? undefined : process.argv[index + 1]; };
const source = resolve(argument('--source') || join(root, 'runtime'));
const output = resolve(argument('--output') || join(root, 'runtime/releases/coding-1.3.0'));
const manifest = JSON.parse(await readFile(join(source, 'node_modules/@earendil-works/pi-coding-agent/package.json'), 'utf8'));
await mkdir(output, { recursive: true });
let exists = false; try { await access(join(output, 'manifest.json')); exists = true; } catch { /* Initial projection. */ }
let dependenciesReady=false;
try { await access(join(output,'node_modules/@gotgenes/pi-subagents/package.json')); dependenciesReady=true; } catch { /* Upgrade the dependency projection. */ }
if (!exists || !dependenciesReady) {
  await cp(join(source, 'node_modules'), join(output, 'node_modules'), { recursive: true, dereference: true });
  const ai = join(output, 'node_modules/@earendil-works/pi-ai/dist');
  await writeFile(join(ai, 'providers/all.js'), `// Umeko explicit-provider policy; native coding engine is unchanged.\nimport { createModels } from '../models.js';\nexport const getBuiltinModel=()=>undefined;export const getBuiltinImageModel=()=>undefined;export const getBuiltinClassifierModel=()=>undefined;export const getBuiltinProviders=()=>[];export const getBuiltinModelDataGeneratedAt=()=>undefined;export const getBuiltinModels=()=>[];export const getBuiltinImageModels=()=>[];export const getBuiltinClassifierModels=()=>[];export const getAllBuiltinModels=()=>[];export const builtinProviders=()=>[];export const builtinModels=options=>createModels(options);export const radiusProvider=()=>{throw new Error('Configure providers in Umeko settings');};\n`);
  await writeFile(join(ai, 'models.generated.js'), 'export const MODELS={};export const IMAGE_MODELS={};export const CLASSIFIER_MODELS={};\n');
  await writeFile(join(output, 'manifest.json'), JSON.stringify({ package: '@earendil-works/pi-coding-agent', version: manifest.version, providerCatalog: false, projection: 2, engine: 'full-coding-agent', nativeTools: true }, null, 2));
}
// Diagnostic UUIDs can contain status digits such as 503. Keep the report in
// the displayed error, but exclude its opaque marker from Pi's retry matching.
const retryPath = join(output, 'node_modules/@earendil-works/pi-ai/dist/utils/retry.js');
const originalRetry = 'const errorMessage = message.errorMessage;';
const adaptedRetry = String.raw`const errorMessage = message.errorMessage.replace(/\[Umeko:[a-f0-9-]{36}\]/gi, "[Umeko report]");`;
const retrySource = await readFile(retryPath, 'utf8');
if (!retrySource.includes(adaptedRetry)) {
  if (retrySource.split(originalRetry).length !== 2) throw new Error('Pi retry classifier changed; update the diagnostics adapter');
  await writeFile(retryPath, retrySource.replace(originalRetry, adaptedRetry));
}
for (const name of ['bridge.mjs', 'package-install.mjs', 'update-engine.mjs', 'resources.mjs', 'provider-policy.mjs', 'chatbox.mjs', 'network-retry.mjs', 'pi-commands.mjs', 'umeko-control.mjs', 'umeko-documents.mjs', 'subagents.mjs', 'subagent-sdk.mjs', 'subagent-settings.mjs', 'input-queue.mjs', 'widget-messages.mjs', 'rpc-ui.mjs', 'umeko-operations.json', 'webview-component.mjs', 'webview-component.json']) await cp(join(root, 'runtime', name), join(output, name));
await cp(join(root, 'runtime/umeko-docs'), join(output, 'umeko-docs'), {recursive:true});
const factoryBin=join(root,'runtime/releases/coding-1.3.0/bin');
if(resolve(output)!==resolve(join(root,'runtime/releases/coding-1.3.0'))) {
  try { await access(factoryBin); await cp(factoryBin,join(output,'bin'),{recursive:true,dereference:true}); }
  catch(error) { if(error.code!=='ENOENT')throw error; }
}
console.log('Native Pi runtime prepared:', manifest.version);
