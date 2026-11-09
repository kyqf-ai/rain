// Runs web/index.html's script against a live server with a minimal DOM stub.
// Catches runtime errors (undefined vars, null access) and checks what the page would render.
const fs = require('fs');
const base = process.argv[2];
const html = fs.readFileSync(__dirname + '/../web/index.html', 'utf8');
const script = html.slice(html.indexOf('<script>') + 8, html.indexOf('</script>'));

const registry = {}, canvases = [];
function ctxFor() {
  const calls = { fillRect: 0, fillText: 0, texts: [] };
  const p = new Proxy({}, {
    get: (t, k) => k === '_calls' ? calls : k === 'measureText' ? () => ({ width: 10 }) :
      k === 'fillRect' ? () => { calls.fillRect++; } : k === 'fillText' ? (s) => { calls.fillText++; calls.texts.push(s); } : () => {},
    set: () => true });
  return p;
}
function mkEl(tag) {
  const e = { tag, children: [], style: {}, dataset: {}, value: '', href: '', className: '', title: '',
    clientWidth: 800, clientHeight: 280, classList: { toggle() {}, add() {}, remove() {} },
    appendChild(c) { this.children.push(c); return c; }, querySelectorAll() { return []; },
    getBoundingClientRect() { return { left: 0 }; } };
  let _t = ''; Object.defineProperty(e, 'textContent', { get() { return _t; }, set(v) { _t = v; this.children = []; } });
  e.getContext = () => (e._ctx || (e._ctx = ctxFor()));
  if (tag === 'canvas') canvases.push(e);
  return e;
}
const documentStub = {
  querySelector(s) { return registry[s] || (registry[s] = Object.assign(mkEl(s.startsWith('#c') && /^#c(Day|Min|Q)$/.test(s) ? 'canvas' : 'div'), { id: s })); },
  querySelectorAll() { return []; }, createElement: mkEl, documentElement: {} };
const errors = [];
process.on('unhandledRejection', e => errors.push('unhandled rejection: ' + (e && e.stack || e)));
const fetchAbs = (u, o) => fetch(base + u, o);
const body = script + '\nreturn {loadDay,loadQuery,setRange,loadStatus,renderStatus,loadMinutes,shiftDay,drawDay};';
const api = new Function('document', 'window', 'fetch', 'getComputedStyle', 'setInterval', body)(
  documentStub, { devicePixelRatio: 1, addEventListener() {} }, fetchAbs,
  () => ({ getPropertyValue: () => '#123456' }), () => 0);

let fails = 0;
const check = (c, m) => { if (!c) { fails++; console.log('FAIL:', m); } };
const txt = s => (registry[s] || {}).textContent;
(async () => {
  await new Promise(r => setTimeout(r, 800));                 // initial loadStatus/loadDay from the script itself
  await api.loadDay();
  check(/^\d+(\.\d+)?$/.test(txt('#cTotal')), 'day total rendered: ' + txt('#cTotal'));
  check(registry['#tDay'].children.length === 24, 'day table has 24 rows: ' + registry['#tDay'].children.length);
  check(/^\d+ \/ \d+$/.test(txt('#cCov')), 'coverage card: ' + txt('#cCov'));
  const day = registry['#cDay'].getContext()._calls;
  check(day.fillText >= 24, 'day chart labels drawn: ' + day.fillText);
  await api.shiftDay(-1);
  await new Promise(r => setTimeout(r, 500));
  check(registry['#tDay'].children.length === 24, 'yesterday table has 24 rows');
  check(registry['#tDay'].children.some(tr => tr.children[3] && tr.children[3].textContent === '已定稿'), 'yesterday has finalized hours');
  check(registry['#cDay'].getContext()._calls.fillRect > 0, 'yesterday bars drawn');
  // minute detail for the first hour with rain
  const rows = registry['#tDay'].children; let h = rows.findIndex(tr => tr.children[1].className === 'rain');
  if (h >= 0) { await api.loadMinutes(h); check(/分钟明细（合计/.test(txt('#minTitle')), 'minute title: ' + txt('#minTitle')); }
  else console.log('note: yesterday had no rain hour, minute view not exercised');
  await api.setRange('month'); await new Promise(r => setTimeout(r, 500));
  check(registry['#qCards'].children.length === 6, 'six summary cards: ' + registry['#qCards'].children.length);
  check(registry['#tQ'].children.length >= 1, 'query rows: ' + registry['#tQ'].children.length);
  check(registry['#qCsv'].href.startsWith('/export.csv?from='), 'csv link: ' + registry['#qCsv'].href);
  registry['#qBy'].value = 'hour'; registry['#qFrom'].value = registry['#qTo'].value; await api.loadQuery();
  check(registry['#tQ'].children.length >= 1, 'hourly query rows');
  await api.loadStatus(); api.renderStatus();
  check(registry['#tStat'].children.length === 15, 'status rows: ' + registry['#tStat'].children.length);
  check(txt('#badge') === '在线', 'badge: ' + txt('#badge'));
  check(errors.length === 0, errors.join('\n'));
  console.log('ui smoke: ' + (fails ? fails + ' FAILED' : 'ok'));
  process.exit(fails ? 1 : 0);
})().catch(e => { console.log('FAIL: exception', e.stack); process.exit(1); });
