// Local Connect's page: what a paired phone or tablet opens. It shows the picture of the tablet in the sim and sends touches,
// scrolls and keys back (src/localmirror.h); before that, it asks once for the pairing code shown in the sim.
// One small page, no libraries. (In pieces: some compilers limit the length of one string.)
#pragma once

namespace xpt {

inline const char* const kLocalPage[] = {
R"XPT(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black">
<meta name="theme-color" content="#000000">
<meta name="apple-mobile-web-app-title" content="XPTablet">
<meta name="application-name" content="XPTablet">
<link rel="apple-touch-icon" href="data:image/png;base64,%XPT_ICON%">
<link rel="icon" type="image/png" href="data:image/png;base64,%XPT_ICON%">
<title>XPTablet</title>
<style>
html, body { margin: 0; height: 100%; background: #000; color: #ecf1fa; font: 15px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; overflow: hidden; overscroll-behavior: none; }
#wrap { position: fixed; inset: 0; display: flex; align-items: center; justify-content: center;
        padding: env(safe-area-inset-top, 0px) env(safe-area-inset-right, 0px) env(safe-area-inset-bottom, 0px) env(safe-area-inset-left, 0px);
        -webkit-user-select: none; user-select: none; -webkit-touch-callout: none; }
canvas { display: none; touch-action: none; -webkit-tap-highlight-color: transparent; outline: none; background: #000; }
:root { --top: #0a1226; --bot: #16264d; --accent: #4c8dff; --accentLite: #6ea3ff; }
#panel { position: fixed; inset: 0; display: flex; align-items: center; justify-content: center; padding: 20px;
         background: linear-gradient(180deg, var(--top) 0%, var(--bot) 100%); transition: background .6s; }
#card { width: 100%; max-width: 380px; text-align: center; }
#brand { margin: 0 auto 24px; }
#brand img { display: block; width: 180px; max-width: 60%; height: auto; margin: 0 auto; }
h1 { font-size: 19px; font-weight: 700; margin: 0 0 6px; }
p { margin: 0 0 16px; color: #9fb0cc; font-size: 14px; }
#code { display: none; width: 100%; box-sizing: border-box; font: 700 30px/1.2 ui-monospace, Menlo, Consolas, monospace; letter-spacing: 12px; text-align: center; padding: 12px 0 12px 12px;
        color: #ecf1fa; background: rgba(10, 20, 38, .9); border: 1.5px solid rgba(255, 255, 255, .16); border-radius: 14px; outline: none; }
#code:focus { border-color: var(--accentLite); }
#msg { min-height: 20px; margin-top: 12px; color: #ffb454; font-size: 14px; }
#again { display: none; margin: 14px auto 0; font: 600 15px system-ui, sans-serif; color: #fff; background: var(--accent); border: 0; border-radius: 999px; padding: 10px 26px; }
.tool { display: none; position: fixed; z-index: 5; width: 40px; height: 40px; border-radius: 12px; border: 0; padding: 0;
        background: rgba(10, 20, 38, .72); color: #ecf1fa; font: 600 18px system-ui, sans-serif; transition: opacity .4s; -webkit-tap-highlight-color: transparent; }
#fs { top: calc(10px + env(safe-area-inset-top, 0px)); right: calc(10px + env(safe-area-inset-right, 0px)); }
#kbbtn { bottom: calc(12px + env(safe-area-inset-bottom, 0px)); right: calc(10px + env(safe-area-inset-right, 0px)); width: auto; padding: 0 14px; font-size: 14px; background: var(--accent); }
#kb { position: fixed; left: -1000px; top: 0; width: 10px; height: 10px; opacity: 0; font-size: 16px; }
#turn { display: none; position: fixed; left: 0; right: 0; bottom: calc(14px + env(safe-area-inset-bottom, 0px)); text-align: center; color: #93a4c0; font-size: 13px; pointer-events: none; }
</style>
</head>
<body>
<div id="wrap"><canvas id="canvas" width="800" height="480"></canvas></div>
<div id="turn">Turn your device sideways for a larger tablet.</div>
<button id="fs" class="tool" type="button" aria-label="Full screen">&#x26F6;</button>
<button id="kbbtn" class="tool" type="button">Keyboard</button>
<textarea id="kb" autocapitalize="off" autocomplete="off" autocorrect="off" spellcheck="false" aria-label="Typing"></textarea>
<div id="panel"><div id="card">
  <div id="brand"><img alt="XPTablet" src="data:image/png;base64,%XPT_LOGO%"></div>
  <h1 id="title">Connecting To XPTablet</h1>
  <p id="hint">Looking for XPTablet on your computer...</p>
  <input id="code" inputmode="numeric" pattern="[0-9]*" maxlength="6" autocomplete="one-time-code" aria-label="Pairing code">
  <div id="msg"></div>
  <button id="again" type="button">Try Again</button>
</div></div>
)XPT",
R"XPT(<script>
(function () {
  'use strict';
  var VERSION = 4, TOKEN = 'xptablet.lc.token';
  var $ = function (id) { return document.getElementById(id); };
  var canvas = $('canvas'), ctx = canvas.getContext('2d'), wrap = $('wrap'), panel = $('panel'), code = $('code');
  var ws = null, stop = false, reload = false, live = false, fullW = 800, fullH = 480, chain = Promise.resolve(), gotPicture = false;

  function show(title, hint, opts) {   // the card over the picture: connecting, the code, or why not
    opts = opts || {};
    panel.style.display = 'flex'; canvas.style.display = 'none'; $('turn').style.display = 'none';
    $('title').textContent = title; $('hint').textContent = hint || '';
    code.style.display = opts.code ? 'block' : 'none';
    $('again').style.display = opts.again ? 'block' : 'none';
    $('msg').textContent = opts.msg || '';
    if (opts.code) { code.value = ''; setTimeout(function () { try { code.focus(); } catch (e) {} }, 50); }
  }
  function fit() {   // the picture as large as the screen allows, in its own shape
    var cs = getComputedStyle(wrap);
    var vw = wrap.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight), vh = wrap.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom);
    var k = Math.min(vw / fullW, vh / fullH);
    canvas.style.width = Math.floor(fullW * k) + 'px'; canvas.style.height = Math.floor(fullH * k) + 'px';
    $('turn').style.display = live && gotPicture && vh > vw * 1.05 ? 'block' : 'none';
  }
  function kind() {
    var ua = navigator.userAgent || '';
    if (/iPhone|iPod/.test(ua)) return 'iPhone';
    if (/iPad/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1)) return 'iPad';
    if (/Android/.test(ua)) return /Mobile/.test(ua) ? 'Android phone' : 'Android tablet';
    return 'Computer';
  }
  function bytes() { var a = []; for (var i = 0; i < arguments.length; ++i) a.push(arguments[i]); return a; }
  function str(a, s) { var u = new TextEncoder().encode(s), n = u.length; while (n >= 0x80) { a.push((n & 0x7F) | 0x80); n >>>= 7; } a.push(n); for (var i = 0; i < u.length; ++i) a.push(u[i]); }
  function send(a) { if (ws && ws.readyState === 1) ws.send(new Uint8Array(a)); }
  function text(u, at) { var n = 0, sh = 0, c; do { c = u[at++]; n |= (c & 0x7F) << sh; sh += 7; } while (c & 0x80 && at < u.length); return new TextDecoder().decode(u.subarray(at, at + n)); }

  // ---- the tablet's theme on these screens (kept for the next visit, so the page opens in it) ----
  var THEME = 'xptablet.lc.theme';
  function applyTheme(t) {
    if (!t || !t.top) return;
    var r = document.documentElement.style;
    r.setProperty('--top', t.top); r.setProperty('--bot', t.bot); r.setProperty('--accent', t.accent); r.setProperty('--accentLite', t.accentLite || t.accent);
    var m = document.querySelector('meta[name="theme-color"]'); if (m) m.setAttribute('content', t.top);
  }
  try { applyTheme(JSON.parse(localStorage.getItem(THEME) || 'null')); } catch (e) {}
  function theme() {
    fetch('/theme', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (t) {
      if (!t || !t.top) return;
      applyTheme(t); try { localStorage.setItem(THEME, JSON.stringify(t)); } catch (e) {}
    }).catch(function () {});
  }
  setInterval(function () { if (panel.style.display !== 'none') theme(); }, 8000);   // (its screens shown: a theme changed in the sim follows)

  function connect() {
    theme();
    if (ws) { try { ws.onclose = null; ws.close(); } catch (e) {} }
    stop = false; live = false; gotPicture = false;
    var s = new WebSocket('wss://' + location.host + '/ws');
    s.binaryType = 'arraybuffer';
    ws = s;
    s.onopen = function () {
      var a = bytes(72, VERSION), tok = null;   // 'H'
      try { tok = localStorage.getItem(TOKEN); } catch (e) {}
      str(a, tok || ''); str(a, kind()); str(a, kind());
      send(a);
    };
    s.onmessage = function (e) {
      var u = new Uint8Array(e.data);
      if (u[0] === 104) hello(u);        // 'h'
      else if (u[0] === 70) picture(u);  // 'F'
    };
    s.onclose = function () {
      if (ws !== s) return;
      ws = null;
      if (stop) return;
      show('Connecting To XPTablet', 'Looking for XPTablet on your computer...');
      setTimeout(function () { if (!ws && !stop) connect(); }, 2000);
    };
  }
  function hello(u) {
    var st = u[1], t = u.length > 2 ? text(u, 2) : '';
    if (st === 1) {   // in (with the token, when this was the pairing)
      if (t) { try { localStorage.setItem(TOKEN, t); } catch (e) {} }
      live = true; code.blur();
      show('Connected', 'Waiting for the tablet\'s picture...');
      battery();
    } else if (st === 0) show('Enter The Pairing Code', 'It is shown in X-Plane, on XPTablet: Settings, Local Connect.', { code: true });
    else if (st === 4) show('Enter The Pairing Code', 'It is shown in X-Plane, on XPTablet: Settings, Local Connect.', { code: true, msg: 'That is not the code. Tries left: ' + t + '.' });
    else { stop = true; reload = st === 2; show('Not Connected', '', { again: true, msg: t || 'The computer turned this device away.' }); try { ws.close(); } catch (e) {} }
  }
  function picture(u) {   // a piece of the tablet's picture: drawn in the order they came
    var W = u[1] | (u[2] << 8), H = u[3] | (u[4] << 8), x = u[5] | (u[6] << 8), y = u[7] | (u[8] << 8), key = (u[13] & 1) !== 0;
    typingNow((u[13] & 2) !== 0);   // (2: the tablet is typing, and wants this device's own keyboard)
    var blob = new Blob([u.subarray(14)], { type: 'image/jpeg' });
    chain = chain.then(function () { return createImageBitmap(blob); }).then(function (bmp) {
      if (canvas.width !== W || canvas.height !== H) { if (!key) return; canvas.width = W; canvas.height = H; }
      fullW = W; fullH = H;
      ctx.drawImage(bmp, x, y);
      if (bmp.close) bmp.close();
      if (!gotPicture && live) { gotPicture = true; panel.style.display = 'none'; canvas.style.display = 'block'; fsShow(); }
      fit();
    }).catch(function () {});
  }
  var batt = null;
  function battery() {
    if (batt) { batt(); return; }
    if (!navigator.getBattery) return;   // (iPhone and iPad browsers do not share it)
    navigator.getBattery().then(function (b) {
      batt = function () { send(bytes(66, Math.round(b.level * 100) + 1, b.charging ? 1 : 0)); };
      b.addEventListener('levelchange', batt); b.addEventListener('chargingchange', batt);
      batt();
    }).catch(function () {});
  }
)XPT",
R"XPT(
  // ---- touches, scrolls and keys: sent as they are, placed as fractions of the picture ----
  function at(e) { var r = canvas.getBoundingClientRect(); return [Math.max(0, Math.min(65535, Math.round((e.clientX - r.left) / r.width * 65535))), Math.max(0, Math.min(65535, Math.round((e.clientY - r.top) / r.height * 65535)))]; }
  // ---- the device's own keyboard (when the tablet is set to use it): a hidden box raised by a tap, its typing sent on ----
  var kb = $('kb'), kbbtn = $('kbbtn'), typing = false;
  function typingNow(on) {
    if (on === typing) return;
    typing = on;
    kbbtn.style.display = on ? 'block' : 'none';
    if (!on && document.activeElement === kb) kb.blur();
  }
  function raiseKeys() { if (typing) { kb.value = ''; kb.focus({ preventScroll: true }); } }   // (only from a tap: browsers raise keyboards for those)
  kbbtn.addEventListener('click', function (e) { e.preventDefault(); raiseKeys(); });
  kb.addEventListener('input', function (e) {   // (phones: what was typed comes here, not as keys)
    var t = e.inputType || '';
    if (t === 'deleteContentBackward') send(bytes(73, 4, 8, 0));
    else if (t === 'insertLineBreak' || t === 'insertParagraph') send(bytes(73, 4, 13, 0));
    else if (e.data) for (var i = 0; i < e.data.length; ++i) { var c = e.data.charCodeAt(i); if (c >= 32 && c < 127) send(bytes(73, 5, c, 0)); }
    kb.value = '';
  });
  // ---- full screen, where the browser allows it (Android, computers, iPad; an iPhone: Add To Home Screen instead) ----
  var fsBtn = $('fs'), root = document.documentElement, fsIdle = null;
  var canFs = !!(root.requestFullscreen || root.webkitRequestFullscreen) && !window.navigator.standalone;
  function inFs() { return !!(document.fullscreenElement || document.webkitFullscreenElement); }
  function fsShow() {   // shown a while after a touch, then it fades (it sits over a corner of the picture)
    if (!canFs || !live) return;
    fsBtn.style.display = 'block'; fsBtn.style.opacity = '1';
    clearTimeout(fsIdle); fsIdle = setTimeout(function () { fsBtn.style.opacity = '0.25'; }, 3500);
  }
  fsBtn.addEventListener('click', function (e) {
    e.preventDefault();
    if (inFs()) { (document.exitFullscreen || document.webkitExitFullscreen).call(document); }
    else { var p = (root.requestFullscreen || root.webkitRequestFullscreen).call(root); if (p && p.catch) p.catch(function () {}); }
    fsShow();
  });
  document.addEventListener('fullscreenchange', function () { setTimeout(fit, 60); });
  document.addEventListener('webkitfullscreenchange', function () { setTimeout(fit, 60); });
  function ptr(k, e, touch) { var p = at(e); send(bytes(73, k | (touch ? 16 : 0), p[0] & 255, p[0] >> 8, p[1] & 255, p[1] >> 8)); }   // 'I'
  canvas.addEventListener('pointermove', function (e) { if (!e.isPrimary || e.pointerType === 'touch') return; ptr(0, e, false); e.preventDefault(); });
  canvas.addEventListener('pointerdown', function (e) { if (!e.isPrimary || e.pointerType === 'touch') return; try { canvas.setPointerCapture(e.pointerId); } catch (x) {} ptr(1, e, false); e.preventDefault(); });
  canvas.addEventListener('pointerup', function (e) { if (!e.isPrimary || e.pointerType === 'touch') return; ptr(2, e, false); e.preventDefault(); });
  canvas.addEventListener('pointercancel', function (e) { if (e.pointerType !== 'touch') send(bytes(73, 6)); });
  canvas.addEventListener('contextmenu', function (e) { e.preventDefault(); });
  canvas.addEventListener('wheel', function (e) { send(bytes(73, 3, e.deltaY < 0 ? 1 : 0)); e.preventDefault(); }, { passive: false });
  // a finger: a tap keeps still (within 10 px it stays where it went down); two fingers pinch (a scroll in or out)
  var finger = null, down = null, still = true, pinch = 0;
  function span(t) { return Math.hypot(t[0].clientX - t[1].clientX, t[0].clientY - t[1].clientY); }
  canvas.addEventListener('touchstart', function (e) {
    e.preventDefault();
    if (e.touches.length >= 2) { if (finger !== null) { ptr(2, down, true); finger = null; } pinch = span(e.touches); return; }
    var t = e.changedTouches[0];
    finger = t.identifier; still = true; down = { clientX: t.clientX, clientY: t.clientY };
    fsShow();
    ptr(0, down, true); ptr(1, down, true);
  }, { passive: false });
  canvas.addEventListener('touchmove', function (e) {
    e.preventDefault();
    if (e.touches.length >= 2 && pinch > 0) { var d = span(e.touches); if (d > pinch * 1.35) { send(bytes(73, 3, 1)); pinch = d; } else if (d < pinch / 1.35) { send(bytes(73, 3, 0)); pinch = d; } return; }
    for (var i = 0; i < e.changedTouches.length; ++i) { var t = e.changedTouches[i]; if (t.identifier !== finger) continue;
      if (still && Math.hypot(t.clientX - down.clientX, t.clientY - down.clientY) < 10) return;
      still = false; down = { clientX: t.clientX, clientY: t.clientY }; ptr(0, down, true); }
  }, { passive: false });
  function lift(e) {
    e.preventDefault();
    if (e.touches.length < 2) pinch = 0;
    for (var i = 0; i < e.changedTouches.length; ++i) { var t = e.changedTouches[i]; if (t.identifier !== finger) continue;
      if (!still) down = { clientX: t.clientX, clientY: t.clientY };
      ptr(2, down, true); finger = null; }
    if (typing && document.activeElement !== kb) raiseKeys();   // (typing already: a tap on the picture raises the keyboard again)
  }
  canvas.addEventListener('touchend', lift, { passive: false });
  canvas.addEventListener('touchcancel', lift, { passive: false });
  // a keyboard on the device: the arrows, W A S D and Space play the tablet's games (pressed and let go, as on the computer)
  var GAME = { ArrowLeft: 0, ArrowRight: 1, ArrowUp: 2, ArrowDown: 3, a: 0, d: 1, w: 2, s: 3, A: 0, D: 1, W: 2, S: 3, ' ': 4 };
  function gameKey(e, on) { if (!live || e.target === code || e.target === kb || e.ctrlKey || e.metaKey || e.repeat) return false; var k = GAME[e.key]; if (k === undefined) return false; send(bytes(73, 7, k, on ? 1 : 0)); return true; }
  window.addEventListener('keyup', function (e) { if (gameKey(e, false)) e.preventDefault(); });
  window.addEventListener('keydown', function (e) {   // and it types into the tablet's text fields
    if (e.target !== kb && gameKey(e, true) && e.key.indexOf('Arrow') === 0) { e.preventDefault(); return; }
    if (!live || e.target === code || e.ctrlKey || e.metaKey) return;
    if (e.key === 'Backspace') { send(bytes(73, 4, 8, 0)); e.preventDefault(); }
    else if (e.key === 'Enter') { send(bytes(73, 4, 13, 0)); e.preventDefault(); }
    else if (e.key.length === 1) { var c = e.key.charCodeAt(0); if (c >= 32 && c < 127) { send(bytes(73, 5, c, 0)); e.preventDefault(); } }
  });
  // ---- the pairing code: sent when the sixth digit is in ----
  code.addEventListener('input', function () {
    code.value = code.value.replace(/[^0-9]/g, '').slice(0, 6);
    if (code.value.length === 6) { var a = bytes(80); str(a, code.value); send(a); $('msg').textContent = 'Checking...'; }   // 'P'
  });
  $('again').addEventListener('click', function () { if (reload) location.reload(); else { show('Connecting To XPTablet', 'Looking for XPTablet on your computer...'); connect(); } });
  window.addEventListener('resize', function () { setTimeout(fit, 50); });
  window.addEventListener('orientationchange', function () { setTimeout(fit, 250); });
  document.addEventListener('visibilitychange', function () { if (!document.hidden && !ws && !stop) connect(); });   // back from sleep: at once
  window.xptPhase = function () { return panel.style.display === 'none' ? 3 : code.style.display === 'block' ? 1 : $('again').style.display === 'block' ? 2 : 0; };   // (for the tests)
  show('Connecting To XPTablet', 'Looking for XPTablet on your computer...');
  connect();
})();
</script>
</body>
</html>
)XPT"};

}   // namespace xpt
