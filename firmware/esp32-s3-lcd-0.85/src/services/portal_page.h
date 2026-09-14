#pragma once

// Wi-Fi setup page served by WebPortal. Self-contained: no external assets.
static constexpr const char kPortalPage[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#09090b">
<title>Wi-Fi setup</title>
<style>
:root{color-scheme:dark;--bg:#09090b;--card:#18181b;--line:#27272a;--text:#fafafa;--muted:#a1a1aa;--accent:#80a0ff;--ok:#3cc878;--bad:#f05050}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-tap-highlight-color:transparent}
main{max-width:440px;margin:0 auto;padding:max(20px,env(safe-area-inset-top)) 16px 32px}
h1{font-size:18px;margin:0}
.sub{color:var(--muted);font-size:13px;margin:2px 0 18px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:14px 16px;margin-bottom:14px}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px}
.label{color:var(--muted);font-size:12px}
.dot{width:8px;height:8px;border-radius:50%;background:var(--muted);flex:none}
.dot.ok{background:var(--ok)}.dot.wait{background:#f0c040;animation:p 1s infinite}
@keyframes p{50%{opacity:.3}}
.head{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
button{font:inherit;color:inherit;background:none;border:0;cursor:pointer}
.link{color:var(--accent);font-size:13px;padding:6px 0}
ul{list-style:none;margin:0;padding:0}
li button{width:100%;display:flex;align-items:center;gap:10px;padding:12px 2px;border-top:1px solid var(--line);text-align:left}
li:first-child button{border-top:0}
.name{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.bars{display:flex;align-items:flex-end;gap:2px;height:14px}
.bars i{width:3px;background:var(--line);border-radius:1px}
.bars i.on{background:var(--text)}
.lock{color:var(--muted);font-size:12px}
.empty{color:var(--muted);font-size:13px;padding:10px 0}
dialog{border:1px solid var(--line);border-radius:18px;background:var(--card);color:var(--text);width:min(92vw,400px);padding:18px}
dialog::backdrop{background:#000a}
input{width:100%;font:inherit;color:var(--text);background:var(--bg);border:1px solid var(--line);border-radius:12px;padding:12px;margin:6px 0 12px;outline:none}
input:focus{border-color:var(--accent)}
.actions{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.btn{height:44px;border-radius:12px;border:1px solid var(--line);font-weight:600}
.btn.primary{background:var(--text);color:var(--bg);border:0}
.btn:disabled{opacity:.4}
.msg{font-size:13px;margin-top:10px;color:var(--muted)}
.msg.bad{color:var(--bad)}
.spin{width:14px;height:14px;border:2px solid var(--line);border-top-color:var(--text);border-radius:50%;animation:s .8s linear infinite;display:inline-block;vertical-align:-2px}
@keyframes s{to{transform:rotate(360deg)}}
</style>
</head>
<body>
<main>
  <h1 id="device">Wi-Fi setup</h1>
  <p class="sub">Choose the network this board should join.</p>

  <section class="card">
    <div class="row">
      <div>
        <div class="label">Status</div>
        <div id="state">…</div>
      </div>
      <span id="dot" class="dot"></span>
    </div>
    <div id="ipRow" class="row" style="margin-top:10px" hidden>
      <div><div class="label">IP address</div><div id="ip"></div></div>
    </div>
  </section>

  <section class="card">
    <div class="head">
      <strong>Networks</strong>
      <button id="rescan" class="link">Rescan</button>
    </div>
    <ul id="list"><li class="empty"><span class="spin"></span> Scanning…</li></ul>
    <button id="other" class="link">Other network…</button>
  </section>
</main>

<dialog id="dlg">
  <form id="form" method="dialog">
    <strong id="dlgTitle">Connect</strong>
    <div id="ssidWrap" hidden>
      <div class="label" style="margin-top:12px">Network name</div>
      <input id="ssid" autocomplete="off" autocapitalize="none" spellcheck="false">
    </div>
    <div id="passWrap">
      <div class="label" style="margin-top:12px">Password</div>
      <input id="pass" type="password" autocomplete="current-password">
    </div>
    <div class="actions">
      <button type="button" class="btn" id="cancel">Cancel</button>
      <button type="submit" class="btn primary" id="join">Connect</button>
    </div>
    <div id="msg" class="msg"></div>
  </form>
</dialog>

<script>
const $ = (id) => document.getElementById(id);
let selected = null;
let waiting = false;

function bars(rssi) {
  const n = rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
  return '<span class="bars">' + [6, 10, 14].map((h, i) =>
    '<i style="height:' + h + 'px" class="' + (i < n ? 'on' : '') + '"></i>').join('') + '</span>';
}
function esc(s) {
  return s.replace(/[&<>"]/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
}

async function status() {
  try {
    const s = await (await fetch('/api/status')).json();
    $('device').textContent = s.device;
    const text = { Connected: 'Connected to ' + s.ssid, Connecting: 'Connecting to ' + s.ssid + '…', Setup: 'Not configured', Off: 'Wi-Fi off' };
    $('state').textContent = text[s.state] || s.state;
    $('dot').className = 'dot ' + (s.state === 'Connected' ? 'ok' : s.state === 'Connecting' ? 'wait' : '');
    $('ipRow').hidden = s.state !== 'Connected';
    $('ip').textContent = s.ip + (s.ap ? '  ·  setup network closes in 30 s' : '');
    if (waiting && s.state === 'Connected') {
      waiting = false;
      $('msg').textContent = 'Connected. Open http://' + s.ip + ' on that network.';
      $('msg').className = 'msg';
      setTimeout(() => $('dlg').close(), 2500);
    }
  } catch (e) {}
}

async function scan(force) {
  if (force) $('list').innerHTML = '<li class="empty"><span class="spin"></span> Scanning…</li>';
  try {
    const r = await (await fetch('/api/scan' + (force ? '?force=1' : ''))).json();
    if (r.scanning && !r.networks.length) { setTimeout(() => scan(false), 1500); return; }
    const seen = new Set();
    const nets = r.networks.filter((n) => n.ssid && !seen.has(n.ssid) && seen.add(n.ssid));
    $('list').innerHTML = nets.length ? nets.map((n, i) =>
      '<li><button data-i="' + i + '">' + bars(n.rssi) + '<span class="name">' + esc(n.ssid) +
      '</span>' + (n.secure ? '<span class="lock">🔒</span>' : '') + '</button></li>').join('')
      : '<li class="empty">No networks found</li>';
    $('list').querySelectorAll('button').forEach((b) => b.onclick = () => open(nets[b.dataset.i]));
    if (r.scanning) setTimeout(() => scan(false), 1500);
  } catch (e) {
    setTimeout(() => scan(false), 2000);
  }
}

function open(net) {
  selected = net;
  $('dlgTitle').textContent = net ? net.ssid : 'Other network';
  $('ssidWrap').hidden = !!net;
  $('passWrap').hidden = net && !net.secure;
  $('ssid').value = '';
  $('pass').value = '';
  $('msg').textContent = '';
  $('join').disabled = false;
  $('dlg').showModal();
  (net ? (net.secure ? $('pass') : $('join')) : $('ssid')).focus();
}

$('form').onsubmit = async (e) => {
  e.preventDefault();
  const ssid = selected ? selected.ssid : $('ssid').value.trim();
  if (!ssid) return;
  $('join').disabled = true;
  $('msg').className = 'msg';
  $('msg').innerHTML = '<span class="spin"></span> Connecting… this page may briefly lose the connection.';
  const body = new URLSearchParams({ ssid, password: $('pass').value });
  try {
    const r = await fetch('/api/wifi', { method: 'POST', body });
    if (!r.ok) throw new Error(await r.text());
    waiting = true;
  } catch (err) {
    $('msg').className = 'msg bad';
    $('msg').textContent = 'Could not save: ' + err.message;
    $('join').disabled = false;
  }
};
$('cancel').onclick = () => { waiting = false; $('dlg').close(); };
$('rescan').onclick = () => scan(true);
$('other').onclick = () => open(null);

status();
scan(false);
setInterval(status, 2000);
</script>
</body>
</html>)HTML";
