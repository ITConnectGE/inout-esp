#pragma once
// Auto-generated from data/index.html
const char PAGE_HTML[] PROGMEM = R"CP_SETUP_HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"><title>Checkpoint Setup</title><style>
:root{
  --bg:#f5f5f3;--surface:#fff;--s2:#f0f0ee;
  --border:#e2e1da;--border2:#c4c3bc;
  --text:#1a1a18;--muted:#6b6b67;--hint:#9b9b96;
  --blue:#178DFF;--blue-bg:#e8f3ff;--blue-tx:#0c4a8a;
  --green:#39955a;--green-bg:#eaf5ee;--green-tx:#1a5430;
  --amber:#b07a10;--amber-bg:#fef3dc;--amber-tx:#7a4e00;
  --red:#c0392b;--red-bg:#fdecea;--red-tx:#7a1e14;
  --r:10px;--rsm:7px;
}
@media(prefers-color-scheme:dark){:root{
  --bg:#161614;--surface:#1f1f1d;--s2:#272725;
  --border:#2e2e2b;--border2:#3e3e3a;
  --text:#f0f0ec;--muted:#a0a09a;--hint:#606060;
  --blue:#4aabff;--blue-bg:#0d2540;--blue-tx:#90ccff;
  --green:#4caf70;--green-bg:#0f2d1a;--green-tx:#7dd89a;
  --amber:#e0a030;--amber-bg:#2d1e00;--amber-tx:#f0c060;
  --red:#e05540;--red-bg:#2d0e0a;--red-tx:#f09080;
}}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
     background:var(--bg);color:var(--text);min-height:100vh;
     -webkit-font-smoothing:antialiased}
.shell{max-width:480px;margin:0 auto;padding:16px 16px 60px}
header{display:flex;align-items:center;gap:12px;padding:20px 0 22px}
.logo{width:38px;height:38px;background:var(--blue);border-radius:9px;
      display:flex;align-items:center;justify-content:center;flex-shrink:0}
.logo svg{width:20px;height:20px;fill:none;stroke:#fff;stroke-width:2.2;
          stroke-linecap:round;stroke-linejoin:round}
.htext h1{font-size:16px;font-weight:600}
.htext p{font-size:12px;color:var(--muted);margin-top:2px}

/* Status bar */
.status-bar{display:flex;align-items:center;gap:8px;background:var(--surface);
            border:1px solid var(--border);border-radius:var(--r);
            padding:10px 13px;margin-bottom:16px;font-size:13px}
.sdot{width:8px;height:8px;border-radius:50%;flex-shrink:0}
.sdot.ok{background:var(--green);box-shadow:0 0 0 3px var(--green-bg)}
.sdot.warn{background:var(--amber);box-shadow:0 0 0 3px var(--amber-bg)}
.sdot.err{background:var(--red);box-shadow:0 0 0 3px var(--red-bg)}
.sdot.spin{background:var(--blue);animation:pulse 1.2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}
.stxt{flex:1;font-weight:500}
.ssub{font-size:11px;color:var(--muted)}
.sig{display:flex;align-items:flex-end;gap:2px;height:14px}
.sig-b{width:4px;border-radius:1px 1px 0 0;background:var(--border2)}
.sig-b.on{background:var(--green)}

/* Metrics */
.metrics{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:16px}
.metric{background:var(--surface);border:1px solid var(--border);
        border-radius:var(--rsm);padding:11px 13px}
.mlbl{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.05em;margin-bottom:3px}
.mval{font-size:20px;font-weight:600;letter-spacing:-.02em}
.mval.mono{font-family:ui-monospace,monospace;font-size:13px;font-weight:500;word-break:break-all}
.msub{font-size:10px;color:var(--hint);margin-top:2px}
.chip{display:inline-block;padding:2px 7px;border-radius:99px;font-size:11px;font-weight:600}
.chip-g{background:var(--green-bg);color:var(--green-tx)}
.chip-r{background:var(--red-bg);color:var(--red-tx)}
.chip-b{background:var(--blue-bg);color:var(--blue-tx)}
.chip-a{background:var(--amber-bg);color:var(--amber-tx)}

/* Cards */
.card{background:var(--surface);border:1px solid var(--border);
      border-radius:var(--r);overflow:hidden;margin-bottom:12px}
