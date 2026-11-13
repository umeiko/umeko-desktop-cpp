// Header policy for the built-in OpenAI/Anthropic transports. Plugin SDKs own
// their signed requests, login flows and downloads; these rules do not alter them.
export const defaults = [
  {id:'content-type',name:'Content-Type',value:'application/json',enabled:true},
  {id:'user-agent',name:'User-Agent',value:'Umeko-Desktop',enabled:true},
  {id:'authorization',name:'Authorization',value:'Bearer {{apiKey}}',enabled:true},
  {id:'x-api-key',name:'x-api-key',value:'{{apiKey}}',enabled:true},
  {id:'anthropic-version',name:'anthropic-version',value:'2023-06-01',enabled:true},
];
const scopes = new Map([['authorization','openai'],['x-api-key','anthropic'],['anthropic-version','anthropic']]);
const managed = new Set(['host','content-length','transfer-encoding','connection','proxy-authorization','proxy-connection','upgrade','trailer','te']);
export function validateHeaders(value = []) {
  if (!Array.isArray(value) || value.length > 32) throw Error('请求头最多可设置 32 项');
  const ids = new Set();
  let rows = value.map(row => {
    if (!row || Object.keys(row).sort().join() !== 'enabled,id,name,value' || typeof row.id !== 'string' || row.id.length<1 || row.id.length>64 || /[^a-z0-9-]/.test(row.id) || ids.has(row.id) || typeof row.enabled !== 'boolean') throw Error('请求头配置格式无效');
    if (typeof row.name !== 'string' || row.name.length<1 || row.name.length > 128 || /[^!#$%&'*+.^_`|~0-9A-Za-z-]/.test(row.name)) throw Error('请求头名称只能包含英文字母、数字和 HTTP 字段符号');
    if (managed.has(row.name.toLowerCase())) throw Error('此请求头由网络传输管理，不能自定义');
    if (typeof row.value !== 'string' || row.value.length > 4096 || /[\x00-\x1f\x7f\u0100-\uffff]/.test(row.value)) throw Error('请求头值不能包含换行、控制字符或非 Latin-1 字符，且最多 4096 字符');
    ids.add(row.id); return {...row};
  });
  rows = [...defaults.filter(row => !ids.has(row.id)).map(row=>({...row})), ...rows];
  if (rows.length > 32) throw Error('请求头最多可设置 32 项');
  for (const protocol of ['openai','anthropic']) {
    const names = rows.filter(row=>row.enabled && (scopes.get(row.id) || protocol)===protocol).map(row=>row.name.toLowerCase());
    if (new Set(names).size !== names.length) throw Error('启用的请求头名称不能重复（不区分大小写）');
  }
  return rows;
}
export function buildHeaders(value, provider) {
  const protocol = provider.api === 'anthropic' ? 'anthropic' : 'openai';
  const rows = validateHeaders(value ?? []).filter(row => (scopes.get(row.id) || protocol) === protocol);
  const headers = Object.fromEntries(rows.filter(row=>row.enabled).map(row=>[row.name,row.value.replaceAll('{{apiKey}}',()=>provider.key || '')]));
  if (Object.values(headers).some(text=>text.length>4096 || /[\x00-\x1f\x7f\u0100-\uffff]/.test(text))) throw Error('展开密钥后的请求头值无效或超过 4096 字符');
  const present = new Set(Object.keys(headers).map(name=>name.toLowerCase()));
  const omitHeaders = [...new Set([...rows.filter(row=>!row.enabled).map(row=>row.name.toLowerCase()),
    ...defaults.filter(row=>(scopes.get(row.id) || protocol)===protocol).map(row=>row.name.toLowerCase())])].filter(name=>!present.has(name));
  return {headers,omitHeaders};
}
