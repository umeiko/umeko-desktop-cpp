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
if (!exists) {
  await cp(join(source, 'node_modules'), join(output, 'node_modules'), { recursive: true, dereference: true });
  const ai = join(output, 'node_modules/@earendil-works/pi-ai/dist');
  await writeFile(join(ai, 'providers/all.js'), `// Umeko explicit-provider policy; native coding engine is unchanged.\nimport { createModels } from '../models.js';\nexport const getBuiltinModel=()=>undefined;export const getBuiltinImageModel=()=>undefined;export const getBuiltinClassifierModel=()=>undefined;export const getBuiltinProviders=()=>[];export const getBuiltinModelDataGeneratedAt=()=>undefined;export const getBuiltinModels=()=>[];export const getBuiltinImageModels=()=>[];export const getBuiltinClassifierModels=()=>[];export const getAllBuiltinModels=()=>[];export const builtinProviders=()=>[];export const builtinModels=options=>createModels(options);export const radiusProvider=()=>{throw new Error('Configure providers in Umeko settings');};\n`);
  await writeFile(join(ai, 'models.generated.js'), 'export const MODELS={};export const IMAGE_MODELS={};export const CLASSIFIER_MODELS={};\n');
  await writeFile(join(output, 'manifest.json'), JSON.stringify({ package: '@earendil-works/pi-coding-agent', version: manifest.version, providerCatalog: false, projection: 2, engine: 'full-coding-agent', nativeTools: true }, null, 2));
}
for (const name of ['bridge.mjs', 'resources.mjs', 'provider-policy.mjs']) await cp(join(root, 'runtime', name), join(output, name));
console.log('Native Pi runtime prepared:', manifest.version);
