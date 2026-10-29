// Direct mode audio playback worklet (public/direct-audio.js), run with a minimal AudioWorklet shim.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

function load() {
  const registered = {};
  const context = {
    AudioWorkletProcessor: class { constructor() { this.port = {}; } },
    registerProcessor: (name, cls) => { registered[name] = cls; },
    Math,
    Float32Array,
    Array,
  };
  vm.runInNewContext(fs.readFileSync(new URL('../public/direct-audio.js', import.meta.url), 'utf8'), context);
  return registered['direct-audio'];
}

const Processor = load();
const quantum = (channels) => [Array.from({ length: channels }, () => new Float32Array(128))];

/** n 5 ms packets of a constant value per channel, written pair by pair like the decoders do. */
function feed(p, channels, values, fromMs, n) {
  for (let k = 0; k < n; k++) {
    const ts = (fromMs + k * 5) * 1000;
    for (let pair = 0; pair * 2 < channels; pair++) {
      const planes = [];
      for (let c = 0; c < Math.min(2, channels - pair * 2); c++) planes.push(new Float32Array(240).fill(values[pair * 2 + c]));
      p.write({ pair, ts, frames: 240, planes });
    }
  }
}

test('7.1 on 8 speakers: every channel in its place', () => {
  const p = new Processor({ processorOptions: { channels: 8 } });
  const values = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8];
  feed(p, 8, values, 0, 20);
  const out = quantum(8);
  p.process([], out);
  // (past the first 2 ms: what arrived at once beyond the wait is played past, faded in)
  assert.deepEqual(out[0].map((ch) => Math.round(ch[120] * 10) / 10), values);
});

test('7.1 on stereo: centre and surrounds mixed in, LFE left out', () => {
  const p = new Processor({ processorOptions: { channels: 8 } });
  feed(p, 8, [0, 0, 0.2, 0.9, 0.1, 0, 0.1, 0], 0, 20); // C, LFE, BL, SL
  const out = quantum(2);
  p.process([], out);
  const k = Math.SQRT1_2;
  assert.ok(Math.abs(out[0][0][120] - k * (0.2 + 0.1 + 0.1)) < 1e-6, `left ${out[0][0][120]}`);
  assert.ok(Math.abs(out[0][1][120] - k * 0.2) < 1e-6, `right ${out[0][1][120]}`);
});

test('a lost packet of one pair stays silent there, the other pairs keep playing', () => {
  const p = new Processor({ processorOptions: { channels: 4 } });
  // packets arrive in real time, 20 ms ahead of playback; pair 1 loses packet 6 (30–35 ms)
  const seen = [];
  let next = 0;
  for (let q = 0; q < 30; q++) {
    while (next * 5 <= (q * 128) / 48 + 20) {
      const ts = next * 5000;
      p.write({ pair: 0, ts, frames: 240, planes: [new Float32Array(240).fill(0.5), new Float32Array(240).fill(0.5)] });
      if (next !== 6) p.write({ pair: 1, ts, frames: 240, planes: [new Float32Array(240).fill(0.25), new Float32Array(240).fill(0.25)] });
      next++;
    }
    const out = quantum(4);
    p.process([], out);
    seen.push(...Array.from(out[0][2], (x, i) => [p.read - 128 + i, x, out[0][0][i]]));
  }
  const at = (abs) => seen.find(([a]) => a === abs);
  assert.equal(at(1400)[1], 0.25, 'packet 5');
  assert.equal(at(1500)[1], 0, 'packet 6 lost: silent');
  assert.equal(at(1500)[2], 0.5, 'pair 0 plays on');
  assert.equal(at(1700)[1], 0.25, 'packet 7');
});

