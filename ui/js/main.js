
const { createApp, ref, reactive, computed, onMounted, nextTick, watch } = Vue;
// Element Plus UMD 包只把 ElMessage/ElMessageBox 挂在 ElementPlus 上，不是独立全局变量
const { ElMessage, ElMessageBox } = ElementPlus;

const app = createApp({
  setup() {
    // ---------- CrSDK 属性中文翻译表（码值取自 CrDeviceProperty.h 枚举） ----------
    // 格式: 码值: [分组, 中文名, 值格式]  fmt: fn=F值÷100 sh=1/x秒 ang=角度 k=色温 db=dB zoom=倍率÷100
    const cameras = ref([]);
    const selectedIp = ref('');
    const appVersion = ref('');
    const propSearch = ref('');
    const advanced = ref(false);
    const logsOpen = ref(false), logLevel = ref('ALL'), logPaused = ref(false);
    let propsEpoch = 0, wizardEpoch=0, qualityEpoch=0;
    const busy=reactive({}), qualityTarget=ref("camera");
    const page = ref('camera');   // 'camera' 相机库/详情 | 'settings' 软件设置（监看已独立成窗口）
    const atem = ref({ connected: false });
    const atemIp = ref('192.168.1.240');
    const atemPort = ref(9910);
    const sdkOk = ref(false);
    const ruleCount = ref(0);
    const recording = ref(false);
    const connected = ref(false);
    const logs = ref([]);
    const props = ref([]);
    const logBox = ref(null);
    const modelNames = ref([]);
    const wizardVisible = ref(false);
    const wizardStep = ref(1);
    const wizard = reactive({ method: 'wifi', ip: '', modelName: 'ILME-FX3', sshUser: '', sshPass: '',
                              searching: false, connecting: false, found: [], selected: null, name: '',
                              pairing: 'idle', pairingMsg: '' });
    const sshDialogVisible = ref(false);

    const current = computed(() => cameras.value.find(c => c.ip === selectedIp.value) || null);

    // ---------- 属性分组与过滤 ----------
    const propsLoading = ref(false);
    const propGroup = ref('全部');
    const propGroups = computed(() => {
      const present = new Set(props.value.map(p => (PROP_META[p.code] ? PROP_META[p.code][0] : '其他')));
      return GROUP_ORDER.filter(g => g === '全部' || present.has(g));
    });
    const filteredProps = computed(() => {
      return props.value.filter(p => {
        const q=propSearch.value.trim().toLowerCase();
        if(q && !propName(p.code).toLowerCase().includes(q) && !('0x'+p.code.toString(16)).includes(q)) return false;
        if(propGroup.value === '全部') return true;
        const g = PROP_META[p.code] ? PROP_META[p.code][0] : '其他';
        return g === propGroup.value;
      });
    });
    function fmtVal(row) { if(!row.readable)return "不可读";if(typeof row.value==='string')return row.value;return fmtCode(row.code,row.value); }

    // ---------- 多机监看独立窗口 ----------
    function openMonitor() { api('monitor.open'); }
    function openSettings() {
      page.value = page.value === 'settings' ? 'camera' : 'settings';
      if (page.value === 'settings') loadLvQuality();
    }

    // ---------- 监看画质设置 ----------
    const lvRes = ref(1);
    const lvComp = ref(-1);
    const qMin = ref(1);
    const qMax = ref(0);   // 0 = 尚未获取/机型不支持（隐藏滑块）
    const wasConnected = ref(false);
    async function loadLvQuality() {
      const ip=qualityTarget.value==='default'?'':selectedIp.value, epoch=++qualityEpoch;
      qMax.value=0;lvComp.value=-1;
      const r=await api('liveview.quality',{ip});
      if(epoch!==qualityEpoch || ip!==(qualityTarget.value==='default'?'':selectedIp.value) || !r.result?.ok) return;
      lvRes.value=r.result.imageQuality??1;lvComp.value=r.result.compression??-1;
      if(r.result.min!==undefined && r.result.max!==undefined){qMin.value=r.result.min;qMax.value=r.result.max;}
    }
    async function applyLvQuality() {
      const ip=qualityTarget.value==='default'?'':selectedIp.value;
      if(qualityTarget.value==='camera' && !ip) return;
      const r=await api('liveview.setQuality',{ip,imageQuality:lvRes.value,compression:lvComp.value});
      if(r.result?.ok) ElMessage.success(ip?'已保存当前相机画质':'已保存默认画质，下次启动监看生效');
    }

    function dotClass(c) {
      if (c.recording) return 'rec';
      if (c.connected) return 'on';
      return '';
    }

    // ---------- 轮询状态 ----------
    async function pollStatus() {
      try {
        const r = await api('status');
        if (!r.result?.ok || !r.result.app) return;
        const s = r.result;
        appVersion.value = s.app.version;
        cameras.value = s.boundCameras || [];
        atem.value = s.atem;
        sdkOk.value = s.app.sdkInitialized;
        ruleCount.value = s.ruleCount;
        // 多相机：详情页状态 = 当前选中相机的实时状态（connected/recording 由后端按相机下发）
        const cur = current.value;
        connected.value = !!(cur && cur.connected);
        recording.value = !!(cur && cur.recording);
        // 相机连上瞬间拉一次监看画质范围（FX3 不支持压缩质量属性时滑块自动隐藏）
        if (connected.value && !wasConnected.value) {loadLvQuality();loadProps();}
        wasConnected.value = connected.value;
        // 启动后自动选中相机库中的相机
        if (!selectedIp.value && s.boundCameras && s.boundCameras.length) {
          selectCam(s.boundCameras[0].ip);
        }
      } catch (e) {}
    }

    async function pollLogs() {
      if(logPaused.value) return;
      try {
        const r = await api('logs');
        if (r.result && r.result.entries) {
          const incoming = r.result.entries;
          if (incoming.length !== logs.value.length ||
              JSON.stringify(incoming.slice(-3)) !== JSON.stringify(logs.value.slice(-3))) {
            logs.value = incoming;
            nextTick(() => { if (logBox.value) logBox.value.scrollTop = logBox.value.scrollHeight; });
          }
        }
      } catch (e) {}
    }

    // ---------- 操作 ----------
    function selectCam(ip) {
      selectedIp.value = ip;
      props.value = []; propsEpoch++;propsLoading.value=false;wasConnected.value=false;
      if(page.value==='settings') loadLvQuality();
      // 切换监看/录制目标：后端记录选中相机（监看在独立窗口，推流由后端按连接相机遍历）
      api('cameras.setActive', { ip });
      loadProps();
    }
    async function record(action) {const ip=selectedIp.value;if(busy[ip])return;busy[ip]=true;
      try{const r=await api('record.'+action,{ip});if(r.result?.ok){ElMessage.success('录制指令已提交，等待相机确认');if(ip===selectedIp.value)loadProps();}}finally{busy[ip]=false;}}
    async function connectToggle(){const c=current.value;if(!c || busy[c.ip])return;const ip=c.ip;busy[ip]=true;
      try{await api(c.connected?'cameras.disconnect':'cameras.connectByIp',{ip,modelId:c.modelId});pollStatus();}finally{busy[ip]=false;}}
    function phaseName(c){return ({online:'已连接',offline:'手动断开',connecting:'连接中',retry_wait:'等待重连',failed:'连接失败'})[c?.phase]||'离线';}

    // ---------- Camera ID 步进器 ----------
    const idInput = ref('');
    // 相机 ID 变化（轮询/本地更新）→ 同步输入框显示
    watch(() => (current.value ? current.value.cameraId : null), v => {
      idInput.value = v ? String(v) : '';
    }, { immediate: true });
    function commitId() {
      if (!current.value) return;
      const n = Number(idInput.value);
      if (!Number.isInteger(n) || n < 1 || n > 15) {
        idInput.value = current.value.cameraId ? String(current.value.cameraId) : '';
        ElMessage.warning('Camera ID 范围 1-15');
        return;
      }
      if (n === current.value.cameraId) return;
      const ip=selectedIp.value; api('cameras.setId',{ip,cameraId:n}).then(r=>{if(r.result?.ok && selectedIp.value===ip) current.value.cameraId=n;else idInput.value=String(current.value?.cameraId||'');});
    }
    function stepId(d) {
      const n = Math.min(15, Math.max(1, (current.value.cameraId || 0) + d));
      idInput.value = String(n);
      commitId();
    }
    function atemToggle() { api(atem.value.connected ? 'atem.disconnect' : 'atem.connect', { ip: atemIp.value, port: parseInt(atemPort.value) }); }
    async function loadProps() {
      const ip=selectedIp.value, epoch=++propsEpoch;
      if(!ip) return;
      propsLoading.value = true;
      try {
        const r = await api('props.list', { ip });
        if(epoch !== propsEpoch || selectedIp.value !== ip) return;
        if (r.result && r.result.ok) {
          props.value = r.result.properties.map(p => ({
            code: p.code, value: p.value,
            possible: p.possible || [], range:p.range, writable: p.writable, readable: p.readable,
            edit: p.value,
          }));
        } else props.value = [];
      } finally { if(epoch===propsEpoch) propsLoading.value = false; }
    }
    function setProperty(row) {
      // 拉出型（执行类）无选中值时发 1；其余用下拉/输入的数值
      const v = (row.edit !== undefined && row.edit !== '') ? Number(row.edit) : 1;
      if(!Number.isSafeInteger(v)) {ElMessage.warning('请输入精确整数（超出安全范围的属性暂不可写）');return;}
      const ip=selectedIp.value;row.busy=true; api('props.set', {ip,code:row.code,value:v}).then(r => {if(r.result?.ok) ElMessage.success('已提交并读取相机实际值');if(selectedIp.value===ip) loadProps();}).finally(()=>row.busy=false);
    }
    function renamePrompt() {
      const ip=selectedIp.value;
      ElMessageBox.prompt('输入新的相机名称', '重命名', { inputValue: current.value.name }).then(({ value }) => {
        if (value) api('cameras.rename', { ip, name: value });
      }).catch(() => {});
    }
    function removeCam() {
      const ip=selectedIp.value;
      ElMessageBox.confirm(`确定移除相机「${current.value.name}」？移除后软件不再自动连接它。`, '移除相机', { type: 'warning' })
        .then(async () => {const r=await api('cameras.remove',{ip});if(r.result?.ok && selectedIp.value===ip) {selectedIp.value='';props.value=[];propsEpoch++;}pollStatus();})
        .catch(() => {});
    }
    // 右上角「⋯」相机设置菜单
    function camMenu(cmd) {
      if (cmd === 'rename') renamePrompt();
      else if (cmd === 'remove') removeCam();
    }

    // ---------- 向导 ----------
    const tipsMap = {
      usb: 'USB 连接步骤：\n1. 用 USB-C 数据线连接相机与电脑\n2. 相机屏幕选择 USB 模式为「遥控拍摄 (PC Remote)」\n3. 首次使用需已安装索尼官方 USB 驱动\n4. 点击下一步，软件将自动搜索 USB 相机',
      wifi: 'Wi-Fi 连接步骤（按顺序操作）：\n1. 关闭相机「遥控拍摄功能」\n2. 相机接入与电脑相同的 Wi-Fi 网络\n3. 重新打开「遥控拍摄功能」，停在等待画面\n4. 下一步搜索相机；首次连接需在相机上确认配对\n5. 配对完成后相机断电重启一次，配对信息才会永久保存\n（如失败可将相机「访问验证」的用户名密码填入 SSH 栏）',
      lan: '有线 LAN 连接步骤：\n1. 用网线将相机接入路由器（与电脑同一局域网）\n2. 打开相机「遥控拍摄功能」，停在等待画面\n3. 下一步搜索相机；首次连接需在相机上确认配对\n4. 配对完成后相机断电重启一次，配对信息永久保存',
    };
    const wizardTips = computed(() => tipsMap[wizard.method]);
    const pairingTips = `现在进行配对，请严格按顺序操作：

① 在相机菜单进入：网络 → 连接/遥控拍摄 → 遥控拍摄功能 → 配对
② 相机停在「配对等待」画面
③ 点击下方「开始配对」按钮
④ 相机屏幕弹出配对请求时，在相机上点「确定」

软件会自动完成连接（如相机开启了访问验证，会弹出凭据填写框）。`;
    // 选中发现的相机：同步型号（连接/入库用正确 modelId，不再默认 FX3）
    function pickFound(row) {
      wizard.selected = row;
      if (row && row.model && modelNames.value.indexOf(row.model) >= 0) wizard.modelName = row.model;
    }
    function openWizard() {
      wizardEpoch++;wizardStep.value = 1; wizard.method = 'wifi'; wizard.ip = selectedIp.value || '';
      wizard.found = []; wizard.selected = null; wizard.searching = false;
      wizard.name = ''; wizard.pairing = 'idle'; wizard.sshUser = ''; wizard.sshPass = ''; wizardVisible.value = true;
    }
    function wizardNext() {
      if (wizardStep.value === 4 && wizard.pairing !== 'ok') return;
      wizardStep.value++;
      if (wizardStep.value === 3) {
        wizard.found = []; wizard.selected = null;
        if (wizard.method !== 'usb') searchCameras();   // 自动搜索，不阻塞翻页
      }
      if (wizardStep.value === 4) { wizard.pairing = 'idle'; wizard.pairingMsg = ''; }
      if (wizardStep.value === 5 && !wizard.name) {
        // 默认名称 = 配对时相机的型号（发现列表里的型号优先，NOTIFY 相机用下拉框选择的型号）
        wizard.name = (wizard.selected && wizard.selected.model && wizard.selected.model !== 'Sony 相机')
          ? wizard.selected.model : (wizard.modelName || 'Sony 相机');
      }
    }
    // 配对：向相机发起连接触发配对请求（此时不入库）
    async function doPairing(useCreds) {
      const ip = (wizard.selected && wizard.selected.ipAddress) || wizard.ip;
      if (!ip) { ElMessage.warning('未指定相机 IP'); return; }
      const epoch=wizardEpoch;wizard.pairing = 'busy';
      const modelId = modelNames.value.indexOf(wizard.modelName) >= 0 ? modelNames.value.indexOf(wizard.modelName) : 8;
      const r = await api('cameras.connectByIp', {
        ip, modelId,
        sshUser: useCreds ? wizard.sshUser : '', sshPass: useCreds ? wizard.sshPass : '',
      });
      if(epoch!==wizardEpoch || !wizardVisible.value) return;
      wizard.connecting = false;
      if (r.result && r.result.ok) {
        wizard.pairing = 'ok';
        ElMessage.success('配对成功');
      } else {
        const msg = (r.result && r.result.message) || '';
        wizard.pairing = 'fail';
        if (msg.includes('SSH') || msg.includes('0x8213') || msg.includes('0x8216')) {
          sshDialogVisible.value = true;
        } else if (msg.includes('0x8210')) {
          wizard.pairingMsg = '相机正被其他程序占用';
        } else {
          wizard.pairingMsg = msg || '配对未完成';
        }
      }
    }

    // 完成：入库 + 建立正式连接
    async function finishWizard() {
      const ip = (wizard.selected && wizard.selected.ipAddress) || wizard.ip;
      if (!ip || wizard.pairing !== 'ok') { ElMessage.error('请先完成配对'); return; }
      const modelId = modelNames.value.indexOf(wizard.modelName) >= 0 ? modelNames.value.indexOf(wizard.modelName) : 8;
      const name = wizard.name || wizard.modelName;
      wizard.connecting=true;
      const epoch=wizardEpoch;
      const r=await api('cameras.add',{ip,modelId,name,sshUser:wizard.sshUser,sshPass:wizard.sshPass});
      if(epoch!==wizardEpoch)return;
      wizard.connecting=false;
      if(!r.result?.ok)return;
      selectedIp.value=ip;api('cameras.setActive',{ip});
      wizardVisible.value=false;pollStatus();ElMessage.success(`相机「${name}」已加入相机库`);
    }

    async function searchCameras() {
      const epoch=wizardEpoch;wizard.searching = true; wizard.found = []; wizard.selected = null;
      const r = await api('cameras.discover', {
        timeoutSec: 6,
        modelId: modelNames.value.indexOf(wizard.modelName) >= 0 ? modelNames.value.indexOf(wizard.modelName) : 8,
      });
      if(epoch!==wizardEpoch || !wizardVisible.value)return;
      wizard.searching = false;
      if (r.result && r.result.cameras) wizard.found = r.result.cameras;
      if (!wizard.found.length) ElMessage.warning('未发现相机，请确认相机停在遥控拍摄等待画面后重试');
    }
    onMounted(async () => {
      const m = await api('models.list');
      if (m.result && m.result.models) modelNames.value = m.result.models;
      pollStatus();
      pollLogs();
      setInterval(pollStatus, 1000);
      setInterval(pollLogs, 1500);
    });

    watch(wizardVisible,visible=>{if(!visible)wizardEpoch++;});
    const quickProps=computed(()=>props.value.filter(p=>[0x100,0x103,0x104,0x108,0x115,0x182].includes(p.code)));
    const visibleLogs=computed(()=>logs.value.filter(l=>logLevel.value==='ALL'||l.level===logLevel.value));
    function exportLogs(){const a=document.createElement('a'); const url=URL.createObjectURL(new Blob([visibleLogs.value.map(l=>`${l.time} [${l.level}] ${l.text}`).join('\n')],{type:'text/plain;charset=utf-8'}));a.href=url;a.download='bridge-diagnostics.log';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
    return {
      busy, connectToggle, phaseName, qualityTarget, appVersion, propSearch, advanced, quickProps, logsOpen, logLevel, logPaused, visibleLogs, exportLogs,
      cameras, selectedIp, atem, atemIp, atemPort, sdkOk, ruleCount,
      connected, recording, logs, props, logBox, modelNames,
      current, dotClass, selectCam, record, idInput, stepId, commitId, atemToggle,
      loadProps, setProperty, renamePrompt, removeCam, camMenu,
      propsLoading, propGroup, propGroups, filteredProps, propName, fmtVal, fmtCode, isOp,
      page, openMonitor, openSettings,
      lvRes, lvComp, qMin, qMax, loadLvQuality, applyLvQuality,
      wizardVisible, wizardStep, wizard, wizardTips, pairingTips, openWizard, wizardNext,
      searchCameras, doPairing, finishWizard, sshDialogVisible, pickFound,
    };
  },
});
app.use(ElementPlus);
for (const [name, comp] of Object.entries(ElementPlusIconsVue || {})) app.component(name, comp);
app.mount('#app');
