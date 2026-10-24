// Direct mode transport (desktop/direct.js) against the server's UDP relay (server/direct-relay.js).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import crypto from 'node:crypto';
import { createRelay } from '../server/direct-relay.js';

const require = createRequire(import.meta.url);
const { DirectEndpoint, MAX_DATA, _test: t } = require('../desktop/direct.js');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (cond, ms = 5000) => {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) throw new Error('timeout');
    await sleep(10);
  }
};

test('both sides derive the same keys; packets only open with the right key and direction', () => {
  const a = t.keyPair();
  const b = t.keyPair();
  const ka = t.deriveKeys(a.privateKey, b.pub, 1234);
  const kb = t.deriveKeys(b.privateKey, a.pub, 1234);
  assert.deepEqual(ka, kb);
  assert.notDeepEqual(ka.toViewer, ka.toHost);
  const payload = Buffer.from('hello');
  const p = t.seal(ka.toViewer, 1, t.T.MEDIA, 1234, 7n, payload);
  assert.deepEqual(t.unseal(kb.toViewer, 1, p), payload);
  assert.equal(t.unseal(kb.toViewer, 2, p), null, 'wrong direction');
  assert.equal(t.unseal(kb.toHost, 1, p), null, 'wrong key');
  const tampered = Buffer.from(p);
  tampered[2] ^= 1; // session id is authenticated
  assert.equal(t.unseal(kb.toViewer, 1, tampered), null);
  assert.throws(() => t.deriveKeys(a.privateKey, 'AAAA', 1));
});

test('candidate validation and private ranges', () => {
  assert.ok(t.validCandidate({ address: '192.168.1.5', port: 5000 }));
  assert.ok(!t.validCandidate({ address: '999.1.1.1', port: 5000 }));
  assert.ok(!t.validCandidate({ address: '224.0.0.1', port: 5000 }), 'multicast');
  assert.ok(!t.validCandidate({ address: '0.0.0.0', port: 5000 }));
  assert.ok(!t.validCandidate({ address: '10.0.0.1', port: 70000 }));
  assert.ok(!t.validCandidate({ address: 'example.com', port: 1 }));
  assert.ok(t.isPrivate('10.1.2.3') && t.isPrivate('172.20.0.1') && t.isPrivate('192.168.0.9') && t.isPrivate('100.70.0.1'));
  assert.ok(!t.isPrivate('94.156.37.47') && !t.isPrivate('172.32.0.1'));
});

test('relay: registration answers with the seen address, only with the right secret', async () => {
  const relay = createRelay({ port: 0, host: '127.0.0.1' });
  const port = await relay.listen();
  const { session, secret } = relay.create({ room: 'r', viewer: 'v1' });
  const key = Buffer.from(secret, 'base64');
  const { default: dgram } = await import('node:dgram');
  const sock = dgram.createSocket('udp4');
  await new Promise((r) => sock.bind(0, '127.0.0.1', r));
  const answers = [];
  sock.on('message', (m) => answers.push(m));
  sock.send(t.registration('viewer', session, crypto.randomBytes(16)), port, '127.0.0.1'); // wrong secret
  await sleep(100);
  assert.equal(answers.length, 0);
  sock.send(t.registration('viewer', session, key), port, '127.0.0.1');
  await until(() => answers.length === 1);
  const ack = t.parseRegistrationAck(answers[0], (s) => (s === session ? key : null));
  assert.deepEqual(ack, { session, address: '127.0.0.1', port: sock.address().port });
  assert.ok(answers[0].length <= 64, 'answer not larger than the question');
  assert.equal(t.parseRegistrationAck(answers[0], () => crypto.randomBytes(16)), null);
  sock.close();
  relay.close();
});

async function pair({ forceRelay = false, drop = null } = {}) {
  const relay = createRelay({ port: 0, host: '127.0.0.1' });
  const port = await relay.listen();
  const { session, secret } = relay.create({ room: 'r', viewer: 'v1' });
  const host = new DirectEndpoint({ role: 'host', serverHost: '127.0.0.1', forceRelay });
  const viewer = new DirectEndpoint({ role: 'viewer', serverHost: '127.0.0.1', forceRelay });
  await host.open();
  await viewer.open();
  if (drop) {
    // lose some of the sharer's packets on the way
    const send = host.socket.send.bind(host.socket);
    host.socket.send = (msg, ...rest) => (drop(msg) ? undefined : send(msg, ...rest));
  }
  const states = { host: [], viewer: [] };
  host.on('state', (id, s) => states.host.push(s));
  viewer.on('state', (id, s) => states.viewer.push(s));
  const offer = await host.addPeer('v1', { session, secret, port });
  const answer = await viewer.addPeer('host', { session, secret, port });
  assert.ok(answer.cands.length >= (forceRelay ? 0 : 1));
  viewer.connectPeer('host', offer);
  host.connectPeer('v1', answer);
  await until(() => states.host.some((s) => s.state === 'connected') && states.viewer.some((s) => s.state === 'connected'));
  const close = () => {
    host.close();
    viewer.close();
    relay.close();
  };
  return { relay, host, viewer, states, close };
}