test('running dry grows the buffer; late packets are dropped, not played out of time', () => {
  const p = new Processor({ processorOptions: { channels: 2 } });
  feed(p, 2, [0.3, 0.3], 0, 4); // 20 ms
  const before = p.target;
  for (let q = 0; q < 20; q++) p.process([], quantum(2)); // plays past the end (~35 ms dry)
  feed(p, 2, [0.3, 0.3], 20, 1); // the network's gap is over
  assert.ok(p.target > before, 'target grew');
  const read = p.read;
  feed(p, 2, [0.9, 0.9], -100, 1); // long gone
  assert.equal(p.read, read);
  const out = quantum(2);
  p.process([], out);
  assert.ok(!out[0][0].includes(0.9));
});

/** Plays `ms` (on average: whole quanta, as the device asks); returns what came out of channel 0. */
function play(p, ms) {
  const got = [];
  p.owed = (p.owed ?? 0) + ms * 48;
  for (; p.owed >= 128; p.owed -= 128) {
    const out = quantum(2);
    p.process([], out);
    got.push(...out[0][0]);
  }
  return got;
}

test('a pause of the sharer (nothing sent for long) does not make the wait longer', () => {
  const p = new Processor({ processorOptions: { channels: 2 } });
  feed(p, 2, [0.3, 0.3], 0, 4);
  play(p, 20);
  const before = p.target;
  play(p, 2000); // nothing plays there: nothing comes
  feed(p, 2, [0.5, 0.5], 2020, 4);
  assert.equal(p.target, before);
  assert.ok(play(p, 30).includes(0.5), 'the sound is back');
});

test('the sharer\'s time going back (it started again): the sound comes back at once, not when its time catches up', () => {
  const p = new Processor({ processorOptions: { channels: 2 } });
  // real time: packets as they are played
  let ms = 10000;
  for (let i = 0; i < 40; i++, ms += 5) {
    feed(p, 2, [0.3, 0.3], ms, 1);
    play(p, 5);
  }
  // the sharer again, its clock 10 s earlier
  const got = [];
  for (let i = 0, t = 0; i < 60; i++, t += 5) {
    feed(p, 2, [0.75, 0.75], t, 1);
    got.push(...play(p, 5));
  }
  const first = got.indexOf(0.75);
  assert.ok(first >= 0, 'plays again');
  assert.ok(first < 48 * 150, `within 150 ms (${(first / 48).toFixed(0)} ms)`);
});

test('a delay the network once caused does not stay: what is more than the wait needs is played past', () => {
  const p = new Processor({ processorOptions: { channels: 2 } });
  // 40 ms more than the wait arrive at once (a burst after a hold-up), then packets in real time
  feed(p, 2, [0.3, 0.3], 0, 8);
  let ms = 40;
  for (let i = 0; i < 300; i++, ms += 5) {
    feed(p, 2, [0.3, 0.3], ms, 1);
    play(p, 5);
  }
  const buffered = (p.end - p.read) / 48;
  assert.ok(buffered <= p.target / 48 + 8, `buffered ${buffered.toFixed(1)} ms, the wait ${(p.target / 48).toFixed(1)} ms`);
});

test('playing past is crossfaded: no sample jumps', () => {
  const p = new Processor({ processorOptions: { channels: 2 } });
  // a slow sine, 60 ms ahead of what is needed, then in real time
  const sine = (ms, i) => Math.sin(((ms * 48 + i) / 48000) * 2 * Math.PI * 50);
  const write = (ms) => p.write({ pair: 0, ts: ms * 1000, frames: 240, planes: [0, 1].map(() => Float32Array.from({ length: 240 }, (_, i) => sine(ms, i))) });
  for (let ms = 0; ms < 70; ms += 5) write(ms);
  const got = [];
  for (let i = 0, ms = 70; i < 200; i++, ms += 5) {
    write(ms);
    got.push(...play(p, 5));
  }
  let worst = 0;
  for (let i = 1; i < got.length; i++) if (got[i - 1] !== 0 && got[i] !== 0) worst = Math.max(worst, Math.abs(got[i] - got[i - 1]));
  assert.ok(worst < 0.05, `largest step ${worst.toFixed(3)}`);
});
