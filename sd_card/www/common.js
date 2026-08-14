'use strict';
let TOKEN = localStorage.getItem('cp_token') || '';
let ROLE  = localStorage.getItem('cp_role')  || '';
let USER  = localStorage.getItem('cp_user')  || '';

const AVC=['#B5D4F4','#9FE1CB','#F5C4B3','#F4C0D1','#C0DD97','#FAD6A5'];
const AXT=['#0C447C','#085041','#8B3A1A','#6B3660','#3A6B11','#7A4E00'];
function avC(n){return AVC[(n||'?').charCodeAt(0)%6];}
function avX(n){return AXT[(n||'?').charCodeAt(0)%6];}
function ini(n){return(n||'?').split(' ').map(w=>w[0]||'').join('').slice(0,2).toUpperCase();}
function av(n){return`<div class="av" style="background:${avC(n)};color:${avX(n)}">${ini(n)}</div>`;}

function toast(msg,dur=2500){
  const t=document.getElementById('toast');t.textContent=msg;t.classList.add('show');
  setTimeout(()=>t.classList.remove('show'),dur);
}
function fmtTs(ts){
  if(!ts)return'—';const s=String(ts);
  if(s.includes('T')&&s.includes(':')){
    try{const d=new Date(ts);if(!isNaN(d)){
      const today=new Date();
      if(d.toDateString()===today.toDateString())
        return d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',second:'2-digit'});
      return d.toLocaleDateString([],{day:'2-digit',month:'2-digit'})+' '+d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});
    }}catch{}
  }
  if(s.startsWith('~'))return'boot+'+s.slice(1);return s;
}
function fmtTsLocal(ts){
  if(!ts)return'—';const s=String(ts);
  if(s.startsWith('~'))return'boot+'+s.slice(1);
  if(s.includes('T')&&s.includes(':')){
    try{
      const tz=localStorage.getItem('cp_tz')||'Asia/Tbilisi';
      const d=new Date(ts);
      if(!isNaN(d)){
        const today=new Date();
        const opts={timeZone:tz,hour:'2-digit',minute:'2-digit',hour12:false};
        const dateOpts={timeZone:tz,day:'2-digit',month:'2-digit'};
        const todayStr=today.toLocaleDateString('en-GB',{timeZone:tz});
        const dStr=d.toLocaleDateString('en-GB',{timeZone:tz});
        if(todayStr===dStr)return d.toLocaleTimeString('en-GB',opts);
        return d.toLocaleDateString('en-GB',dateOpts)+' '+d.toLocaleTimeString('en-GB',opts);
      }
    }catch{}
  }
  return ts;
}
function fmtUptime(s){
  if(!s)return'—';if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m '+Math.floor(s%60)+'s';
  return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';
}
function showErr(id,msg){const e=document.getElementById(id);if(e){e.textContent=msg;e.classList.add('show');}}
function hideErr(id){const e=document.getElementById(id);if(e)e.classList.remove('show');}

function headers(){return{'Content-Type':'application/json','X-Session-Token':TOKEN};}
async function api(method,path,body,_retried){
  try{
    const r=await fetch(path,{method,headers:headers(),body:body?JSON.stringify(body):undefined});
    const d=await r.json();
    if(r.status===401){if(TOKEN)doLogout();return null;}
    return{ok:r.ok,status:r.status,data:d};
  }catch(e){
    if(method==='GET'&&!_retried){
      await new Promise(res=>setTimeout(res,300));
      return api(method,path,body,true);
    }
    return{ok:false,status:0,data:{error:e.message}};
  }
}
async function lg(p){const r=await api('GET',p);return r?.data||null;}
async function lpost(p,b){const r=await api('POST',p,b);return r?.data||null;}

function togglePwd(id,btn){
  const inp=document.getElementById(id);
  if(inp.type==='password'){inp.type='text';btn.textContent='🙈';}
  else{inp.type='password';btn.textContent='👁';}
}

function doLogout(){
  fetch('/api/auth/logout',{method:'POST',headers:headers()}).catch(()=>{});
  TOKEN='';ROLE='';USER='';
  localStorage.removeItem('cp_token');localStorage.removeItem('cp_role');localStorage.removeItem('cp_user');
  window.location.replace('/');
}

function closeModal(id){document.getElementById(id).classList.remove('show');}
function openSidebar(){
  document.getElementById('sidebar').classList.add('open');
  document.getElementById('overlay').classList.add('show');
  document.body.style.overflow='hidden';
}
function closeSidebar(){
  document.getElementById('sidebar').classList.remove('open');
  document.getElementById('overlay').classList.remove('show');
  document.body.style.overflow='';
}

