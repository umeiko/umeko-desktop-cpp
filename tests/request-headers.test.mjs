import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import {mkdtemp,readFile,readdir,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {defaults,validateHeaders,buildHeaders} from '../runtime/request-headers.mjs';
import {Gateway} from '../runtime/gateway.mjs';
import {generateMedia} from '../runtime/native-generation.mjs';

const policy = () => {
  const rows=structuredClone(defaults); rows[1].value='My-Client/1.0'; rows[2].name='X-Custom-Auth';
  rows.push({id:'custom-project',name:'X-Project',value:'opaque-header-secret',enabled:true});return rows;
};
test('default, custom, disabled and protocol scoped header validation',()=>{
  assert.deepEqual(validateHeaders([]),defaults);
  for(const patch of [{name:'X\r\nInjected'},{name:'X-End\n'},{id:'trailing\n'},{value:'text\nnext'},{value:'中文'},{enabled:1},{name:'Host'},{name:''},{name:'USER-agent'}]) {
    const rows=policy();Object.assign(rows.at(-1),patch);assert.throws(()=>validateHeaders(rows));
  }
  const duplicate=policy(); duplicate.at(-1).name='User-Agent'; duplicate.at(-1).enabled=false;validateHeaders(duplicate);
  assert.throws(()=>validateHeaders([defaults[0],defaults[0]]));
  assert.equal(buildHeaders([], {api:'anthropic',key:'fixture'}).headers['x-api-key'],'fixture');
  assert.equal(buildHeaders([], {api:'openai',key:'fixture'}).headers.Authorization,'Bearer fixture');
  const special=policy();special.at(-1).id='constructor';
  assert.equal(buildHeaders(special,{api:'openai',key:'dollar$&token'}).headers['X-Custom-Auth'],'Bearer dollar$&token');
  assert.equal(buildHeaders(special,{api:'openai',key:'fixture'}).headers['X-Project'],'opaque-header-secret');
  for (const key of ['x\nInjected','x'.repeat(4097)]) assert.throws(()=>buildHeaders(special,{api:'openai',key}));
});

async function fixture(t) {
  const calls=[];let failed=false;
  const server=http.createServer(async(req,res)=>{
    let raw='';for await(const chunk of req)raw+=chunk;
    calls.push({path:req.url,headers:req.headers,body:raw?JSON.parse(raw):{}});
    res.setHeader('Content-Type','application/json');
    if(failed){res.statusCode=400;res.end(JSON.stringify({error:{message:'echoed opaque-header-secret and fixture-key'}}));return;}
    if(req.url.endsWith('/images/generations'))return res.end(JSON.stringify({data:[{url:`http://127.0.0.1:${server.address().port}/download`}]}));
    if(req.url==='/download'){res.setHeader('Content-Type','image/png');return res.end(Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII=','base64'));}
    res.end(JSON.stringify(req.url.endsWith('/messages')?{id:'fixture',type:'message',role:'assistant',content:[{type:'text',text:'ok'}],stop_reason:'end_turn',usage:{}}:
      req.url.endsWith('/responses')?{id:'fixture',object:'response',status:'completed',output:[],usage:{}}:
      {choices:[{message:{role:'assistant',content:'ok'},finish_reason:'stop'}]}));
  });
  await new Promise(r=>server.listen(0,'127.0.0.1',r));
  const provider={id:'fixture',name:'Fixture',api:'openai',baseUrl:`http://127.0.0.1:${server.address().port}/v1`,key:'fixture-key',proxy:{mode:'direct'},models:[{alias:'friendly',upstream:'actual'}]};
  const gateway=new Gateway();gateway.configure({providers:[provider],requestHeaders:policy()});
  t.after(async()=>{await gateway.close();server.closeAllConnections();await new Promise(r=>server.close(r));});
  const call=async()=>{for await(const _ of gateway.events(provider,provider.models[0],{messages:[{role:'user',content:'hello'}],tools:[],max_tokens:100,parameters:{}},new AbortController())){}}
  return {provider,gateway,calls,call,fail:()=>{failed=true;}};
}
test('real requests support rename, override, omit without fetch fallbacks, and reset',async t=>{
  const f=await fixture(t);
  for(const protocol of ['chat','responses','anthropic']) {
    f.provider.api=protocol==='anthropic'?'anthropic':'openai';f.provider.wireApi=protocol==='responses'?'responses':'';
    await f.call();const headers=f.calls.at(-1).headers;
    assert.equal(headers['user-agent'],'My-Client/1.0');assert.equal(headers['x-project'],'opaque-header-secret');
    assert.equal(headers.authorization,undefined);
    assert.equal(headers[protocol==='anthropic'?'x-api-key':'x-custom-auth'],protocol==='anthropic'?'fixture-key':'Bearer fixture-key');
    assert.equal(headers[protocol==='anthropic'?'x-custom-auth':'x-api-key'],undefined);
  }
  f.provider.api='openai';
  f.gateway.requestHeaders=f.gateway.requestHeaders.map(row=>['content-type','user-agent','custom-project'].includes(row.id)?{...row,enabled:false}:row);
  await f.call();for(const key of ['content-type','user-agent','x-project'])assert.equal(f.calls.at(-1).headers[key],undefined,key);
  f.gateway.requestHeaders=[];await f.call();assert.equal(f.calls.at(-1).headers['user-agent'],'Umeko-Desktop');assert.equal(f.calls.at(-1).headers.authorization,'Bearer fixture-key');
});
test('custom credentials and provider keys are redacted even if the upstream echoes them',async t=>{
  const f=await fixture(t),directory=await mkdtemp(join(tmpdir(),'umeko-header-errors-'));
  t.after(()=>rm(directory,{recursive:true,force:true}));f.gateway.diagnosticsDir=directory;f.fail();
  await assert.rejects(f.call(),error=>!error.message.includes('opaque-header-secret')&&!error.message.includes('fixture-key'));
  const text=await readFile(join(directory,(await readdir(directory))[0]),'utf8');assert.ok(!text.includes('opaque-header-secret'));assert.ok(!text.includes('fixture-key'));
});
test('native generation passes header policy to Python without forwarding it to media downloads',async t=>{
  const f=await fixture(t);
  for await(const _ of generateMedia({providers:[f.provider],python:process.env.UMEKO_TEST_PYTHON||'/usr/bin/python3',requestHeaders:policy(),multimodal:{image:{providerId:'fixture',model:'image'}}},{kind:'image',prompt:'test'},new AbortController().signal)){}
  assert.equal(f.calls[0].headers['x-custom-auth'],'Bearer fixture-key');assert.equal(f.calls[0].headers['user-agent'],'My-Client/1.0');
  assert.equal(f.calls[1].path,'/download');assert.equal(f.calls[1].headers['x-project'],undefined);assert.equal(f.calls[1].headers['x-custom-auth'],undefined);
});
