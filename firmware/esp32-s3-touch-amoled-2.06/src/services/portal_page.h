#pragma once

// Wi-Fi setup page served by WebPortal. Self-contained: no external assets.
//
// Deliberately calm: the list is drawn once from the scan the board made before opening its
// access point, and is only redrawn when Rescan is tapped. Scanning (and joining a network) moves
// the radio to other channels, which briefly disconnects the phone showing this page — so the
// page never does either on its own, and says so when the user asks for it.
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
[hidden]{display:none!important}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-tap-highlight-color:transparent}
main{max-width:440px;margin:0 auto;padding:max(20px,env(safe-area-inset-top)) 16px 32px}
h1{font-size:18px;margin:0}
.sub{color:var(--muted);font-size:13px;margin:2px 0 18px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:14px 16px;margin-bottom:14px}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px}
.label{color:var(--muted);font-size:12px}
.dot{width:8px;height:8px;border-radius:50%;background:var(--muted);flex:none}
.dot.ok{background:var(--ok)}.dot.wait{background:#f0c040;animation:p 1s infinite}.dot.bad{background:var(--bad)}
@keyframes p{50%{opacity:.3}}
.head{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
button{font:inherit;color:inherit;background:none;border:0;cursor:pointer}
.link{color:var(--accent);font-size:13px;padding:6px 0}
ul{list-style:none;margin:0;padding:0}
li button{width:100%;display:flex;align-items:center;gap:10px;padding:14px 2px;border-top:1px solid var(--line);text-align:left}
li:first-child button{border-top:0}
.name{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.bars{display:flex;align-items:flex-end;gap:2px;height:14px}
.bars i{width:3px;background:var(--line);border-radius:1px}
.bars i.on{background:var(--text)}
.lock{color:var(--muted);font-size:12px}
.empty{color:var(--muted);font-size:13px;padding:10px 0}
.note{color:var(--muted);font-size:12px;margin-top:8px}
input{width:100%;font:inherit;color:var(--text);background:var(--bg);border:1px solid var(--line);border-radius:12px;padding:12px;margin:6px 0 12px;outline:none}
input:focus{border-color:var(--accent)}
.actions{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.btn{height:44px;border-radius:12px;border:1px solid var(--line);font-weight:600}
.btn.primary{background:var(--text);color:var(--bg);border:0}
.btn:disabled{opacity:.4}
.msg{font-size:13px;margin-top:10px;color:var(--muted)}
.msg.bad{color:var(--bad)}.msg.ok{color:var(--ok)}
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
    <div id="ipRow" class="note" hidden></div>
  </section>

  <!-- Network list, or the password form for the chosen network: never both, never a popup. -->
  <section class="card" id="pick">
    <div class="head">
      <strong>Networks</strong>
      <button id="rescan" class="link">Rescan</button>
    </div>
    <ul id="list"><li class="empty"><span class="spin"></span> Loading…</li></ul>
    <button id="other" class="link">Other network…</button>
  </section>

  <section class="card" id="join" hidden>
    <strong id="joinTitle">Connect</strong>
    <form id="form">
      <div id="ssidWrap" hidden>
        <div class="label" style="margin-top:12px">Network name</div>
        <input id="ssid" autocomplete="off" autocapitalize="none" spellcheck="false">
      </div>
      <div id="passWrap">
        <div class="label" style="margin-top:12px">Password</div>
        <input id="pass" type="password" autocomplete="current-password">
      </div>
      <div class="actions">
        <button type="button" class="btn" id="back">Back</button>
        <button type="submit" class="btn primary" id="go">Connect</button>
      </div>
    </form>
    <div id="msg" class="msg"></div>
  </section>
</main>

<script>
const $ = (id) => document.getElementById(id);
let chosen = null;      // network being joined; null = typed in by hand
let attempt = null;     // attempt id we are waiting for the result of

function bars(rssi) {
  const n = rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
  return '<span class="bars">' + [6, 10, 14].map((h, i) =>
    '<i style="height:' + h + 'px" class="' + (i < n ? 'on' : '') + '"></i>').join('') + '</span>';
}
function esc(s) {
  return s.replace(/[&<>"]/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
}
function message(text, kind) {
  $('msg').className = 'msg' + (kind ? ' ' + kind : '');
  $('msg').innerHTML = text;
}

async function status() {
  let s;
  try { s = await (await fetch('/api/status', { cache: 'no-store' })).json(); }
  catch (e) {
    // The phone dropped off the setup network (the radio changed channel while joining).
    if (attempt !== null) message('<span class="spin"></span> Waiting for the board… if your phone left the "' +
      esc($('device').textContent) + '" network, join it again.');
    return;
  }
  $('device').textContent = s.device;
  const text = { Connected: 'Connected to ' + s.ssid, Connecting: 'Connecting to ' + s.ssid + '…', Setup: 'Not connected', Off: 'Wi-Fi off' };
  $('state').textContent = text[s.state] || s.state;
  $('dot').className = 'dot ' + (s.state === 'Connected' ? 'ok' : s.state === 'Connecting' ? 'wait' : s.error ? 'bad' : '');
  $('ipRow').hidden = !(s.state === 'Connected' || s.error);
  $('ipRow').textContent = s.state === 'Connected'
    ? 'IP address ' + s.ip + (s.ap ? ' · this setup network closes in about a minute' : '')
    : 'Last attempt: ' + s.error;

  if (attempt === null || s.attempt !== attempt) return;
  if (s.state === 'Connected') {
    message('Connected to ' + esc(s.ssid) + '. The board is online — you can close this page.', 'ok');
    attempt = null;
  } else if (s.state === 'Setup' && s.error) {
    message(esc(s.error) + '. Check the password and try again.', 'bad');
    $('go').disabled = false;
    attempt = null;
  }
}

async function loadNetworks(force) {
  $('list').innerHTML = '<li class="empty"><span class="spin"></span> ' + (force ? 'Scanning…' : 'Loading…') + '</li>';
  try {
    let r = await (await fetch('/api/scan' + (force ? '?force=1' : ''), { cache: 'no-store' })).json();
    while (r.scanning) {
      await new Promise((ok) => setTimeout(ok, 1500));
      r = await (await fetch('/api/scan', { cache: 'no-store' })).json();
    }
    const seen = new Set();
    const nets = r.networks.filter((n) => n.ssid && !seen.has(n.ssid) && seen.add(n.ssid));
    $('list').innerHTML = nets.length ? nets.map((n, i) =>
      '<li><button data-i="' + i + '">' + bars(n.rssi) + '<span class="name">' + esc(n.ssid) +
      '</span>' + (n.secure ? '<span class="lock">🔒</span>' : '') + '</button></li>').join('')
      : '<li class="empty">No networks found. Tap Rescan.</li>';
    $('list').querySelectorAll('button').forEach((b) => b.onclick = () => showJoin(nets[b.dataset.i]));
  } catch (e) {
    $('list').innerHTML = '<li class="empty">Could not load networks. Tap Rescan.</li>';
  }
}

function showJoin(net) {
  chosen = net;
  $('joinTitle').textContent = net ? net.ssid : 'Other network';
  $('ssidWrap').hidden = !!net;
  $('passWrap').hidden = !!net && !net.secure;
  $('ssid').value = '';
  $('pass').value = '';
  $('go').disabled = false;
  message('');
  $('pick').hidden = true;
  $('join').hidden = false;
  (net ? (net.secure ? $('pass') : $('go')) : $('ssid')).focus();
}

$('back').onclick = () => { attempt = null; $('join').hidden = true; $('pick').hidden = false; };
$('other').onclick = () => showJoin(null);
// Two taps, because a scan can briefly disconnect the phone (no confirm(): captive-portal
// browsers on some phones ignore it).
let rescanArmed = false;
$('rescan').onclick = () => {
  if (!rescanArmed) {
    rescanArmed = true;
    $('rescan').textContent = 'Tap again — phone may disconnect briefly';
    setTimeout(() => { rescanArmed = false; $('rescan').textContent = 'Rescan'; }, 4000);
    return;
  }
  rescanArmed = false;
  $('rescan').textContent = 'Rescan';
  loadNetworks(true);
};

$('form').onsubmit = async (e) => {
  e.preventDefault();
  const ssid = chosen ? chosen.ssid : $('ssid').value.trim();
  const password = $('pass').value;
  if (!ssid) return;
  if (password && password.length < 8) { message('Wi-Fi passwords are at least 8 characters.', 'bad'); return; }
  $('go').disabled = true;
  message('<span class="spin"></span> Connecting… your phone may drop off this setup network for a moment.');
  try {
    const before = await (await fetch('/api/status', { cache: 'no-store' })).json();
    const r = await fetch('/api/wifi', { method: 'POST', body: new URLSearchParams({ ssid, password }) });
    if (!r.ok) throw new Error(await r.text());
    attempt = before.attempt + 1;
  } catch (err) {
    message('Could not send: ' + esc(err.message), 'bad');
    $('go').disabled = false;
  }
};

status();
loadNetworks(false);
setInterval(status, 1500);
</script>
</body>
</html>)HTML";
