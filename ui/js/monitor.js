
const { createApp, ref, reactive, computed, onMounted } = Vue;
const { ElMessage } = ElementPlus;

function mxId(ip) { return ip.replace(/[^0-9A-Za-z]/g, '_'); }

createApp({
  setup() {
    const cameras = ref([]);
    const gridMode = ref('2');
    const liveOn = ref(false),gridPage=ref(1);
    const capacity=computed(()=>Number(gridMode.value)**2);
    const shownCameras=computed(()=>cameras.value.slice((gridPage.value-1)*capacity.value,gridPage.value*capacity.value));
    const pageCount=computed(()=>Math.max(1,Math.ceil(cameras.value.length/capacity.value)));
    const fps = reactive({});
    const lastFrame=reactive({}); const now=ref(Date.now());
    function frameState(c){if(!c.connected)return '设备离线';if(!liveOn.value)return '监看已停止';if(!lastFrame[c.ip])return '等待首帧';if(now.value-lastFrame[c.ip]>3000)return '画面中断';return (fps[c.ip]||0)+' FPS';}
    const pendingFrames = {}, decoding={};let streamEpoch=0;
    function fresh(c){return c.connected && liveOn.value && lastFrame[c.ip] && now.value-lastFrame[c.ip]<=3000;}

    const connCount = computed(() => cameras.value.filter(c => c.connected).length);
    const recCount = computed(() => cameras.value.filter(c => c.recordingKnown && c.recording).length);
    const stateText = computed(() => {
      if (!cameras.value.length) return '待机';
      let t = connCount.value + '/' + cameras.value.length + ' 在线';
      if (recCount.value) t += ' · ' + recCount.value + ' 台录制中';
      else if (liveOn.value && Object.values(fps).some(v => v > 0)) t += ' · 推流中';
      return t;
    });

    function pollStatus() {
      api('status').then(r => {
        if(r.result?.ok && r.result.boundCameras){
          for(const c of r.result.boundCameras)if(!c.connected){delete lastFrame[c.ip];delete pendingFrames[c.ip];delete decoding[c.ip];fps[c.ip]=0;}
          cameras.value=r.result.boundCameras;
        }
      });
    }

    function toggleLive() {
      streamEpoch++;for(const k in lastFrame)delete lastFrame[k];for(const k in pendingFrames)delete pendingFrames[k];
      if (liveOn.value) {
        liveOn.value = false;
        for (const k in fps) delete fps[k];
        api('liveview.stop');
      } else {
        liveOn.value = true;
        api('liveview.start').then(r=>{if(!r.result?.ok)liveOn.value=false;});
      }
    }

    onEvent('liveview', d => {
      if(!liveOn.value || !cameras.value.some(c=>c.ip===d.ip && c.connected))return;
      if(decoding[d.ip])return;
      const epoch=streamEpoch,token={};decoding[d.ip]=token;
      const img=new Image();
      img.onload=()=>{if(decoding[d.ip]!==token)return;delete decoding[d.ip];
        if(epoch!==streamEpoch || !liveOn.value || !cameras.value.some(c=>c.ip===d.ip&&c.connected))return;
        pendingFrames[d.ip]=img;lastFrame[d.ip]=Date.now();fps[d.ip]=d.fps||0;};
      img.onerror=()=>{if(decoding[d.ip]===token)delete decoding[d.ip];};img.src=d.frame;
    });

    // rAF 垂直同步渲染（与主窗口同一套防撕裂机制）
    function paint() {
      requestAnimationFrame(paint);
      if (!liveOn.value) { for (const k in pendingFrames) delete pendingFrames[k]; return; }
      for (const ip in pendingFrames) {
        const img = pendingFrames[ip];
        if (!img) continue;
        const cv = document.getElementById('mx_' + mxId(ip));
        if (!cv) continue;
        try { cv.width=img.naturalWidth;cv.height=img.naturalHeight;cv.getContext('2d').drawImage(img,0,0); } catch(e){}
        delete pendingFrames[ip];
      }
    }
    onMounted(() => {
      requestAnimationFrame(paint);
      setInterval(()=>{now.value=Date.now();for(const c of cameras.value)if(!c.connected||!liveOn.value||now.value-(lastFrame[c.ip]||0)>3000)fps[c.ip]=0;},500);
      pollStatus();
      setInterval(pollStatus, 1000);
      setTimeout(() => { liveOn.value = true; api('liveview.start').then(r=>{if(!r.result?.ok)liveOn.value=false;}); }, 300);
    });

    return { shownCameras,capacity,gridPage,pageCount,fresh, frameState, cameras, gridMode, liveOn, fps, stateText, mxId, toggleLive };
  },
}).use(ElementPlus).mount('#app');
