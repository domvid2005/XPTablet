// Local Connect, end to end: a real browser (headless Chrome) opens the page the real server serves, pairs with the code the
// stand-in host prints (as the sim would show it), is shown the tablet's picture, and works the tablet by touch.
//   node tests/e2e_localconnect.mjs <local_host> <chrome> <out-dir>
import { spawn } from 'node:child_process';
import { mkdtempSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [hostBin, chromeBin, outDir] = process.argv.slice(2);
mkdirSync(outDir, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { if (!ok) { console.log('FAIL: ' + what); ++failures; } else console.log('ok: ' + what); };

const port = 18543, dbg = 9333;
const hostLines = [];
const host = spawn(hostBin, [mkdtempSync(join(tmpdir(), 'xpt-lc-data-')), String(port)]);
host.stdout.on('data', (d) => { for (const l of String(d).split('\n')) if (l) hostLines.push(l); });
const hostSaid = async (re, ms = 15000) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { const l = hostLines.find((x) => re.test(x)); if (l) return l; await sleep(50); } return null; };

const chrome = spawn(chromeBin, ['--headless=new', '--no-sandbox', '--disable-gpu', '--ignore-certificate-errors',
  '--remote-debugging-port=' + dbg, '--window-size=800,480', '--user-data-dir=' + mkdtempSync(join(tmpdir(), 'xpt-lc-chrome-')), 'about:blank'], { stdio: 'ignore' });
const done = (code) => { try { chrome.kill(); } catch (e) {} try { host.kill(); } catch (e) {} process.exit(code); };

try {
  check(await hostSaid(/^address: https:\/\/127\.0\.0\.1:/), 'the host is serving');
  let target = null;
  for (let i = 0; i < 100 && !target; ++i) { try { const l = await (await fetch(`http://127.0.0.1:${dbg}/json`)).json(); target = l.find((t) => t.type === 'page'); } catch (e) {} if (!target) await sleep(100); }
  if (!target) { console.log('FAIL: the browser did not start'); done(1); }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => ws.addEventListener('open', r));
  let nextId = 1;
  const waiting = new Map(), problems = [];
  ws.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m.result || m); waiting.delete(m.id); }
    if (m.method === 'Runtime.exceptionThrown') problems.push('exception: ' + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text));
    if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error' && !/favicon/.test(m.params.entry.url || '') && !/favicon/.test(m.params.entry.text || '')) problems.push('log: ' + m.params.entry.text + ' ' + (m.params.entry.url || ''));
  });
  const cdp = (method, params = {}) => new Promise((r) => { const id = nextId++; waiting.set(id, r); ws.send(JSON.stringify({ id, method, params })); });
  const js = async (expr) => (await cdp('Runtime.evaluate', { expression: expr, returnByValue: true })).result?.value;
  const shot = async (name) => { const r = await cdp('Page.captureScreenshot', { format: 'png' }); writeFileSync(join(outDir, name + '.png'), Buffer.from(r.data, 'base64')); };
  const phase = () => js("typeof window.xptPhase === 'function' ? window.xptPhase() : -1");
  const until = async (fn, ms = 20000) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (await fn()) return true; await sleep(100); } return false; };
  const type = (digits) => js(`(function(){var c=document.getElementById('code');c.value=${JSON.stringify(digits)};c.dispatchEvent(new Event('input'));})()`);
  // what the canvas shows, in short: how many different colours along a few rows, and a sum that changes when the picture does
  const picture = () => js("(function(){var c=document.getElementById('canvas'),x=c.getContext('2d'),seen={},n=0,sum=0;for(var r=1;r<8;++r){var d=x.getImageData(0,Math.floor(c.height*r/8),c.width,1).data;for(var i=0;i<d.length;i+=4){var k=(d[i]>>3)+','+(d[i+1]>>3)+','+(d[i+2]>>3);if(!seen[k]){seen[k]=1;++n;}sum=(sum+d[i]*3+d[i+1]*5+d[i+2]*7)%1000003;}}return c.width+'x'+c.height+' '+n+' '+sum;})()");
  const touch = async (x, y) => { await cdp('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y }] }); await sleep(90); await cdp('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] }); };
  const click = async (x, y) => { await cdp('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y }); await cdp('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 }); await sleep(90); await cdp('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 }); };

  await cdp('Runtime.enable'); await cdp('Log.enable'); await cdp('Page.enable');
  await cdp('Emulation.setDeviceMetricsOverride', { width: 800, height: 480, deviceScaleFactor: 2, mobile: true });
  await cdp('Emulation.setTouchEmulationEnabled', { enabled: true });
  await cdp('Page.navigate', { url: `https://127.0.0.1:${port}/` });

  check(await until(async () => (await phase()) === 1), 'the page loads over HTTPS, connects, and asks for the pairing code');
  const pair = await hostSaid(/^pair: \d{6} /);
  check(pair, 'the code is shown on the computer');
  await sleep(200);
  await shot('1_pairing');
  const code = pair.split(' ')[1], wrong = code === '111111' ? '222222' : '111111';
  await type(wrong);
  check(await until(async () => /Tries left: 4/.test(await js("document.getElementById('msg').textContent"))) && !hostLines.some((l) => /^devices: 1/.test(l)), 'a wrong code pairs nothing, and says how many tries are left');
  await shot('2_wrong_code');
  await type(code);
  check(await until(async () => (await phase()) === 3), 'the right code: the tablet\'s picture comes up');
  check(await hostSaid(/^devices: 1/), 'the computer lists the device as connected');
  await sleep(1500);
  await shot('3_mirror_home');
  const home = await picture();
  check(/^1200x720 /.test(home) && Number(home.split(' ')[1]) > 40, 'the picture is the tablet\'s, whole (' + home + ')');
  check(Boolean(await js("localStorage.getItem('xptablet.lc.token')")), 'the device keeps its token');
  // a finger on the Map in the favorites bar: the tablet in the "sim" opens it, and the picture follows
  const appsBefore = hostLines.filter((l) => /^app: /.test(l)).length;
  await touch(270, 410);
  check(await until(async () => hostLines.filter((l) => /^app: /.test(l)).length > appsBefore, 6000), 'a touch on the device works the tablet (it opened the Map)');
  if (process.env.XPT_LC_TRACE) console.log(hostLines.slice(-12).join(' | '));
  check(await until(async () => (await picture()) !== home), 'and the picture on the device follows');
  await sleep(1500);
  await shot('4_mirror_map');
  // a mouse (a computer's browser) works it too: the logo opens the pull-down
  const before = await picture();
  await click(400, 14);
  check(await until(async () => (await picture()) !== before), 'a mouse click works the tablet too');
  await sleep(1200);
  await shot('5_mirror_pulldown');
  // the full-screen button: shown in the corner (with its icon), above the picture
  check(await until(async () => (await js("(function(){var b=document.getElementById('fs'),r=b.getBoundingClientRect();return getComputedStyle(b).display!=='none'&&r.width>30&&parseFloat(getComputedStyle(b).opacity)>0.4&&!!b.querySelector('svg')})()")) === true), 'the full-screen button shows in the corner');
  // reload: straight back in, no code
  const pairsBefore = hostLines.filter((l) => /^pair: /.test(l)).length;
  await cdp('Page.navigate', { url: `https://127.0.0.1:${port}/` });
  check(await until(async () => (await phase()) === 3), 'reloaded: the paired device comes straight back, with the picture');
  check(hostLines.filter((l) => /^pair: /.test(l)).length === pairsBefore, 'and is not asked for a code again');
  check(problems.length === 0, 'no errors on the page' + (problems.length ? ': ' + problems.slice(0, 4).join(' | ') : ''));
} catch (e) { console.log('FAIL: ' + (e && e.stack || e)); ++failures; }
console.log(failures ? `e2e_localconnect: ${failures} failed` : 'e2e_localconnect: ok');
done(failures ? 1 : 0);