function openChangePassword(){
  document.getElementById('chp-old').value='';
  document.getElementById('chp-new').value='';
  document.getElementById('chp-conf').value='';
  hideErr('chpass-err');
  document.getElementById('modal-chpass').classList.add('show');
}
async function submitChangePassword(){
  const old=document.getElementById('chp-old').value;
  const nw=document.getElementById('chp-new').value;
  const conf=document.getElementById('chp-conf').value;
  hideErr('chpass-err');
  if(nw!==conf){showErr('chpass-err','Passwords do not match');return;}
  if(nw.length<8){showErr('chpass-err','Min 8 chars');return;}
  const r=await lpost('/api/auth/change-password',{old_password:old,new_password:nw});
  if(r?.ok){toast('Password changed ✓');closeModal('modal-chpass');}
  else showErr('chpass-err',r?.error||'Failed');
}

function buildNav(activePage){
  const sup=ROLE==='super_admin';
  const ni=(pg,href,label,icon,extra='')=>
    `<a class="ni${activePage===pg?' active':''}" href="${href}">${icon}${label}${extra}</a>`;
  const ic=(d)=>`<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">${d}</svg>`;
  document.getElementById('main-nav').innerHTML=
    ni('dashboard','/','Dashboard',ic('<rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/>'))+
    ni('events','/events.html','Events',ic('<polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/>'),
      '<span class="ni-badge" id="nb" style="display:none">0</span>')+
    ni('employees','/employees.html','Employees',ic('<path d="M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2"/><circle cx="9" cy="7" r="4"/><path d="M23 21v-2a4 4 0 0 0-3-3.87M16 3.13a4 4 0 0 1 0 7.75"/>'))+
    ni('cards','/employees.html#cards','Cards',ic('<rect x="1" y="4" width="22" height="16" rx="2"/><line x1="1" y1="10" x2="23" y2="10"/>'))+
    ni('photos','/photos.html','Photos',ic('<rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="8.5" cy="8.5" r="1.5"/><polyline points="21 15 16 10 5 21"/>'))+
    (sup?
      '<div class="nav-sep">Admin</div>'+
      ni('admins','/admins.html','Admins',ic('<circle cx="12" cy="8" r="4"/><path d="M6 20v-2a6 6 0 0 1 12 0v2"/><path d="M18 8h2M22 12l-2-2 2-2"/>'))+
      ni('setup','/setup.html','Device setup',ic('<circle cx="12" cy="12" r="3"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14M4.93 4.93a10 10 0 0 0 0 14.14"/>'))+
      ni('devices','/devices.html','Device info',ic('<rect x="4" y="4" width="16" height="16" rx="2"/><rect x="9" y="9" width="6" height="6"/>'))+
      ni('devlog','/devlog.html','Device log',ic('<polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/>'))
    :'');
}

async function syncServer(){
  try{
    const r=await fetch('/api/proxy/device/sync',{headers:{'X-Session-Token':TOKEN}});
    const online=r.status!==503&&r.status!==0;
    const chip=document.getElementById('chip');
    const bar=document.getElementById('offline-bar');
    const dot=document.getElementById('sdot');
    const stxt=document.getElementById('sync-txt');
    if(online){
      if(chip){chip.style.cssText='background:var(--green-bg);color:var(--green-tx)';chip.textContent='● Online';}
      if(bar)bar.classList.remove('show');
      if(dot)dot.classList.remove('off');
      if(stxt)stxt.textContent='Synced '+new Date().toLocaleTimeString();
    }else{
      if(chip){chip.style.cssText='background:var(--amber-bg);color:var(--amber-tx)';chip.textContent='● Offline';}
      if(bar)bar.classList.add('show');
      if(dot)dot.classList.add('off');
      if(stxt)stxt.textContent='Server unreachable';
    }
  }catch{}
}

function initShell(activePage){
  const uav=document.getElementById('user-av');
  const unm=document.getElementById('user-name');
  const urol=document.getElementById('user-role');
  if(uav)uav.textContent=ini(USER);
  if(unm)unm.textContent=USER;
  if(urol)urol.textContent=ROLE==='super_admin'?'Super Admin':'Admin';
  buildNav(activePage);
  syncServer();
  setInterval(syncServer,60000);
  setInterval(()=>lg('/api/local/status').then(s=>{
    if(!s)return;
    const sb=document.getElementById('sb-id');
    if(sb)sb.textContent=s.identifier||'Device';
  }),20000);
  // inject change-password modal for sidebar button
  document.body.insertAdjacentHTML('beforeend',
    `<div class="modal-bg" id="modal-chpass"><div class="modal">
    <h3>Change password</h3><div class="modal-err" id="chpass-err"></div>
    <div class="field"><label>Current password</label><input type="password" id="chp-old"></div>
    <div class="field"><label>New password</label><input type="password" id="chp-new"></div>
    <div class="field"><label>Confirm</label><input type="password" id="chp-conf"></div>
    <div class="modal-footer">
      <button class="btn" onclick="closeModal('modal-chpass')">Cancel</button>
      <button class="btn btn-primary" onclick="submitChangePassword()">Change password</button>
    </div></div></div>`);
}
