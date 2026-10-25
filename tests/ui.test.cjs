// Isolated browser fixtures only: never installed in the application UI.
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const { pathToFileURL } = require('node:url');
const path = require('node:path');
const fs = require('node:fs');
const assert = require('node:assert/strict');
const output=path.resolve(__dirname,'../build-maintenance/ui-test-results');
fs.mkdirSync(output,{recursive:true});

async function fixture(page) {
  await page.addInitScript(() => {
    const listeners=[];
    const cameras=[{ip:'192.0.2.1',name:'Test A',cameraId:1,modelId:8,model:'ILME-FX3',connected:true,recording:false,recordingKnown:true,phase:'online'},
      {ip:'192.0.2.2',name:'Test B',cameraId:2,modelId:7,model:'ILCE-7M4',connected:true,recording:false,recordingKnown:true,phase:'online'}];
    window.__requests=[];
    window.__deliver=d=>listeners.forEach(fn=>fn({data:d}));
    window.chrome={webview:{addEventListener:(name,fn)=>listeners.push(fn),postMessage:raw=>{
      const q=JSON.parse(raw);window.__requests.push(q);
      let result={ok:true,code:'OK',message:'Complete'},delay=0;
      if(q.method==='models.list') result.models=['ILME-FX3','ILCE-7M4'];
      if(q.method==='status') Object.assign(result,{boundCameras:cameras,app:{version:'0.2.1-dev',sdkInitialized:true},atem:{connected:false},ruleCount:2});
      if(q.method==='logs') result.entries=[{time:'12:00:00',level:'INFO',text:'Fixture log'}];
      if(q.method==='props.list') {
        result.properties=[{code:0x100,value:q.params.ip==='192.0.2.1'?280:400,possible:[280,400],writable:true,readable:true}];
        delay=q.params.ip==='192.0.2.1'?250:10;
      }
      if(q.method==='cameras.connectByIp') result={ok:false,code:'CAMERA_CONNECT_FAILED',message:'Fixture rejected pairing'};
      if(q.method==='cameras.discover') result.cameras=[];
      if(q.method==='liveview.quality') Object.assign(result,{imageQuality:1,compression:-1});
      setTimeout(()=>window.__deliver({id:q.id,result}),delay);
    }}};
  });
}

(async()=>{
  const browser=await chromium.launch({headless:true,executablePath:process.env.BROWSER_EXE || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'});
  const report={layouts:[],errors:[],checks:[]};
  try {
    // Desktop resolution / Windows scaling matrix, expressed as CSS viewport.
    for(const [width,height] of [[1024,768],[1280,720],[1920,1080]]) for(const scale of [1,1.25,1.5]) {
      const context=await browser.newContext({viewport:{width:Math.floor(width/scale),height:Math.floor(height/scale)},deviceScaleFactor:scale});
      const page=await context.newPage();page.on('pageerror',e=>report.errors.push(e.message));
      await fixture(page);
      await page.goto(pathToFileURL(path.resolve(__dirname,'../ui/index.html')).href);
      await page.getByText('Test A',{exact:true}).first().waitFor();
      await page.getByRole('button',{name:'高级属性',exact:true}).click();
      await page.getByRole('button',{name:'展开日志',exact:true}).scrollIntoViewIfNeeded();
      await page.getByRole('button',{name:'⚙ 软件设置',exact:true}).scrollIntoViewIfNeeded();
      const overflow=await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth);
      assert.equal(overflow,false,`horizontal overflow ${width} ${height} @${scale}`);
      await page.getByRole('button',{name:'⚙ 软件设置',exact:true}).click();
      await page.getByText('默认画质',{exact:true}).click();
      assert.equal(await page.getByText('软件设置',{exact:true}).count(),1);
      await page.getByRole('button',{name:'⚙ 软件设置',exact:true}).click();
      await page.getByRole('button',{name:'＋ 添加相机',exact:true}).click();
      await page.getByRole('button',{name:'下一步',exact:true}).scrollIntoViewIfNeeded();
      const box=await page.getByRole('button',{name:'下一步',exact:true}).boundingBox();
      assert(box && box.x>=0 && box.x+box.width<=Math.floor(width/scale)+1);
      report.layouts.push({width,height,scale,passed:true});
      if(scale===1) await page.screenshot({path:path.join(output,`main-${width}.png`)});
      await context.close();
    }
    const context=await browser.newContext({viewport:{width:1280,height:720}});
    const page=await context.newPage();page.on('pageerror',e=>report.errors.push(e.message));await fixture(page);
    await page.goto(pathToFileURL(path.resolve(__dirname,'../ui/index.html')).href);
    await page.getByText('Test A',{exact:true}).first().waitFor();
    await page.getByText('Test B',{exact:true}).first().click();
    await page.waitForTimeout(600);
    const values=await page.locator('.el-table__body').innerText();
    assert(!values.includes('F2.8'),'stale A property overwrote B');
    const props=await page.evaluate(()=>window.__requests.filter(q=>q.method==='props.list').map(q=>q.params.ip));
    assert(props.includes('192.0.2.1') && props.includes('192.0.2.2'));
    report.checks.push('late property reply discarded after camera switch');
    await page.getByRole('button',{name:'＋ 添加相机',exact:true}).click();
    await page.getByRole('button',{name:'下一步',exact:true}).click();
    await page.getByRole('button',{name:'下一步',exact:true}).click();
    await page.getByPlaceholder('填写相机屏幕显示的 IP 地址').fill('192.0.2.20');
    await page.getByRole('button',{name:'下一步',exact:true}).click();
    assert(await page.getByRole('button',{name:'下一步',exact:true}).isDisabled());
    await page.getByRole('button',{name:'🔗 开始配对',exact:true}).click();
    await page.getByText('Fixture rejected pairing（相机未响应时请确认已进入配对页面后重试）',{exact:true}).waitFor();
    assert(await page.getByRole('button',{name:'下一步',exact:true}).isDisabled());
    report.checks.push('failed pairing cannot reach successful completion');
    await page.goto(pathToFileURL(path.resolve(__dirname,'../ui/monitor.html')).href);
    await page.getByText('Test A',{exact:true}).waitFor();
    await page.waitForTimeout(500);
    assert((await page.getByText('等待首帧',{exact:true}).count())>=1);
    await page.getByRole('button',{name:'■ 停止监看',exact:true}).click();
    assert((await page.getByText('监看已停止',{exact:true}).count())>=1);
    assert.equal(await page.locator('canvas.on').count(),0);
    await page.getByText('4×4',{exact:true}).click();assert.equal(await page.locator('.grid.g4').count(),1);
    report.checks.push('monitor waiting/stopped states and 4x4 layout');
    await page.screenshot({path:path.join(output,'monitor.png')});
    await context.close();
    assert.deepEqual(report.errors,[]);
    console.log(JSON.stringify(report,null,2));
  } finally {
    fs.writeFileSync(path.join(output,'report.json'),JSON.stringify(report,null,2));
    await browser.close();
  }
})().catch(e=>{console.error(e);process.exitCode=1;});
