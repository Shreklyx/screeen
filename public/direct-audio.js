'use strict';

// Direct mode (app → app) audio playback: the decoded Opus packets are written into a ring by
// their timestamps (each channel pair arrives separately) and played with a small buffer that
// grows only when the network makes it run dry, and slowly shrinks again.
//
// Input channel order (7.1): L R C LFE BL BR SL SR. Output: as many channels as the speakers
// have (8, 6 or 2); fewer speakers get a downmix (centre and surrounds −3 dB, no LFE).

const RATE = 48000;
const SIZE = RATE; // one second
const MIN_TARGET = 480; // 10 ms
const MAX_TARGET = 4800; // 100 ms
const K = Math.SQRT1_2;

const soft = (x) => (x > 0.9 ? 0.9 + 0.1 * Math.tanh((x - 0.9) / 0.1) : x < -0.9 ? -0.9 + 0.1 * Math.tanh((x + 0.9) / 0.1) : x);

class DirectAudio extends AudioWorkletProcessor {
  constructor({ processorOptions }) {
    super();
    this.channels = Math.max(1, Math.min(8, processorOptions.channels | 0));
    this.ring = Array.from({ length: this.channels }, () => new Float32Array(SIZE));
    this.read = null; // absolute sample index of the next output sample
    this.end = -Infinity; // one past the newest written sample
    this.target = 720; // 15 ms to start with
    this.calm = 0; // samples played since the last underrun
    this.port.onmessage = (e) => this.write(e.data);
  }

  write({ pair, ts, frames, planes }) {
    const at = Math.round((ts * RATE) / 1e6);
    if (this.read == null) this.read = at - this.target;
    if (at + frames <= this.read) return; // too late, already played
    if (at - this.read > SIZE - 2048) this.read = at - this.target; // far ahead (clock jump): start over
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

  process(_inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    if (this.read == null) return true;
    const buffered = this.end - this.read;
    if (buffered < n) {
      // ran dry: wait a little longer next time
      this.target = Math.min(MAX_TARGET, this.target + 240);
      this.calm = 0;
      if (buffered <= 0) this.read = this.end - this.target;
      return true; // silence
    }
    if (buffered > this.target + 2400) this.read = this.end - this.target; // the buffer grew (clock drift): catch up
    this.calm += n;
    if (this.calm > RATE * 5 && this.target > MIN_TARGET) {
      this.target -= 48;
      this.calm = 0;
    }
    const ch = this.channels;
    const outs = out.length;
    for (let i = 0; i < n; i++) {
      const idx = (((this.read + i) % SIZE) + SIZE) % SIZE;
      const s = (c) => (c < ch ? this.ring[c][idx] : 0);
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
      for (let c = 0; c < ch; c++) this.ring[c][idx] = 0; // never play a sample twice
    }
    this.read += n;
    return true;
  }
}

registerProcessor('direct-audio', DirectAudio);
