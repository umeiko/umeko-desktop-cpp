import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import {formatHttpError} from '../runtime/provider-errors.mjs';
import {Gateway} from '../runtime/gateway.mjs';
import {failureReport} from '../runtime/transport.mjs';
for(const status of [400,401,403,404,408,413,429,500,503])test('provider HTTP summary '+status,()=>{
 const message=formatHttpError(status,{error:{message:'context_length exceeded: Bearer token-value credential-value',code:'limit',api_key:'credential-value'}},{'Retry-After':'3'},['token-value']);
 assert.match(message,new RegExp('HTTP '+status));assert.match(message,/context_length/);assert.match(message,/code=limit/);assert(!message.includes('token-value'));assert(!message.includes('credential-value'));
 assert(formatHttpError(status,'x'.repeat(10000)).length<=1000);
 const report=failureReport('',{name:'Fixture',key:'fixture-secret'},{},{status,body:'fixture-secret failure',headers:{}},new Error('HTTP '+status),[{response:{status}}]);
 assert.match(report.message,new RegExp('HTTP '+status));assert(!report.message.includes('fixture-secret'));assert.equal(report.retryable,[408,429,500,503].includes(status));
});
test('arrays and HTML bodies stay bounded and plain',()=>{
 assert.match(formatHttpError(400,[{message:'invalid'}]),/invalid/);
 const html=formatHttpError(500,'<script>evil()</script><h1>Temporarily unavailable</h1>');assert(!html.includes('script'));assert(!html.includes('evil'));assert.match(html,/Temporarily unavailable/);
});
test('loopback gateway preserves redacted upstream status and reason',async t=>{
 const upstream=http.createServer(async(req,res)=>{for await(const chunk of req){}res.writeHead(401,{'Content-Type':'application/json'});res.end(JSON.stringify({error:{message:'密钥 fixture-secret 已失效' ,code:'invalid_api_key'}}));});
 await new Promise(resolve=>upstream.listen(0,'127.0.0.1',resolve));
 const gateway=new Gateway(()=>{});t.after(async()=>{await gateway.close();upstream.closeAllConnections();await new Promise(resolve=>upstream.close(resolve));});
 const port=await gateway.start();gateway.configure({token:'private',providers:[{id:'provider',api:'openai',key:'fixture-secret',proxy:{mode:'direct'},baseUrl:`http://127.0.0.1:${upstream.address().port}/v1`,models:[{alias:'model',upstream:'model'}]}],models:[]});
 const response=await fetch(`http://127.0.0.1:${port}/v1/chat/completions`,{method:'POST',headers:{Authorization:'Bearer private'},body:JSON.stringify({model:'model',messages:[{role:'user',content:'hello'}]})});
 const result=await response.text();assert.match(result,/HTTP 401/);assert.match(result,/invalid_api_key/);assert.match(result,/密钥/);assert(!result.includes('fixture-secret'));assert.match(result,/Umeko:/);
});