.card-hd{padding:12px 15px 10px;display:flex;align-items:center;justify-content:space-between}
.card-hd h2{font-size:13px;font-weight:600}
.card-hd .hint{font-size:11px;color:var(--muted)}
.divider{height:1px;background:var(--border)}
.info-row{display:flex;justify-content:space-between;align-items:center;
          padding:8px 15px;font-size:12px;border-bottom:1px solid var(--border)}
.info-row:last-child{border-bottom:none}
.ir-label{color:var(--muted)}
.ir-value{font-weight:500;font-family:ui-monospace,monospace;font-size:11px;
          text-align:right;max-width:60%;word-break:break-all}

/* Form */
.field{padding:11px 15px;display:flex;flex-direction:column;gap:5px;
       border-bottom:1px solid var(--border)}
.field:last-child{border-bottom:none}
.field-row{flex-direction:row;align-items:center;justify-content:space-between;gap:12px}
label{font-size:13px;font-weight:500}
.lhint{font-size:11px;color:var(--muted);font-weight:400}
input[type=text],input[type=url],input[type=password],input[type=number]{
  width:100%;padding:8px 10px;border:1px solid var(--border2);
  border-radius:var(--rsm);background:var(--bg);color:var(--text);
  font-size:13px;font-family:inherit;outline:none;transition:border-color .15s,box-shadow .15s}
input:focus{border-color:var(--blue);box-shadow:0 0 0 3px var(--blue-bg)}
input[type=number]{max-width:90px}
.mono-in{font-family:ui-monospace,monospace;font-size:12px}
select{padding:8px 10px;border:1px solid var(--border2);border-radius:var(--rsm);
       background:var(--bg);color:var(--text);font-size:13px;font-family:inherit;outline:none;width:100%}
select:focus{border-color:var(--blue)}
.toggle{position:relative;width:42px;height:24px;flex-shrink:0}
.toggle input{opacity:0;width:0;height:0}
.ttrack{position:absolute;inset:0;background:var(--border2);border-radius:99px;
        cursor:pointer;transition:background .2s}
