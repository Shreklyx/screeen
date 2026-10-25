import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import WebSocket from 'ws';
import { createApp, turnCredentials } from '../server/server.js';

let app;
let base;

before(async () => {
  app = createApp({ port: 0, hostPassword: 'titok123', turnSecret: 's3cret', turnHost: 'example.test', turnPort: 3478, trustProxy: false });
  await new Promise((r) => app.server.listen(0, '127.0.0.1', r));
  base = `127.0.0.1:${app.server.address().port}`;
});

after(() => app.close());

/** Opens a signaling socket and collects messages. */
async function client(headers = {}) {
  const ws = new WebSocket(`ws://${base}/ws`, { headers: { Origin: `http://${base}`, ...headers } });
  const inbox = [];
  const waiters = [];
  ws.on('message', (raw) => {
    const msg = JSON.parse(raw.toString());
    const i = waiters.findIndex((w) => w.pred(msg));
    if (i >= 0) waiters.splice(i, 1)[0].resolve(msg);
    else inbox.push(msg);
  });
  await new Promise((resolve, reject) => {
    ws.once('open', resolve);
    ws.once('error', reject);
  });
  return {
    ws,
    send: (m) => ws.send(JSON.stringify(m)),
    next(pred = () => true, ms = 2000) {
      const i = inbox.findIndex(pred);
      if (i >= 0) return Promise.resolve(inbox.splice(i, 1)[0]);
      return new Promise((resolve, reject) => {
        const w = { pred, resolve };
        waiters.push(w);
        setTimeout(() => {
          const k = waiters.indexOf(w);
          if (k >= 0) {
            waiters.splice(k, 1);
            reject(new Error('timeout waiting for message'));
          }
        }, ms);
      });
    },
    nothing(pred = () => true, ms = 300) {
      return new Promise((resolve, reject) => {
        const w = { pred, resolve: (m) => reject(new Error(`unexpected ${JSON.stringify(m)}`)) };
        waiters.push(w);
        setTimeout(() => {
          waiters.splice(waiters.indexOf(w), 1);
          resolve();
        }, ms);
      });
    },
    close: () => ws.close(),
  };
}

const is = (t) => (m) => m.t === t;

test('serves the page with security headers', async () => {
  const res = await fetch(`http://${base}/`);
  assert.equal(res.status, 200);
  assert.match(res.headers.get('content-type'), /text\/html/);
  assert.match(res.headers.get('content-security-policy'), /script-src 'self' 'wasm-unsafe-eval'/); // ss-web.wasm
  assert.match(res.headers.get('permissions-policy'), /display-capture=\(self\)/);
  assert.match(res.headers.get('permissions-policy'), /microphone=\(self\)/); // audio source picker
  assert.equal((await fetch(`http://${base}/app.js`)).status, 200);
  for (const f of ['/direct-web.js', '/direct-audio.js', '/capture-audio.js', '/ss-web.js']) assert.equal((await fetch(`http://${base}${f}`)).status, 200, f);
  const wasm = await fetch(`http://${base}/ss-web.wasm`);
  assert.equal(wasm.headers.get('content-type'), 'application/wasm');
  assert.ok(WebAssembly.validate(new Uint8Array(await wasm.arrayBuffer())));
  assert.equal((await fetch(`http://${base}/../server/server.js`)).status, 404);
  assert.equal((await fetch(`http://${base}/server/server.js`)).status, 404);
  assert.equal((await fetch(`http://${base}/healthz`)).status, 200);
});

test('serves the home-screen app manifest and icons', async () => {
  const manifest = await fetch(`http://${base}/manifest.webmanifest`);
  assert.equal(manifest.status, 200);
  assert.match(manifest.headers.get('content-type'), /application\/manifest\+json/);
  const m = await manifest.json();
  assert.equal(m.display, 'standalone');
  for (const icon of m.icons) {
    const res = await fetch(`http://${base}${icon.src}`);
    assert.equal(res.status, 200, icon.src);
    assert.equal(res.headers.get('content-type'), 'image/png');
  }
  const apple = await fetch(`http://${base}/apple-touch-icon.png`);
  assert.equal(apple.status, 200);
  assert.deepEqual([...new Uint8Array(await apple.arrayBuffer()).slice(1, 4)], [0x50, 0x4e, 0x47]); // "PNG"
});

test('odd request lines do not crash the server', async () => {
  const net = await import('node:net');
  const [host, port] = base.split(':');
  const reply = await new Promise((resolve, reject) => {
    const sock = net.connect(Number(port), host, () => sock.write('GET // HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'));
    let data = '';
    sock.on('data', (d) => { data += d; });
    sock.on('end', () => resolve(data));
    sock.on('error', reject);
  });
  assert.match(reply, /^HTTP\/1\.1 (400|404)/);
  assert.equal((await fetch(`http://${base}/healthz`)).status, 200);
});

