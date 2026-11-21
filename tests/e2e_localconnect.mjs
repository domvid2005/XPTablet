// Local Connect, end to end: a real browser (headless Chrome) opens the page the real server serves, pairs with the code the
// stand-in host prints (as the sim would show it), and uses the tablet; the host must see what was asked.
//   node tests/e2e_localconnect.mjs <local_host> <index.html.gz> <chrome> <out-dir>
import { spawn } from 'node:child_process';
import { mkdtempSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [hostBin, page, chromeBin, outDir] = process.argv.slice(2);
mkdirSync(outDir, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { if (!ok) { console.log('FAIL: ' + what); ++failures; } else console.log('ok: ' + what); };

const port = 18543, dbg = 9333;
const hostLines = [];
const host = spawn(hostBin, [page, mkdtempSync(join(tmpdir(), 'xpt-lc-data-')), String(port)]);
host.stdout.on('data', (d) => { for (const l of String(d).split('\n')) if (l) { hostLines.push(l); } });
const hostSaid = async (re, ms = 15000) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { const l = hostLines.find((x) => re.test(x)); if (l) return l; await sleep(50); } return null; };

const chrome = spawn(chromeBin, ['--headless=new', '--no-sandbox', '--disable-gpu-sandbox', '--ignore-certificate-errors', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
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
    if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error' && /^https:\/\/127\.0\.0\.1/.test(m.params.entry.url || 'https://127.0.0.1') && !/favicon/.test(m.params.entry.url || '')) problems.push('log: ' + m.params.entry.text + ' ' + (m.params.entry.url || ''));
  });
  const cdp = (method, params = {}) => new Promise((r) => { const id = nextId++; waiting.set(id, r); ws.send(JSON.stringify({ id, method, params })); });
  const js = async (expr) => (await cdp('Runtime.evaluate', { expression: expr, returnByValue: true })).result?.value;
  const shot = async (name) => { const r = await cdp('Page.captureScreenshot', { format: 'png' }); writeFileSync(join(outDir, name + '.png'), Buffer.from(r.data, 'base64')); };
  const phase = () => js("window.Module && typeof Module._lc_phase === 'function' && Module.calledRun ? Module._lc_phase() : -1");
  const until = async (fn, ms = 20000) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (await fn()) return true; await sleep(100); } return false; };
  const tap = async (x, y) => { await js(`Module._lc_mouse(${x},${y},-1); Module._lc_mouse(${x},${y},1)`); await sleep(120); await js(`Module._lc_mouse(${x},${y},0)`); await sleep(160); await js('Module._lc_mouse(-1000,-1000,-1)'); await sleep(60); };

  await cdp('Runtime.enable'); await cdp('Log.enable'); await cdp('Page.enable');
  await cdp('Emulation.setDeviceMetricsOverride', { width: 800, height: 480, deviceScaleFactor: 2, mobile: true });
  await cdp('Page.navigate', { url: `https://127.0.0.1:${port}/` });

  check(await until(async () => (await phase()) === 1, 120000), 'the page loads over HTTPS, connects, and asks for the pairing code');
  const pair = await hostSaid(/^pair: \d{6} /);
  check(pair, 'the code is shown on the computer');
  await sleep(300);
  await shot('1_pairing');
  // a wrong code first, then the right one, on the page's own keys
  const key = (d) => { const i = d === 0 ? 10 : d - 1; return [254 + (i % 3) * 100 + 46, 196 + Math.floor(i / 3) * 50 + 21]; };
  const code = pair.split(' ')[1], wrong = code === '111111' ? '222222' : '111111';
  for (const ch of wrong) await tap(...key(Number(ch)));
  await sleep(500);
  check((await phase()) === 1 && !hostLines.some((l) => /^devices: 1/.test(l)), 'a wrong code pairs nothing');
  await shot('2_wrong_code');
  for (const ch of code) await tap(...key(Number(ch)));
  check(await until(async () => (await phase()) === 3), 'the right code: the tablet comes up');
  check(await hostSaid(/^devices: 1/), 'the computer lists the device as connected');
  await sleep(1200);
  await shot('3_live_home');
  check(Boolean(await js("localStorage.getItem('xptablet.lc.token')")), 'the device keeps its token');
  // use it: the Map from the dock (the computer hears which page is shown, and feeds the map)
  await tap(270, 410);
  check(await hostSaid(/^level: map shown/), 'opening the Map on the device reaches the computer');
  await sleep(2500);
  await shot('4_map');
  // a setting changed on the device (the brightness, in the pull-down from the logo) becomes the computer's
  await tap(400, 14);
  await sleep(900);
  await tap(60, 341);
  check(await hostSaid(/^settings: changed by a device/), 'a setting changed on the device reaches the computer');
  await shot('4b_pulldown');
  // reload: straight back in, no code
  const pairsBefore = hostLines.filter((l) => /^pair: /.test(l)).length;
  await cdp('Page.navigate', { url: `https://127.0.0.1:${port}/` });
  check(await until(async () => (await phase()) === 3, 120000), 'reloaded: the paired device comes straight back');
  check(hostLines.filter((l) => /^pair: /.test(l)).length === pairsBefore, 'and is not asked for a code again');
  await sleep(800);
  await shot('5_back_again');
  check(problems.length === 0, 'no errors on the page' + (problems.length ? ': ' + problems.slice(0, 4).join(' | ') : ''));
} catch (e) { console.log('FAIL: ' + (e && e.stack || e)); ++failures; }
console.log(failures ? `e2e_localconnect: ${failures} failed` : 'e2e_localconnect: ok');
done(failures ? 1 : 0);
