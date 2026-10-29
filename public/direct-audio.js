'use strict';

// Direct mode (app → app) audio playback: the decoded Opus packets are written into a ring by
// their timestamps (each channel pair arrives separately) and played with a small buffer that
// grows only when the network makes it run dry, and slowly shrinks again; what the buffer holds
// beyond that is played past, so no delay stays. Same as native/common/audio_ring.cpp.
//
// Input channel order (7.1): L R C LFE BL BR SL SR. Output: as many channels as the speakers
// have (8, 6 or 2); fewer speakers get a downmix (centre and surrounds −3 dB, no LFE).

const RATE = 48000;
const SIZE = RATE; // one second
const MIN_TARGET = 240; // 5 ms
const MAX_TARGET = 4800; // 100 ms
const FADE = 96; // 2 ms: playing past what is too much is crossfaded
const WINDOW = RATE / 2; // the buffer's lowest level is looked at every half second
const K = Math.SQRT1_2;

const soft = (x) => (x > 0.9 ? 0.9 + 0.1 * Math.tanh((x - 0.9) / 0.1) : x < -0.9 ? -0.9 + 0.1 * Math.tanh((x + 0.9) / 0.1) : x);

class DirectAudio extends AudioWorkletProcessor {
  constructor({ processorOptions }) {
    super();
    this.channels = Math.max(1, Math.min(8, processorOptions.channels | 0));
    this.ring = Array.from({ length: this.channels }, () => new Float32Array(SIZE));
    this.read = null; // absolute sample index of the next output sample
    this.end = -Infinity; // one past the newest written sample
    this.target = 480; // 10 ms to start with
    this.calm = 0; // samples played since the last underrun
    this.since = -1; // samples since it ran dry (-1: long enough ago)
    this.dryLen = 0; // for how long it was dry
    this.dryEnd = 0; // where the sound ended then
    this.dryTarget = this.target; // the wait then
    this.behind = 0; // packets far too late in a row
    this.low = Infinity; // the lowest the buffer got in this window
    this.lowFor = 0;
    this.port.onmessage = (e) => this.write(e.data);
  }

  write({ pair, ts, frames, planes }) {
    const at = Math.round((ts * RATE) / 1e6);
    if (this.read == null) this.read = at - this.target;
    if (at + frames <= this.read) {
      // too late, already played – but many far behind in a row: the sharer's time went back (it
      // started again), so from these on (before, the sound stayed away until its time caught up)
      if (at + frames > this.read - MAX_TARGET - RATE / 10 || ++this.behind < 20) return;
      this.restart(at);
      this.since = -1;
    }
    this.behind = 0;
    if (this.since >= 0) {
      // sound after it ran dry (while dry and a little after: a burst can come out of order) – how
      // late? One from inside the gap (the network held it back) is as late as it is into the gap: the
      // wait grows by that much (the one that waited longest counts). One from after the gap (nothing
      // was sent – the sharer had nothing to play – or it was lost) is not late: no reason to wait
      // longer (before, a pause made the wait 100 ms for minutes)
      const late = Math.min(this.since, this.dryEnd + this.since - at);
      if (late > 96) this.target = Math.max(this.target, Math.min(MAX_TARGET, this.dryTarget + late));
    }
    if (at - this.read > SIZE - 2048) this.restart(at); // far ahead (clock jump): start over
    planes.forEach((plane, c) => {
      const ring = this.ring[pair * 2 + c];
      if (!ring) return;
      for (let i = 0; i < frames; i++) {
        const a = at + i;
        if (a >= this.read) ring[((a % SIZE) + SIZE) % SIZE] = plane[i];
      }
    });
    if (at + frames > this.end) this.end = at + frames;
  }

  restart(at) {
    for (const r of this.ring) r.fill(0);
    this.read = at - this.target;
    this.end = at;
    this.low = Infinity;
    this.lowFor = 0;
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    if (this.read == null) return true;
    const buffered = this.end - this.read;
    if (buffered < n) {
      // ran dry: since when, and where the sound ended, is kept (the packets that come decide what it was)
      if (this.since < 0) {
        this.since = this.dryLen = 0;
        this.dryEnd = this.end;
        this.dryTarget = this.target;
      }
      this.since += n;
      this.dryLen += n;
      this.calm = 0;
      this.low = Infinity;
      this.lowFor = 0;
      if (buffered <= 0) this.read = this.end - this.target;
      return true; // silence
    }
    if (this.since >= 0 && (this.since += n) > this.dryLen + 2400) this.since = -1; // (50 ms on, what comes is not from the gap)
    // more than the wait needs – a burst after the network held packets back, the clocks' drift, a
    // wait that shrank – is played past: at once when much, else what the buffer never went below
    // in the last half second
    let skip = 0;
    this.low = Math.min(this.low, buffered);
    this.lowFor += n;
    if (buffered > this.target + 2400) skip = buffered - this.target;
    else if (this.lowFor >= WINDOW && this.low > this.target + 240) skip = this.low - this.target;
    if (skip > 0 || this.lowFor >= WINDOW) {
      // (a new window: what was before a skip says nothing now)
      this.low = Infinity;
      this.lowFor = 0;
    }
    skip = Math.max(0, Math.min(skip, buffered - n));
    this.calm += n;
    if (this.calm > RATE * 3 && this.target > MIN_TARGET) {
      this.target -= 48;
      this.calm = 0;
    }
    const ch = this.channels;
    const outs = out.length;
    const ring = this.ring;
    const wrap = (a) => ((a % SIZE) + SIZE) % SIZE;
    for (let i = 0; i < n; i++) {
      const idx = wrap(this.read + skip + i);
      let s = (c) => (c < ch ? ring[c][idx] : 0);
      if (skip && i < FADE) {
        // (from where it was to where it goes, without a click)
        const from = wrap(this.read + i);
        const w = (i + 0.5) / FADE;
        s = (c) => (c < ch ? ring[c][from] * (1 - w) + ring[c][idx] * w : 0);
      }
      if (ch === 1) {
        for (let o = 0; o < outs; o++) out[o][i] = o < 2 ? s(0) : 0;
      } else if (ch <= 2 || outs >= ch) {
        for (let o = 0; o < outs; o++) out[o][i] = s(o);
      } else if (outs >= 6) {
        // 7.1 → 5.1: sides folded into the surrounds
        out[0][i] = s(0);
        out[1][i] = s(1);
        out[2][i] = s(2);
        out[3][i] = s(3);
        out[4][i] = soft(K * (s(4) + s(6)));
        out[5][i] = soft(K * (s(5) + s(7)));
      } else {
        out[0][i] = soft(s(0) + K * s(2) + K * s(4) + K * s(6));
        out[1][i] = soft(s(1) + K * s(2) + K * s(5) + K * s(7));
      }
    }
    // never play a sample twice (nor one played past)
    for (let a = this.read; a < this.read + skip + n; a++) {
      const idx = wrap(a);
      for (let c = 0; c < ch; c++) ring[c][idx] = 0;
    }
    this.read += skip + n;
    return true;
  }
}

registerProcessor('direct-audio', DirectAudio);