test('relay only forwards the expected message types – no WebRTC ones', async () => {
  const h = await client();
  h.send({ t: 'host', password: 'titok123' });
  const { code } = await h.next(is('hosted'));
  const v = await client();
  v.send({ t: 'join', code });
  const { id } = await v.next(is('joined'));
  await h.next(is('viewer'));
  v.send({ t: 'signal', to: 'host', data: { type: 'offer', sdp: 'x' } });
  v.send({ t: 'signal', to: 'host', data: { type: 'direct-answer', pub: 'y'.repeat(40 * 1024) } });
  v.send({ t: 'signal', to: 'host', data: { type: 'answer', sdp: 'y' } });
  v.send({ t: 'signal', to: 'host', data: { type: 'candidate', candidate: {} } });
  h.send({ t: 'signal', to: id, data: { type: 'offer', sdp: 'z' } });
  h.send({ t: 'signal', to: id, data: { type: 'candidate', candidate: {} } });
  h.send({ t: 'signal', to: id, data: { type: 'direct-answer', pub: 'z' } });
  await h.nothing((m) => m.t === 'signal');
  await v.nothing((m) => m.t === 'signal');
  v.send({ t: 'signal', to: 'host', data: { type: 'recall' } });
  assert.deepEqual((await h.next(is('signal'))).data, { type: 'recall' });
  h.send({ t: 'close' });
  h.close();
  v.close();
});

test('rejects WebSocket connections from other sites, accepts the desktop app', async () => {
  await assert.rejects(client({ Origin: 'https://evil.example' }));
  await assert.rejects(client({ Origin: 'app://screenshare.evil' }));
  const app = await client({ Origin: 'app://screenshare' });
  app.close();
});

test('hosting needs the password', async () => {
  const c = await client();
  c.send({ t: 'host', password: 'rossz' });
  assert.deepEqual(await c.next(), { t: 'error', code: 'auth' });
  c.send({ t: 'host' });
  assert.deepEqual(await c.next(), { t: 'error', code: 'auth' });
  c.close();
});

test('full host/viewer flow with relay, state and close', async () => {
  const h = await client();
  h.send({ t: 'host', password: 'titok123' });
  const hosted = await h.next(is('hosted'));
  assert.match(hosted.code, /^[a-z0-9]{6}$/);
  assert.ok(hosted.token.length > 20);
  const turn = hosted.iceServers.find((s) => s.username);
  assert.ok(turn.urls.some((u) => u.startsWith('turn:example.test:3478')));

  const v = await client();
  v.send({ t: 'join', code: hosted.code.toUpperCase(), caps: { H264: { supported: true, hw: true }, junk: 1, surround: true, direct: true } });
  const joined = await v.next(is('joined'));
  assert.equal(joined.hostPresent, true);
  const announced = await h.next(is('viewer'));
  assert.equal(announced.id, joined.id);
  assert.deepEqual(announced.caps, { surround: true, direct: true });
  assert.equal((await v.next(is('count'))).n, 1);

  h.send({ t: 'signal', to: joined.id, data: { type: 'direct-offer', pub: 'x' } });
  assert.deepEqual(await v.next(is('signal')), { t: 'signal', from: 'host', data: { type: 'direct-offer', pub: 'x' } });
  v.send({ t: 'signal', to: 'host', data: { type: 'direct-answer', pub: 'y' } });
  assert.deepEqual(await h.next(is('signal')), { t: 'signal', from: joined.id, data: { type: 'direct-answer', pub: 'y' } });

  h.send({ t: 'state', live: true });
  assert.deepEqual(await v.next(is('state')), { t: 'state', live: true });

  // a second viewer cannot talk to the first one, and viewers cannot change state
  const v2 = await client();
  v2.send({ t: 'join', code: hosted.code, caps: { surround: 'yes' } });
  const joined2 = await v2.next(is('joined'));
  assert.deepEqual((await h.next(is('viewer'))).caps, {});
  v2.send({ t: 'signal', to: joined.id, data: { type: 'direct-offer', pub: 'evil' } });
  v2.send({ t: 'state', live: false });
  await v.nothing((m) => m.t === 'signal' || m.t === 'state');

  v2.close();
  assert.equal((await h.next((m) => m.t === 'viewer-left')).id, joined2.id);

  h.send({ t: 'close' });
  assert.equal((await v.next(is('closed'))).reason, 'closed');
  v.send({ t: 'join', code: hosted.code });
  assert.deepEqual(await v.next(is('error')), { t: 'error', code: 'notfound' });
  h.close();
  v.close();
});

