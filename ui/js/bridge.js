const bridge=window.chrome?.webview;
let rpcId=0; const pending=new Map(),eventHandlers={};
if(bridge) bridge.addEventListener('message',({data:d})=>{
 if(d?.id!==undefined&&pending.has(d.id)){const p=pending.get(d.id);pending.delete(d.id);clearTimeout(p.timer);p.resolve(d);
 if(d.result?.ok===false && window.ElementPlus) ElementPlus.ElMessage.error(`${d.result.message} [${d.result.code}]`);
 }else if(d?.event) for(const f of eventHandlers[d.event]||[]) f(d);
});
function api(method,params={}){if(!bridge)return Promise.resolve({result:{ok:false,code:'NO_HOST',message:'请从桌面程序打开'}});
 return new Promise(resolve=>{const id=++rpcId;const timer=setTimeout(()=>{pending.delete(id);resolve({result:{ok:false,code:'TIMEOUT',message:'操作超时，请检查日志'}});},120000);pending.set(id,{resolve,timer});bridge.postMessage(JSON.stringify({id,method,params}));});}
function onEvent(name,fn){(eventHandlers[name] ||= []).push(fn);}
