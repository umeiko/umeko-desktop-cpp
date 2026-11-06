import {test} from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {Gateway} from '../runtime/gateway.mjs';

async function fixture(t,handler) {
  const server=http.createServer(handler);await new Promise(r=>server.listen(0,'127.0.0.1',r));
  const gateway=new Gateway();gateway.configure({token:'fixture-private',python:process.env.UMEKO_TEST_PYTHON || '/usr/bin/python3',
    providers:[{id:'fixture',name:'Fixture',api:'openai',key:'secret-fixture',baseUrl:`http://127.0.0.1:${server.address().port}/v1`,proxy:{mode:'direct'},models:[{alias:'text',upstream:'upstream',vision:false}]}],
    multimodal:{image:{providerId:'fixture',model:'image-fixture',count:1},speech:{providerId:'fixture',model:'voice-fixture',voice:'alloy',format:'mp3',speed:1},video:{providerId:'fixture',model:'video-fixture'}}});
  const port=await gateway.start();
  t.after(async()=>{await gateway.close();server.closeAllConnections();await new Promise(r=>server.close(r));});
  return {gateway,url:`http://127.0.0.1:${port}`,server};
}
test('private media bridge uses the chosen account and preserves UTF-8',async t=>{
  const requests=[];
  const f=await fixture(t,async(req,res)=>{
    let raw='';for await(const c of req)raw+=c;requests.push({path:req.url,auth:req.headers.authorization,body:JSON.parse(raw)});
    res.setHeader('Content-Type','application/json');res.end(JSON.stringify({data:[{b64_json:'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jF1sAAAAASUVORK5CYII='}]}));
  });
  const response=await fetch(f.url+'/umeko/generate',{method:'POST',headers:{Authorization:'Bearer fixture-private'},body:JSON.stringify({kind:'image',prompt:'你好，图片'})});
  const value=await response.json();assert.equal(response.status,200,JSON.stringify(value));assert.equal(value.model,'image-fixture');assert.equal(value.items[0].mimeType,'image/png');
  assert.equal(requests[0].path,'/v1/images/generations');assert.equal(requests[0].auth,'Bearer secret-fixture');assert.equal(requests[0].body.prompt,'你好，图片');
  assert.ok(!JSON.stringify(value).includes('secret-fixture'));
});
test('media endpoints require the private token and reject invalid selections',async t=>{
  const f=await fixture(t,()=>{throw Error('must not contact upstream');});
  let response=await fetch(f.url+'/umeko/generate',{method:'POST',body:'{}'});assert.equal(response.status,401);
  response=await fetch(f.url+'/umeko/generate',{method:'POST',headers:{Authorization:'Bearer fixture-private'},body:JSON.stringify({kind:'unknown'})});assert.equal(response.status,400);
});
test('gateway sends explicit empty text for tool-only assistant turns',async t=>{
  let body;const f=await fixture(t,async(req,res)=>{let raw='';for await(const c of req)raw+=c;body=JSON.parse(raw);res.setHeader('Content-Type','application/json');res.end(JSON.stringify({choices:[{message:{role:'assistant',content:'ok'},finish_reason:'stop'}]}));});
  const response=await fetch(f.url+'/v1/chat/completions',{method:'POST',headers:{Authorization:'Bearer fixture-private'},body:JSON.stringify({model:'text',messages:[{role:'assistant',content:null,tool_calls:[{id:'x',type:'function',function:{name:'read',arguments:'{}'}}]}]})});
  assert.equal(response.status,200);await response.json();assert.equal(body.messages[0].content,'');assert.equal(body.messages[0].tool_calls[0].id,'x');
});

test('plugin media bridge confines small file references and validates inputs',async t=>{
  const directory=await fs.mkdtemp(path.join(os.tmpdir(),'umeko-native-media-'));
  let requests=0,escape=false;
  const gateway=new Gateway(event=>{
    if(event.type!=='provider')return;
    requests++;
    void (async()=>{
      const own=path.join(directory,event.id);await fs.mkdir(own);
      const file=path.join(escape?directory:own,'image.bin');await fs.writeFile(file,'fixture-binary');
      gateway.accept({type:'provider-event',id:event.id,event:{type:'result',result:{items:[{file,mimeType:'image/png'}]}}});
      gateway.accept({type:'provider-result',id:event.id});
    })();
  });
  gateway.configure({token:'fixture-private',generationDirectory:directory,providers:[{id:'plugin',api:'plugin',generationCapabilities:['image'],models:[]}],multimodal:{image:{providerId:'plugin',model:'draw',count:1}}});
  const port=await gateway.start();
  t.after(async()=>{await gateway.close();await fs.rm(directory,{recursive:true,force:true});});
  const post=body=>fetch(`http://127.0.0.1:${port}/umeko/generate`,{method:'POST',headers:{Authorization:'Bearer fixture-private'},body:JSON.stringify(body)});
  let response=await post({kind:'image',prompt:'hello'});assert.equal(response.status,200);
  let result=await response.json();assert.equal(Buffer.from(result.items[0].data,'base64').toString(),'fixture-binary');assert.equal(result.items[0].extension,'png');
  response=await post({kind:'image',prompt:'hello',count:1.5});assert.equal(response.status,400);assert.equal(requests,1);
  escape=true;response=await post({kind:'image',prompt:'hello'});assert.equal(response.status,400);result=await response.json();assert.match(result.error.message,/transfer path/);
});
