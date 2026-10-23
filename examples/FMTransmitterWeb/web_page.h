// web_page.h - configuration page served by FMTransmitterWeb.
// Kept in a .h tab on purpose: the Arduino IDE preprocessor (ctags) misparses a long raw string
// literal full of JavaScript inside the .ino and breaks the build.
#pragma once
#include <Arduino.h>

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 FM</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1d2330;--mut:#667085;--acc:#2f6fed;--line:#e3e6eb;--bad:#c4321c;--ok:#1c8a4a}
@media (prefers-color-scheme:dark){:root{--bg:#12151b;--card:#1b2029;--fg:#e8ebf0;--mut:#98a2b3;--acc:#6e9bff;--line:#2a313d;--bad:#ff7a66;--ok:#4cc98a}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:760px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:4px 0 14px;display:flex;gap:10px;align-items:baseline}h1 small{color:var(--mut);font-weight:400;font-size:13px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin-bottom:14px}
h2{font-size:14px;text-transform:uppercase;letter-spacing:.05em;color:var(--mut);margin:0 0 10px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:10px 14px}
label{display:flex;flex-direction:column;gap:4px;font-size:13px;color:var(--mut)}
label.chk{flex-direction:row;align-items:center;gap:8px;color:var(--fg);font-size:15px}
input,select{font:inherit;color:var(--fg);background:transparent;border:1px solid var(--line);border-radius:7px;padding:7px 9px;min-width:0}
input[type=checkbox]{width:18px;height:18px}
button{font:inherit;border:0;border-radius:7px;padding:8px 14px;background:var(--acc);color:#fff;cursor:pointer}
button.ghost{background:transparent;color:var(--acc);border:1px solid var(--line)}
button.del{background:transparent;color:var(--bad);border:1px solid var(--line);padding:6px 10px}
.row{display:flex;gap:8px;align-items:center;margin-top:12px;flex-wrap:wrap}
.now{font-size:17px;font-weight:600;word-break:break-word}.mut{color:var(--mut);font-size:13px}
.st{display:grid;grid-template-columns:auto 1fr 2fr auto;gap:8px;align-items:center;margin-bottom:8px}
@media (max-width:560px){.st{grid-template-columns:auto 1fr auto}.st>input[type=radio]{grid-row:1;grid-column:1}.st .n{grid-row:1;grid-column:2}.st .del{grid-row:1;grid-column:3}.st .u{grid-row:2;grid-column:2/4}}
.msg{font-size:13px}.msg.bad{color:var(--bad)}.msg.ok{color:var(--ok)}
details summary{cursor:pointer;color:var(--mut);font-size:13px;margin-top:6px}
.air{display:flex;gap:14px;align-items:center;justify-content:space-between}
#air{min-width:118px;padding:12px 16px;font-weight:600}
#air.off{background:var(--ok)}#air.on{background:var(--bad)}
input[type=file]{border:0;padding:0;max-width:100%}
</style></head><body><main>
<h1>ESP32 FM <small id="ip"></small></h1>

<section><h2>On air</h2>
<div class="air"><div><div class="now" id="title">-</div>
<div class="mut" id="sub"></div>
<div class="mut" id="stats"></div></div>
<button id="air" onclick="toggleAir()">&hellip;</button></div>
</section>

<section><h2>Stations</h2>
<div id="stations"></div>
<div class="row"><button class="ghost" onclick="addSt()">Add station</button><button onclick="saveSt()">Save stations</button><span class="msg" id="msgSt"></span></div>
</section>

<section><h2>Transmitter</h2>
<div class="grid">
<label>Frequency (MHz)<input id="freq" type="number" step="0.05" min="88" max="108"></label>
<label>Station name (PS, 8 chars)<input id="ps" maxlength="8"></label>
<label>Pre-emphasis<select id="preemph"><option value="50">50 &micro;s (Europe)</option><option value="75">75 &micro;s (Americas)</option><option value="0">Off</option></select></label>
<label>Programme type (PTY)<input id="pty" type="number" min="0" max="31"></label>
<label>Gain<input id="gain" type="number" step="0.05" min="0" max="4"></label>
</div>
<div class="row">
<label class="chk"><input type="checkbox" id="stereo">Stereo</label>
<label class="chk"><input type="checkbox" id="rds">RDS</label>
<label class="chk"><input type="checkbox" id="agc">AGC</label>
</div>
<details><summary>Advanced</summary><div class="grid" style="margin-top:10px">
<label>L-R gain (k)<input id="k" type="number" step="0.01"></label>
<label>38 kHz phase trim (&deg;)<input id="h" type="number" step="1"></label>
<label>RDS charset<select id="charset"><option value="0">RDS table</option><option value="1">ASCII, no accents</option><option value="2">Latin-1</option></select></label>
</div>
<label class="chk" style="margin-top:10px"><input type="checkbox" id="rtab">Toggle RadioText A/B flag on each new title (leave off if your receiver shows two titles)</label>
</details>
<div class="row"><button onclick="saveRadio()">Apply and save</button><span class="msg" id="msgRadio"></span></div>
<div class="mut" style="margin-top:6px">Switching mono/stereo restarts the ESP32. Every other setting applies live.</div>
</section>

<section><h2>System</h2>
<div class="mut" id="sys"></div>
<div class="row"><input type="file" id="fw" accept=".bin"><button class="ghost" onclick="uploadFw()">Update firmware</button><span class="msg" id="msgFw"></span></div>
<div class="mut" style="margin-top:6px">Upload the .bin from Sketch &rarr; Export Compiled Binary. The transmitter goes off air during the update, then restarts.</div>
</section>

<section><h2>Wi-Fi</h2>
<div class="grid"><label>SSID<input id="ssid"></label><label>Password<input id="pass" type="password" placeholder="unchanged"></label></div>
<div class="row"><button onclick="saveWifi()">Save and restart</button><span class="msg" id="msgWifi"></span></div>
</section>
</main>
<script>
let cfg=null, edited=false;
const $=id=>document.getElementById(id);
function esc(s){return s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function msg(id,t,ok){const e=$(id);e.textContent=t;e.className='msg '+(ok?'ok':'bad');if(ok)setTimeout(()=>e.textContent='',3000)}
async function post(u,data){const r=await fetch(u,{method:'POST',body:new URLSearchParams(data)});if(!r.ok)throw new Error(await r.text());return r.json()}
function renderStations(){
  $('stations').innerHTML=cfg.stations.map((s,i)=>`<div class="st">
  <input type="radio" name="sel" ${i==cfg.sel?'checked':''} onclick="selSt(${i})" title="Put on air">
  <input class="n" value="${esc(s.name)}" placeholder="Name" oninput="edited=true">
  <input class="u" value="${esc(s.url)}" placeholder="http://..." oninput="edited=true">
  <button class="del" onclick="delSt(${i})">&times;</button></div>`).join('')||'<div class="mut">No station yet.</div>';
}
function readStations(){return [...document.querySelectorAll('.st')].map(r=>({name:r.querySelector('.n').value,url:r.querySelector('.u').value}))}
function addSt(){cfg.stations=readStations();if(cfg.stations.length>=8)return;cfg.stations.push({name:'',url:''});edited=true;renderStations()}
function delSt(i){cfg.stations=readStations();cfg.stations.splice(i,1);if(cfg.sel>=cfg.stations.length)cfg.sel=0;edited=true;renderStations()}
async function saveSt(){const l=readStations().filter(s=>s.url.trim());const d={count:l.length};l.forEach((s,i)=>{d['n'+i]=s.name;d['u'+i]=s.url});
  try{await post('/api/stations',d);edited=false;msg('msgSt','Saved',1);load()}catch(e){msg('msgSt',e.message)}}
async function selSt(i){try{await post('/api/select',{i});cfg.sel=i;load()}catch(e){msg('msgSt',e.message)}}
async function saveRadio(){const d={freq:$('freq').value,ps:$('ps').value,preemph:$('preemph').value,pty:$('pty').value,gain:$('gain').value,
  k:$('k').value,h:$('h').value,charset:$('charset').value,stereo:$('stereo').checked?1:0,rds:$('rds').checked?1:0,agc:$('agc').checked?1:0,rtab:$('rtab').checked?1:0};
  try{const r=await post('/api/radio',d);msg('msgRadio',r.restart?'Saved, restarting...':'Applied and saved',1);if(r.restart)setTimeout(load,6000)}catch(e){msg('msgRadio',e.message)}}
let airState=null;
async function toggleAir(){if(airState===null)return;try{await post('/api/onair',{on:airState?0:1});airState=!airState;paintAir(airState);setTimeout(load,800)}catch(e){}}
function paintAir(on){const b=$('air');b.textContent=on?'Go off air':'Go on air';b.className=on?'on':'off'}
function dur(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+' d ':'')+h+' h '+m+' min'}
async function uploadFw(){const f=$('fw').files[0];if(!f)return msg('msgFw','Choose a .bin file first');
  const fd=new FormData();fd.append('firmware',f,f.name);msg('msgFw','Uploading '+Math.round(f.size/1024)+' kB...',1);
  try{const r=await fetch('/update',{method:'POST',body:fd});if(!r.ok)throw new Error(await r.text());msg('msgFw','Updated, restarting...',1);setTimeout(load,8000)}catch(e){msg('msgFw',e.message)}}
async function saveWifi(){try{await post('/api/wifi',{ssid:$('ssid').value,pass:$('pass').value});msg('msgWifi','Saved, restarting...',1)}catch(e){msg('msgWifi',e.message)}}
function fill(c){for(const k of ['freq','ps','preemph','pty','gain','k','h','charset','ssid'])$(k).value=c[k];for(const k of ['stereo','rds','agc','rtab'])$(k).checked=c[k]}
function status(c){const s=c.status,st=c.stations[c.sel];
  airState=s.want;paintAir(s.want);
  $('sys').textContent='firmware '+s.fw+' \u00b7 up '+dur(s.uptime)+' \u00b7 last reset: '+s.reset+' \u00b7 lowest free heap '+Math.round(s.minheap/1024)+' kB (largest block now '+Math.round(s.largest/1024)+' kB, lowest '+Math.round(s.minlargest/1024)+' kB)'+' \u00b7 decoders: MP3'+(s.aac?' + AAC':' only');
  if(!s.onair){$('title').textContent='Off air';$('sub').textContent=(st?st.name+' \u00b7 ':'')+c.freq.toFixed(2)+' MHz';$('stats').textContent=(s.ap?'':'RSSI '+s.rssi+' dBm (AP '+s.bssid+', ch '+s.ch+') \u00b7 ')+'heap '+Math.round(s.heap/1024)+' kB';$('ip').textContent=s.ap?'setup access point '+s.ip:s.ip;return}
  $('ip').textContent=s.ap?'setup access point '+s.ip:s.ip;
  $('title').textContent=s.title||(st?st.name:'-');
  $('sub').textContent=(st?st.name+' · ':'')+s.actual.toFixed(2)+' MHz · '+(c.stereo?'stereo':'mono')+(c.stereo&&c.rds?' + RDS':'')+(s.playing?' · '+s.codec:' · not playing');
  $('stats').textContent='buffer '+s.fill+'/'+s.size+' · underruns '+s.underruns+' · rate '+s.rate+' Hz · peak '+s.peak+(s.ap?'':' · RSSI '+s.rssi+' dBm (AP '+s.bssid+', ch '+s.ch+')')+' · heap '+Math.round(s.heap/1024)+' kB'}
async function load(){try{const c=await (await fetch('/api/state')).json();
  if(!cfg){fill(c)} if(!edited){cfg=c;renderStations()} else {cfg.sel=c.sel}
  status(c)}catch(e){}}
load();setInterval(load,2000);
</script></body></html>)HTML";