test('host can resume the room with its token after a reconnect', async () => {
  const h = await client();
  h.send({ t: 'host', password: 'titok123' });
  const hosted = await h.next(is('hosted'));
  const v = await client();
  v.send({ t: 'join', code: hosted.code });
  const joined = await v.next(is('joined'));

  h.close();
  await v.next(is('host-away'));

  const h2 = await client();
  h2.send({ t: 'host', resume: { code: hosted.code, token: 'wrong' }, password: 'nope' });
  assert.deepEqual(await h2.next(is('error')), { t: 'error', code: 'auth' });
  h2.send({ t: 'host', resume: { code: hosted.code, token: hosted.token } });
  const resumed = await h2.next(is('hosted'));
  assert.equal(resumed.code, hosted.code);
  assert.deepEqual(resumed.viewers.map((x) => x.id), [joined.id]);
  await v.next(is('host-back'));
  h2.send({ t: 'close' });
  await v.next(is('closed'));
  h2.close();
  v.close();
});

test('unknown room and malformed messages', async () => {
  const c = await client();
  c.send({ t: 'join', code: 'zzzzzz' });
  assert.deepEqual(await c.next(), { t: 'error', code: 'notfound' });
  c.ws.send('not json');
  c.send({ t: 'constructor' });
  c.send({ t: '__proto__' });
  c.send({ t: 'signal', to: 'host', data: 'x' });
  await c.nothing();
  c.close();
});

test('turn credentials follow the TURN REST API scheme', () => {
  const { username, credential } = turnCredentials('secret', 60, 1_000_000, 'abc');
  assert.equal(username, '1060:abc');
  assert.notEqual(turnCredentials('secret').username, turnCredentials('secret').username);
  assert.equal(credential, crypto.createHmac('sha1', 'secret').update(username).digest('base64'));
});

test('password guessing is rate limited per IP', async () => {
  const c = await client();
  let last;
  for (let i = 0; i < 12; i++) {
    c.send({ t: 'host', password: `guess${i}` });
    last = await c.next();
  }
  assert.deepEqual(last, { t: 'error', code: 'rate' });
  c.send({ t: 'host', password: 'titok123' });
  assert.deepEqual(await c.next(), { t: 'error', code: 'rate' });
  c.close();
});

test('direct mode: the host gets a UDP session for an app viewer; it ends when the viewer leaves', async () => {
  // a free UDP port for the relay
  const { default: dgram } = await import('node:dgram');
  const probe = dgram.createSocket('udp4');
  await new Promise((r) => probe.bind(0, '127.0.0.1', r));
  const directPort = probe.address().port;
  probe.close();
  const app2 = createApp({ port: 0, host: '127.0.0.1', hostPassword: 'titok123', turnSecret: '', directPort });
  await new Promise((r) => app2.server.listen(0, '127.0.0.1', r));
  assert.equal(await app2.relayReady, directPort);
  const saved = base;
  base = `127.0.0.1:${app2.server.address().port}`;
  try {
    const h = await client();
    h.send({ t: 'host', password: 'titok123' });
    const { code } = await h.next(is('hosted'));
    const browser = await client();
    browser.send({ t: 'join', code, caps: { direct: 'yes' } });
    const b = await browser.next(is('joined'));
    assert.deepEqual((await h.next(is('viewer'))).caps, {});
    h.send({ t: 'direct', to: b.id });
    await browser.nothing(is('direct'));
    await h.nothing(is('direct'));

    const v = await client();
    v.send({ t: 'join', code, caps: { direct: true } });
    const { id } = await v.next(is('joined'));
    assert.deepEqual((await h.next(is('viewer'))).caps, { direct: true });
    v.send({ t: 'direct', to: id }); // only the host may ask
    await v.nothing(is('direct'));
    h.send({ t: 'direct', to: id });
    const forHost = await h.next(is('direct'));
    const forViewer = await v.next(is('direct'));
    assert.equal(forHost.viewer, id);
    assert.equal(forHost.session, forViewer.session);
    assert.equal(forHost.secret, forViewer.secret);
    assert.equal(forViewer.port, directPort);
    assert.equal(Buffer.from(forViewer.secret, 'base64').length, 16);
    assert.equal(app2.relay.sessions.size, 1);
    // asking again replaces the session
    h.send({ t: 'direct', to: id });
    await h.next(is('direct'));
    await v.next(is('direct'));
    assert.equal(app2.relay.sessions.size, 1);
    // direct signals pass between the two
    h.send({ t: 'signal', to: id, data: { type: 'direct-offer', pub: 'x' } });
    assert.equal((await v.next(is('signal'))).data.type, 'direct-offer');
    v.send({ t: 'signal', to: 'host', data: { type: 'direct-answer', pub: 'y' } });
    assert.equal((await h.next(is('signal'))).data.type, 'direct-answer');
    v.send({ t: 'signal', to: 'host', data: { type: 'direct-fail' } });
    assert.equal((await h.next(is('signal'))).data.type, 'direct-fail');

    v.close();
    await h.next(is('viewer-left'));
    assert.equal(app2.relay.sessions.size, 0);
    h.send({ t: 'close' });
    h.close();
    browser.close();
  } finally {
    base = saved;
    await app2.close();
  }
});
