const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');
(async () => {
  const [,, json, outdir, scale] = process.argv;
  fs.mkdirSync(outdir, {recursive:true});
  const data = JSON.parse(fs.readFileSync(json,'utf8'));
  const b = await chromium.launch();
  const p = await b.newPage({viewport:{width:data.w*(+scale||1), height:data.h*(+scale||1)}});
  await p.setContent(`<html><body style="margin:0;background:transparent"><div id=a style="width:${data.w*(+scale||1)}px;height:${data.h*(+scale||1)}px"></div></body></html>`);
  await p.addScriptTag({path: path.join(__dirname,'node_modules/lottie-web/build/player/lottie.min.js')});
  const info = await p.evaluate((d)=>{ window.anim = lottie.loadAnimation({container:document.getElementById('a'),renderer:'svg',loop:false,autoplay:false,animationData:d}); return {tf: window.anim.totalFrames, ip: d.ip, op: d.op}; }, data);
  console.log(info);
  const el = await p.$('#a');
  for (let f=0; f<info.tf; f++) {
    await p.evaluate((f)=>window.anim.goToAndStop(f,true), f);
    await el.screenshot({path: path.join(outdir, String(f).padStart(3,'0')+'.png'), omitBackground:true});
  }
  await b.close();
})();
