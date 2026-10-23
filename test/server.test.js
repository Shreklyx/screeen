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
  assert.match(res.headers.get('content-security-policy'), /script-src 'self'/);
  assert.match(res.headers.get('permissions-policy'), /display-capture=\(self\)/);
  assert.match(res.headers.get('permissions-policy'), /microphone=\(self\)/); // audio source picker
  assert.equal((await fetch(`http://${base}/app.js`)).status, 200);
  assert.equal((await fetch(`http://${base}/../server/server.js`)).status, 404);
  assert.equal((await fetch(`http://${base}/server/server.js`)).status, 404);
  assert.equal((await fetch(`http://${base}/healthz`)).status, 200);
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

test('relay only forwards the expected message types', async () => {
  const h = await client();
  h.send({ t: 'host', password: 'titok123' });
  const { code } = await h.next(is('hosted'));
  const v = await client();
  v.send({ t: 'join', code });
  const { id } = await v.next(is('joined'));
  await h.next(is('viewer'));
  v.send({ t: 'signal', to: 'host', data: { type: 'offer', sdp: 'x' } });
  v.send({ t: 'signal', to: 'host', data: { type: 'answer', sdp: 'y'.repeat(40 * 1024) } });
  h.send({ t: 'signal', to: id, data: { type: 'answer', sdp: 'z' } });
  await h.nothing((m) => m.t === 'signal');
  await v.nothing((m) => m.t === 'signal');
  v.send({ t: 'signal', to: 'host', data: { type: 'recall' } });
  assert.deepEqual((await h.next(is('signal'))).data, { type: 'recall' });
  h.send({ t: 'close' });
  h.close();
  v.close();
});

test('rejects WebSocket connections from other sites', async () => {
  await assert.rejects(client({ Origin: 'https://evil.example' }));
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
  v.send({ t: 'join', code: hosted.code.toUpperCase(), caps: { H264: { supported: true, hw: true }, junk: 1 } });
  const joined = await v.next(is('joined'));
  assert.equal(joined.hostPresent, true);
  const announced = await h.next(is('viewer'));
  assert.equal(announced.id, joined.id);
  assert.deepEqual(announced.caps, { H264: { supported: true, hw: true } });
  assert.equal((await v.next(is('count'))).n, 1);

  h.send({ t: 'signal', to: joined.id, data: { type: 'offer', sdp: 'x' } });
  assert.deepEqual(await v.next(is('signal')), { t: 'signal', from: 'host', data: { type: 'offer', sdp: 'x' } });
  v.send({ t: 'signal', to: 'host', data: { type: 'answer', sdp: 'y' } });
  assert.deepEqual(await h.next(is('signal')), { t: 'signal', from: joined.id, data: { type: 'answer', sdp: 'y' } });

  h.send({ t: 'state', live: true });
  assert.deepEqual(await v.next(is('state')), { t: 'state', live: true });

  // a second viewer cannot talk to the first one, and viewers cannot change state
  const v2 = await client();
  v2.send({ t: 'join', code: hosted.code });
  const joined2 = await v2.next(is('joined'));
  v2.send({ t: 'signal', to: joined.id, data: { type: 'offer', sdp: 'evil' } });
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
