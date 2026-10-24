import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import {Gateway, collectOpenAI} from '../runtime/gateway.mjs';
import {responsesEvents} from '../runtime/openai-responses.mjs';
const list = async events => {const out=[];for await(const event of events)out.push(event);return out;};
test('Codex routes tools, images and tool results through the official Responses wire format',async t=>{
  const requests=[];
  const upstream=http.createServer(async(req,res)=>{
    let data='';for await(const chunk of req)data+=chunk;
    requests.push({path:req.url,headers:req.headers,body:JSON.parse(data)});
    const events=requests.length===1?[
      {type:'response.output_item.added',item:{type:'function_call',id:'fc_1',call_id:'call_1',name:'read_file',arguments:''}},
      {type:'response.function_call_arguments.delta',item_id:'fc_1',delta:'{"path":"a.txt"}'},
      {type:'response.function_call_arguments.done',item_id:'fc_1',arguments:'{"path":"a.txt"}'},
      {type:'response.completed',response:{status:'completed',output:[],usage:{input_tokens:3,output_tokens:5}}}
    ]:[{type:'response.output_text.delta',delta:'Finished'},
      {type:'response.completed',response:{status:'completed',output:[{type:'message',content:[{type:'output_text',text:'Finished'}]}],usage:{input_tokens:8,output_tokens:2}}}];
    res.writeHead(200,{'Content-Type':'text/event-stream'});
    for(const event of events)res.write('data: '+JSON.stringify(event)+'\n\n');res.end();
  });
  await new Promise(resolve=>upstream.listen(0,'127.0.0.1',resolve));
  const gateway=new Gateway(()=>{});
  t.after(async()=>{await gateway.close();upstream.closeAllConnections();await new Promise(resolve=>upstream.close(resolve));});
  const provider={id:'codex',name:'Codex(Work)',api:'openai',wireApi:'responses',baseUrl:`http://127.0.0.1:${upstream.address().port}/v1`,key:'fixture-key',proxy:{mode:'direct'}};
  const messages=[{role:'user',content:[{type:'text',text:'Read this'},{type:'image_url',image_url:{url:'data:image/png;base64,fixture'}}]}];
  const body={messages,tools:[{name:'read_file',parameters:{type:'object',properties:{path:{type:'string'}}}}],max_tokens:100,parameters:{reasoning_effort:'high'}};
  let result=await collectOpenAI(gateway.events(provider,{upstream:'gpt-6.1-sol'},body,new AbortController()));
  assert.equal(result.choices[0].finish_reason,'tool_calls');
  const calls=result.choices[0].message.tool_calls;
  assert.equal(calls[0].function.arguments,'{"path":"a.txt"}');
  messages.push({role:'assistant',content:'',tool_calls:calls},{role:'tool',tool_call_id:'call_1',content:'file contents'});
  result=await collectOpenAI(gateway.events(provider,{upstream:'gpt-6.1-sol'},body,new AbortController()));
  assert.equal(result.choices[0].message.content,'Finished');assert.equal(result.usage.prompt_tokens,8);
  const request=requests[0];
  assert.equal(request.path,'/v1/responses');assert.equal(request.headers.authorization,'Bearer fixture-key');
  assert.equal(request.body.model,'gpt-6.1-sol');assert.equal(request.body.max_output_tokens,100);assert.equal(request.body.store,false);
  assert.equal(request.body.input[0].content[1].type,'input_image');assert.equal(request.body.tools[0].name,'read_file');
  assert.deepEqual(requests[1].body.input.at(-1),{type:'function_call_output',call_id:'call_1',output:'file contents'});
});
test('Responses rejects failed/truncated streams and preserves the output limit',async()=>{
  await assert.rejects(list(responsesEvents([{type:'response.failed',response:{error:{message:'rejected'}}}])));
  await assert.rejects(list(responsesEvents([{type:'response.output_text.delta',delta:'partial'}])));
  const result=await list(responsesEvents([{type:'response.incomplete',response:{status:'incomplete',incomplete_details:{reason:'max_output_tokens'},output:[]}}]));
  assert.equal(result.at(-1).choices[0].finish_reason,'length');
  const tool=await list(responsesEvents([{object:'response',status:'completed',output:[{type:'function_call',id:'fc',call_id:'call',name:'read',arguments:'{}'}]}]));
  assert.equal(tool[0].choices[0].delta.tool_calls[0].function.arguments,'{}');
});