.toggle input:checked+.ttrack{background:var(--blue)}
.ttrack:before{content:'';position:absolute;height:18px;width:18px;left:3px;top:3px;
               background:#fff;border-radius:50%;transition:transform .2s;box-shadow:0 1px 3px rgba(0,0,0,.2)}
.toggle input:checked+.ttrack:before{transform:translateX(18px)}

/* Buttons */
button{font-family:inherit;cursor:pointer;border:none;outline:none}
.btn{display:flex;align-items:center;justify-content:center;gap:6px;
     padding:10px 16px;border-radius:var(--rsm);font-size:14px;font-weight:500;
     transition:background .15s,opacity .15s}
.btn-primary{background:var(--blue);color:#fff;width:100%}
.btn-primary:active{opacity:.85}
.btn-primary:disabled{opacity:.4;cursor:not-allowed}
.btn-secondary{background:var(--s2);color:var(--text);border:1px solid var(--border2);flex:1}
.btn-secondary:active{background:var(--border)}
.btn-danger{background:var(--red-bg);color:var(--red-tx);border:1px solid var(--border);flex:1}
.btn-row{padding:11px 15px;display:flex;gap:8px}
.btn-loading::after{content:'';width:12px;height:12px;border:2px solid rgba(255,255,255,.4);
                    border-top-color:#fff;border-radius:50%;animation:spin .7s linear infinite;display:inline-block}
@keyframes spin{to{transform:rotate(360deg)}}

/* Toast */
.toast{position:fixed;bottom:22px;left:50%;transform:translateX(-50%) translateY(70px);
       background:var(--text);color:var(--bg);padding:9px 20px;border-radius:99px;
       font-size:13px;font-weight:500;transition:transform .22s,opacity .22s;
       opacity:0;z-index:99;pointer-events:none;white-space:nowrap}
.toast.show{transform:translateX(-50%) translateY(0);opacity:1}

/* Section header */
.section{font-size:11px;font-weight:600;color:var(--muted);text-transform:uppercase;
         letter-spacing:.06em;margin:20px 0 8px 2px}
</style></head><body><div class="shell"><header><div class="logo"><svg viewBox="0 0 24 24"><path d="M12 2L4 6v6c0 5.25 3.5 10.15 8 11.35C16.5 22.15 20 17.25 20 12V6l-8-4z"/></svg></div><div class="htext"><h1>Checkpoint</h1><p id="h-id">Loading device…</p></div></header><div class="status-bar"><div class="sdot spin" id="sdot"></div><div style="flex:1"><div class="stxt" id="stxt">Connecting…</div><div class="ssub" id="ssub"></div></div><div class="sig" id="sig"><div class="sig-b" style="height:4px"></div><div class="sig-b" style="height:7px"></div><div class="sig-b" style="height:10px"></div><div class="sig-b" style="height:14px"></div></div></div><div class="metrics"><div class="metric"><div class="mlbl">Uptime</div><div class="mval" id="m-up">—</div></div><div class="metric"><div class="mlbl">Unsynced</div><div class="mval" id="m-sync">—</div><div class="msub" id="m-sync-sub"></div></div><div class="metric"><div class="mlbl">IP address</div><div class="mval mono" id="m-ip">—</div></div><div class="metric"><div class="mlbl">SD card</div><div class="mval" id="m-sd">—</div><div class="msub" id="m-sd-sub"></div></div></div><div class="section">Device</div><div class="card"><div class="card-hd"><h2>Status</h2><span id="fw-chip"></span></div><div class="divider"></div><div class="info-row"><span class="ir-label">Identifier</span><span class="ir-value" id="d-id">—</span></div><div class="info-row"><span class="ir-label">Server</span><span class="ir-value" id="d-srv">—</span></div><div class="info-row"><span class="ir-label">Token</span><span class="ir-value" id="d-tok">—</span></div><div class="info-row"><span class="ir-label">NTP</span><span class="ir-value" id="d-ntp">—</span></div><div class="info-row"><span class="ir-label">LCD</span><span class="ir-value" id="d-lcd">—</span></div><div class="info-row"><span class="ir-label">Whitelist age</span><span class="ir-value" id="d-wl">—</span></div><div class="btn-row"><button class="btn btn-secondary" id="btn-relay" onclick="testRelay()">🔓 Test relay</button><button class="btn btn-secondary" onclick="loadStatus()">↻ Refresh</button></div></div><div class="section">Server configuration</div><div class="card"><div class="card-hd"><h2>Connection</h2></div><div class="divider"></div><div class="field"><label for="f-server">Server URL</label><input type="url" id="f-server" placeholder="https://api.example.com/v1"
             autocapitalize="none" autocorrect="off" spellcheck="false"></div><div class="field"><label for="f-token">Device token <span class="lhint">— from admin panel</span></label><input type="password" id="f-token" placeholder="tok_••••••••••••" class="mono-in"
             autocapitalize="none" autocorrect="off" spellcheck="false"></div><div class="btn-row"><button class="btn btn-primary" id="btn-save-server" onclick="saveServer()">Save & test connection</button></div></div><div class="section">Hardware</div><div class="card"><div class="card-hd"><h2>Pins &amp; timing</h2><span class="hint">Requires reboot</span></div><div class="divider"></div><div class="field"><label>CS IN reader <span class="lhint">(PN532 #1)</span></label><input type="number" id="f-csin" min="0" max="39" placeholder="4"></div><div class="field"><label>CS OUT reader <span class="lhint">(PN532 #2)</span></label><input type="number" id="f-csout" min="0" max="39" placeholder="25"></div><div class="field"><label>CS SD card</label><input type="number" id="f-cssd" min="0" max="39" placeholder="5"></div><div class="field"><label>Relay pin</label><input type="number" id="f-relay" min="0" max="39" placeholder="26"></div><div class="field"><label>Relay open duration <span class="lhint">(ms)</span></label><input type="number" id="f-relayms" min="500" max="10000" step="100" placeholder="3000"></div><div class="field field-row"><div><label>Active-LOW relay</label><div style="font-size:11px;color:var(--hint);margin-top:2px">Enable if relay triggers in reverse</div></div><label class="toggle"><input type="checkbox" id="f-activelow"><span class="ttrack"></span></label></div><div class="btn-row"><button class="btn btn-primary" id="btn-save-hw" onclick="saveHardware()">Save hardware config</button></div></div><div class="section">Admin panel</div><div class="card"><div class="card-hd"><h2>Full admin panel</h2></div><div class="divider"></div><div class="field" style="border:none"><div style="font-size:13px;color:var(--muted);line-height:1.6">
        The full admin panel (events, employees, cards, devices) is served from the SD card.
        Insert SD card with <code style="font-size:11px;background:var(--s2);padding:1px 5px;border-radius:4px">/www/index.html</code> and tap below.
      </div></div><div class="btn-row"><button class="btn btn-secondary" onclick="window.location='/admin'" id="btn-admin">Open admin panel →</button></div></div><div class="section">Danger zone</div><div class="card"><div class="card-hd"><h2>Reset</h2></div><div class="divider"></div><div class="btn-row"><button class="btn btn-danger" onclick="doReset('wifi')">Reset WiFi</button><button class="btn btn-danger" onclick="doReset('all')">Factory reset</button></div></div></div><div class="toast" id="toast"></div><script>
let _status = null;

// ── Toast ─────────────────────────────────────────────────────────────────────
function toast(msg, dur=2500) {
  const t = document.getElementById('toast');
  t.textContent = msg; t.classList.add('show');
  setTimeout(() => t.classList.remove('show'), dur);
}

// ── Helpers ───────────────────────────────────────────────────────────────────
function fmtUptime(s) {
  if (!s) return '—';
  if (s < 60)   return s + 's';
  if (s < 3600) return Math.floor(s/60) + 'm ' + (s%60) + 's';
  return Math.floor(s/3600) + 'h ' + Math.floor((s%3600)/60) + 'm';
}
function fmtAge(s) {
  if (!s || s <= 0) return 'Never';
  if (s < 60)   return s + 's ago';
  if (s < 3600) return Math.floor(s/60) + 'm ago';
  return Math.floor(s/3600) + 'h ago';
}
function rssiStrength(r) {
  if (r >= -50) return 4; if (r >= -65) return 3;
  if (r >= -75) return 2; return 1;
}
function setBtn(id, loading) {
  const b = document.getElementById(id);
  if (!b) return;
  b.disabled = loading;
  if (loading) b.classList.add('btn-loading');
  else b.classList.remove('btn-loading');
}

// ── Load status ───────────────────────────────────────────────────────────────
async function loadStatus() {
  try {
    const r = await fetch('/api/status');
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const s = await r.json();
    _status = s;
    applyStatus(s);
  } catch(e) {
    document.getElementById('sdot').className = 'sdot err';
    document.getElementById('stxt').textContent = 'Device unreachable';
    console.warn(e);
  }
}

function applyStatus(s) {
  // Header
  document.getElementById('h-id').textContent = s.identifier || 'Unknown';

  // Status dot
  const dot = document.getElementById('sdot');
  const configured = s.token_set && s.server_url && s.server_url.length > 0;
  dot.className = 'sdot ' + (configured ? 'ok' : 'warn');
  document.getElementById('stxt').textContent = configured ? 'Connected' : 'Not configured';
  document.getElementById('ssub').textContent = s.ip || '';

  // Signal
  const bars = document.querySelectorAll('.sig-b');
  const str = rssiStrength(s.rssi || -90);
  bars.forEach((b, i) => b.classList.toggle('on', i < str));

  // Metrics
  document.getElementById('m-up').textContent   = fmtUptime(s.uptime_s);
  document.getElementById('m-sync').textContent = s.unsynced_events ?? '—';
  document.getElementById('m-sync-sub').textContent = (s.unsynced_events > 0) ? 'Pending' : 'All synced';
  document.getElementById('m-ip').textContent   = s.ip || '—';

  const sdOk = s.sd_mounted;
  document.getElementById('m-sd').textContent   = sdOk ? (s.sd_used_mb + '/' + s.sd_total_mb + 'MB') : 'No card';
  document.getElementById('m-sd-sub').textContent = sdOk ? '' : 'Insert SD card';

  // Info rows
  document.getElementById('fw-chip').innerHTML = `<span class="chip chip-b">v${s.firmware||'?'}</span>`;
  document.getElementById('d-id').textContent  = s.identifier || '—';
  document.getElementById('d-srv').textContent = s.server_url || 'Not set';
  document.getElementById('d-tok').innerHTML   = s.token_set
    ? '<span class="chip chip-g">Set ✓</span>'
    : '<span class="chip chip-r">Not set</span>';
  document.getElementById('d-ntp').innerHTML   = s.ntp_synced
    ? '<span class="chip chip-g">Synced</span>'
    : '<span class="chip chip-a">Not synced</span>';
  document.getElementById('d-lcd').innerHTML   = s.lcd_found
    ? '<span class="chip chip-g">Found</span>'
    : '<span class="chip chip-r">Not found</span>';
  document.getElementById('d-wl').textContent  = fmtAge(s.whitelist_age_s);

  // Admin panel button
  document.getElementById('btn-admin').disabled = !sdOk;

  // Pre-fill form fields (don't overwrite focused field)
  const active = document.activeElement;
  if (active !== document.getElementById('f-server'))
    document.getElementById('f-server').value = s.server_url || '';
  // Placeholders for hardware
  document.getElementById('f-csin').placeholder   = s.cs_in   ?? 4;
  document.getElementById('f-csout').placeholder  = s.cs_out  ?? 25;
  document.getElementById('f-cssd').placeholder   = s.cs_sd   ?? 5;
  document.getElementById('f-relay').placeholder  = s.relay_pin ?? 26;
  document.getElementById('f-relayms').placeholder= s.relay_ms  ?? 3000;
}

// ── Save server config ────────────────────────────────────────────────────────
async function saveServer() {
  const sv = document.getElementById('f-server').value.trim();
  const tk = document.getElementById('f-token').value.trim();
  if (!sv) { toast('Enter a server URL'); return; }

  setBtn('btn-save-server', true);
  const body = { server_url: sv };
  if (tk) body.device_token = tk;

  try {
    const r = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    toast('Saved ✓');
    document.getElementById('f-token').value = '';
    setTimeout(loadStatus, 600);
  } catch(e) {
    toast('Failed: ' + e.message);
  }
  setBtn('btn-save-server', false);
}

// ── Save hardware config ──────────────────────────────────────────────────────
async function saveHardware() {
  const body = {};
  const csi = document.getElementById('f-csin').value;
  const cso = document.getElementById('f-csout').value;
  const csd = document.getElementById('f-cssd').value;
  const rp  = document.getElementById('f-relay').value;
  const rm  = document.getElementById('f-relayms').value;
  const al  = document.getElementById('f-activelow').checked;

  if (csi) body.cs_in      = parseInt(csi);
  if (cso) body.cs_out     = parseInt(cso);
  if (csd) body.cs_sd      = parseInt(csd);
  if (rp)  body.relay_pin  = parseInt(rp);
  if (rm)  body.relay_ms   = parseInt(rm);
  body.active_low = al;

  setBtn('btn-save-hw', true);
  try {
    const r = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    toast('Saved — reboot to apply ✓');
    setTimeout(loadStatus, 600);
  } catch(e) {
    toast('Failed: ' + e.message);
  }
  setBtn('btn-save-hw', false);
}

// ── Test relay ────────────────────────────────────────────────────────────────
async function testRelay() {
  const btn = document.getElementById('btn-relay');
  btn.disabled = true; btn.textContent = 'Opening…';
  try {
    const r = await fetch('/api/open', { method: 'POST' });
    toast(r.ok ? 'Relay triggered ✓' : 'Failed');
  } catch(e) { toast('Error: ' + e.message); }
  setTimeout(() => { btn.disabled = false; btn.textContent = '🔓 Test relay'; },
             (_status?.relay_ms || 3000) + 200);
}

// ── Reset ─────────────────────────────────────────────────────────────────────
async function doReset(type) {
  const msg = type === 'all'
    ? 'Factory reset will erase all settings including WiFi credentials. Are you sure?'
    : 'This will erase WiFi credentials and reboot into setup mode. Continue?';
  if (!confirm(msg)) return;
  try {
    await fetch('/api/reset?type=' + type, { method: 'POST' });
    toast('Resetting…');
  } catch(e) {
    toast('Reset sent — device rebooting');
  }
}

// ── Boot ──────────────────────────────────────────────────────────────────────
loadStatus();
setInterval(loadStatus, 10000);
</script></body></html>
)CP_SETUP_HTML";