function frame(n, size) {
  const b = Buffer.alloc(size);
  b.writeUInt32BE(n, 0);
  for (let i = 4; i < size; i++) b[i] = (n + i) & 0xff;
  return b;
}

async function stream(host, viewer, { frames = 40, size = 30000, keyEvery = 0 } = {}) {
  const got = [];
  viewer.on('media', (meta, data) => {
    if (meta.stream === 0) got.push({ n: data.readUInt32BE(0), key: meta.key, ok: data.equals(frame(data.readUInt32BE(0), data.length)), capture: meta.capture });
  });
  for (let n = 0; n < frames; n++) {
    host.sendMedia({ stream: 0, key: n === 0 || (keyEvery && n % keyEvery === 0), ts: n * 16667, capture: n }, frame(n, size + (n % 7) * 100));
    await sleep(5);
  }
  return got;
}

test('direct path: frames arrive whole and in order', async () => {
  const { host, viewer, states, close } = await pair();
  try {
    assert.ok(['lan', 'internet'].includes(states.viewer.at(-1).path), `path ${states.viewer.at(-1).path}`);
    const got = await stream(host, viewer, { frames: 40, size: 5 * MAX_DATA + 17 });
    await until(() => got.length === 40);
    assert.deepEqual(got.map((g) => g.n), [...Array(40).keys()]);
    assert.ok(got.every((g) => g.ok));
    // audio goes as it is
    const audio = [];
    viewer.on('media', (meta, data) => { if (meta.stream === 1) audio.push([meta.sub, data.toString()]); });
    host.sendMedia({ stream: 1, sub: 2, ts: 5, capture: 1 }, Buffer.from('opus'));
    await until(() => audio.length === 1);
    assert.deepEqual(audio[0], [2, 'opus']);
  } finally {
    close();
  }
});

test('lost packets are sent again (NACK) – no frame is skipped', async () => {
  let n = 0;
  let resending = false;
  // every 9th media packet is lost the first time it is sent
  const { host, viewer, close } = await pair({ drop: (msg) => !resending && msg[0] === 0x53 && msg[1] === t.T.MEDIA && ++n % 9 === 0 && n < 400 });
  const onNack = host.onNack.bind(host);
  host.onNack = (...args) => {
    resending = true;
    try {
      onNack(...args);
    } finally {
      resending = false;
    }
  };
  try {
    const got = await stream(host, viewer, { frames: 30, size: 8 * MAX_DATA });
    await until(() => got.length === 30);
    assert.deepEqual(got.map((g) => g.n), [...Array(30).keys()]);
    assert.ok(got.every((g) => g.ok));
  } finally {
    close();
  }
});

test('a frame that cannot be completed is skipped; the viewer asks for a keyframe and goes on from it', async () => {
  const { host, viewer, close } = await pair();
  // the first packet of frame 5 never arrives, retransmissions included
  const enqueue = host.enqueue.bind(host);
  host.enqueue = (peer, payload, urgent) => {
    if (payload[0] === 0 && payload.readUInt32BE(8) === 5 && payload.readUInt16BE(12) === 0) return;
    enqueue(peer, payload, urgent);
  };
  // (a request before frame 0 arrived – a busy test machine – is not the one this is about)
  let answered = false;
  host.on('keyframe', () => {
    if (answered || host.videoFrame < 8) return;
    answered = true;
    host.sendMedia({ stream: 0, key: true, ts: 0, capture: 0 }, frame(100, 2 * MAX_DATA));
  });
  try {
    const got = await stream(host, viewer, { frames: 15, size: 3 * MAX_DATA });
    await until(() => got.some((g) => g.n === 100));
    const ns = got.map((g) => g.n);
    // 0–4, then nothing until the keyframe (frames sent after it decode again)
    assert.deepEqual(ns.slice(0, 6), [0, 1, 2, 3, 4, 100], `${ns}`);
    assert.ok(!ns.includes(5));
    assert.ok(got.find((g) => g.n === 100).key);
  } finally {
    close();
  }
});

test('relay: works when only the server can be reached', async () => {
  const { host, viewer, states, relay, close } = await pair({ forceRelay: true });
  try {
    assert.equal(states.viewer.at(-1).path, 'relay');
    assert.equal(states.host.at(-1).path, 'relay');
    const got = await stream(host, viewer, { frames: 20, size: 4 * MAX_DATA });
    await until(() => got.length === 20);
    assert.ok(got.every((g) => g.ok));
    const s = [...relay.sessions.values()][0];
    assert.ok(s.relayed > 20 * 4 * MAX_DATA, `relayed ${s.relayed}`);
  } finally {
    close();
  }
});

test('clock: the viewer learns the round trip and the clock offset', async () => {
  const { viewer, close } = await pair();
  try {
    const clocks = [];
    viewer.on('clock', (id, c) => clocks.push(c));
    await until(() => clocks.length >= 2, 3000);
    assert.ok(clocks.at(-1).rtt >= 0 && clocks.at(-1).rtt < 50);
    assert.ok(Math.abs(clocks.at(-1).offset) < 20, `offset ${clocks.at(-1).offset}`);
  } finally {
    close();
  }
});
