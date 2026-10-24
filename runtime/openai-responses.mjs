// Convert the agent's chat/tool history to the official Responses API.
const textOf = value => typeof value === 'string' ? value : (value || []).filter(p => p.type === 'text').map(p => p.text || '').join('\n');
export function responsesRequest(messages, tools, maxTokens, parameters = {}) {
  const input = [];
  for (const message of messages) {
    const {role, content} = message;
    if (role === 'tool') { input.push({type:'function_call_output',call_id:message.tool_call_id,output:textOf(content)}); continue; }
    if (content && (!Array.isArray(content) || content.length)) {
      let parts = content;
      if (Array.isArray(content)) parts = role === 'assistant' ? textOf(content) : content.flatMap(p => p.type === 'text' ? [{type:'input_text',text:p.text}] : p.type === 'image_url' ? [{type:'input_image',image_url:p.image_url.url,detail:p.image_url.detail || 'auto'}] : []);
      input.push({role,content:parts});
    }
    for (const call of message.tool_calls || []) input.push({type:'function_call',call_id:call.id,name:call.function.name,arguments:call.function.arguments});
  }
  const body = {input,max_output_tokens:maxTokens,stream:true,store:false};
  if (tools.length) body.tools = tools.map(t => ({type:'function',name:t.name,description:t.description || '',parameters:t.parameters,strict:false}));
  for (const name of ['temperature','top_p','parallel_tool_calls','tool_choice']) if (parameters[name] !== undefined) body[name] = parameters[name];
  if (body.tool_choice?.type === 'function') body.tool_choice = {type:'function',name:body.tool_choice.function.name};
  if (parameters.reasoning_effort) body.reasoning = {effort:parameters.reasoning_effort};
  return body;
}
export async function* responsesEvents(events) {
  const calls = new Map(); let textSeen = false, completed = false;
  const chunk = (delta = {}, finish_reason = null, usage) => ({choices:[{index:0,delta,finish_reason}],...(usage ? {usage} : {})});
  const tool = item => {
    const index = calls.size; calls.set(item.id,{index,arguments:!!item.arguments});
    return chunk({tool_calls:[{index,id:item.call_id,type:'function',function:{name:item.name,arguments:item.arguments || ''}}]});
  };
  for await (const event of events) {
    const kind = event.type;
    if (kind === 'error' || kind === 'response.failed') throw new Error(JSON.stringify(event.error || event.response?.error || 'Responses API failed'));
    if (kind === 'response.output_text.delta' || kind === 'response.refusal.delta') { textSeen = true; yield chunk({content:event.delta}); }
    else if (kind === 'response.reasoning_summary_text.delta') yield chunk({reasoning_content:event.delta});
    else if (kind === 'response.output_item.added' && event.item.type === 'function_call') yield tool(event.item);
    else if (['response.function_call_arguments.delta','response.function_call_arguments.done'].includes(kind)) {
      const slot = calls.get(event.item_id); if (!slot) throw new Error('Responses tool call missing its declaration');
      const args = kind.endsWith('.delta') ? event.delta : !slot.arguments ? event.arguments : '';
      if (args) {slot.arguments = true; yield chunk({tool_calls:[{index:slot.index,function:{arguments:args}}]});}
    } else if (['response.completed','response.incomplete'].includes(kind) || event.object === 'response') {
      const response = event.response || event;
      if (response.status === 'failed') throw new Error(JSON.stringify(response.error));
      for (const item of response.output || []) {
        if (item.type === 'function_call' && !calls.has(item.id)) yield tool(item);
        else if (item.type === 'message' && !textSeen) {const content = (item.content || []).map(p => p.text || p.refusal || '').join(''); if (content) yield chunk({content});}
      }
      const reason = response.incomplete_details?.reason;
      if (response.status === 'incomplete' && !['max_output_tokens','content_filter'].includes(reason)) throw new Error('Incomplete Responses API result: ' + reason);
      const finish = reason === 'max_output_tokens' ? 'length' : reason === 'content_filter' ? 'content_filter' : calls.size ? 'tool_calls' : 'stop';
      const raw = response.usage || {};
      const usage = {prompt_tokens:raw.input_tokens || 0,completion_tokens:raw.output_tokens || 0,total_tokens:raw.total_tokens || 0,
        ...(raw.input_tokens_details ? {prompt_tokens_details:raw.input_tokens_details} : {}),...(raw.output_tokens_details ? {completion_tokens_details:raw.output_tokens_details} : {})};
      yield chunk({},finish,usage); completed = true;
    }
  }
  if (!completed) throw new Error('Responses API stream ended before completion');
}
