// Exact request formats; independent of provider/model names. Mirrors the
// published Python thinking_formats.py compatibility adapter (Pi MIT).
export const formats = ['openai','openai-none','chat-template','qwen','deepseek','anthropic-adaptive','anthropic','responses'];
export function thinkingParameters(format, effort) {
  if(!formats.includes(format))throw Error('Unknown thinking format');
  const on=effort!=='off';
  if(format==='openai-none')return {reasoning_effort:on?effort:'none'};
  if(['openai','anthropic','responses'].includes(format))return on?{reasoning_effort:effort}:{};
  if(format==='chat-template')return {chat_template_kwargs:{enable_thinking:on,...(on?{reasoning_effort:effort}:{})}};
  return {...(format==='qwen'?{enable_thinking:on}:{thinking:{type:on?(format==='anthropic-adaptive'?'adaptive':'enabled'):'disabled'}}),...(on?{reasoning_effort:effort}:{})};
}
export function selectedFormat(model, effort) {
  return model.reasoningFormatAuto!==false && model.reasoningFormatDetection?.state==='known'
    ? (effort === 'off' && model.reasoningFormatDetection.offFormat || model.reasoningFormatDetection.format) || model.reasoningFormat || 'openai' : model.reasoningFormat || 'openai';
}
