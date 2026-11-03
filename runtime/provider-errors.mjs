// Bounded provider summaries; credentials and response HTML never become UI markup.
export function safeErrorText(value, secrets = []) {
  let text=String(value ?? '');
  for(const secret of secrets)if(typeof secret==='string' && secret.length>=3)text=text.split(secret).join('[REDACTED]');
  return text.replace(/(https?:\/\/)[^\s/@]+:[^\s/@]+@/gi,'$1[REDACTED]@')
    .replace(/\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]+/gi,'$1 [REDACTED]')
    .replace(/(["']?(?:authorization|proxy-authorization|x-api-key|api[_-]?key|access[_-]?token|refresh[_-]?token|token|password|client[_-]?secret|cookie|set-cookie)["']?\s*[:=]\s*)(?:"[^"\r\n]*"|'[^'\r\n]*'|[^\s,;}]+)/gi,'$1[REDACTED]')
    .replace(/\bsk-[A-Za-z0-9_-]+|\bAKIA[A-Z0-9]{16}\b|\beyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+/g,'[REDACTED]');
}
const line = value => String(value??'').replace(/<(script|style)\b[^>]*>[\s\S]*?(?:<\/\1\s*>|$)/gi,' ').replace(/<!--[\s\S]*?-->|<[^>]+>/g,' ').replace(/&(?:nbsp|lt|gt|amp|quot|apos);/g,x=>({'&nbsp;':' ','&lt;':'<','&gt;':'>','&amp;':'&','&quot;':'"','&apos;':"'"}[x])).replace(/[\s\x00-\x1f\x7f]+/g,' ').trim();
function secretValues(value, result=[]) {
  if(value && typeof value==='object')for(const [key,item]of Object.entries(value)) {
    if(/authorization|api.?key|cookie|password|token$|client.?secret/i.test(key) && typeof item==='string')result.push(item);
    else if(item && typeof item==='object')secretValues(item,result);
  }
  return result;
}
export function formatHttpError(status, body='', headers={}, secrets=[]) {
  let payload=body;
  if(typeof body==='string')try{payload=JSON.parse(body);}catch{payload=null;}
  secrets=[...secrets,...secretValues(payload),...secretValues(headers)];
  const details=payload && typeof payload==='object' && !Array.isArray(payload)?payload.error??payload:payload;
  let reason='',metadata=[];
  if(details && typeof details==='object' && !Array.isArray(details)) {
    reason=details.message||details.detail||payload.message||payload.detail||'';
    for(const name of ['type','param','code'])if(details[name]!=null && details[name]!=='')metadata.push(name+'='+details[name]);
  } else if(typeof details==='string')reason=details;
  if(typeof reason!=='string')reason=JSON.stringify(reason);
  if(!reason)reason=typeof body==='string'?body:JSON.stringify(body);
  reason=line(safeErrorText(line(reason),secrets))||'服务器未返回错误详情';
  status=Number(status)||0;
  const hint=({401:'请检查 API 密钥与权限',403:'请检查 API 密钥与权限',404:'请检查地址或模型名称',408:'请求超时，请检查网络或超时设置',413:'请求过大或上下文过长',429:'请求受到限流，请稍后重试'})[status] || (status>=500 && status<=599?'供应商服务端错误':/context[_ -]?(?:length|window)|maximum context|too many tokens|上下文.{0,8}(?:超|长)/i.test(reason+metadata.join())?'请求过大或上下文过长':'');
  let suffix=(metadata.length?' ['+metadata.map(item=>line(safeErrorText(item,secrets)).slice(0,100)).join(', ')+']':'')+(hint?'（提示：'+hint+'）':'');
  const retry=Object.entries(headers).find(([key])=>key.toLowerCase()==='retry-after')?.[1];
  if(status===429 && retry)suffix+=' [Retry-After: '+line(safeErrorText(retry,secrets)).slice(0,100)+']';
  const prefix='供应商返回 HTTP '+status+'：', max=1000-prefix.length-suffix.length;
  return prefix+(reason.length>max?reason.slice(0,max-1)+'…':reason)+suffix;
}
