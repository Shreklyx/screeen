'use strict';

/* =====================================================================
 * Screen Share – low-latency 4K screen sharing with system audio.
 *
 * Signaling: a WebSocket to this site's own server (/ws). Media: WebRTC,
 * directly between the browsers (or through the server's TURN relay when a
 * direct path is impossible). The host keeps one RTCPeerConnection per
 * viewer and always makes the offers; viewers only answer.
 * ===================================================================== */

const RES_TARGETS = { native: null, 2160: [3840, 2160], 1440: [2560, 1440], 1080: [1920, 1080], 720: [1280, 720] };
const CONTENT_HINT = { motion: 'motion', detail: 'detail', balanced: '' };
const DEGRADATION = { motion: 'maintain-framerate', detail: 'maintain-resolution', balanced: 'balanced' };
const CODECS = ['AV1', 'H265', 'VP9', 'H264', 'VP8']; // best compression first
const CODEC_LABEL = { AV1: 'AV1', H265: 'H.265', VP9: 'VP9', H264: 'H.264', VP8: 'VP8' };
const OPUS_PARAMS = {
  stereo: 1, 'sprop-stereo': 1, maxaveragebitrate: 510000, maxplaybackrate: 48000,
  'sprop-maxcapturerate': 48000, useinbandfec: 1, usedtx: 0, cbr: 0,
};
const AUDIO_MAX_BPS = 510000;
const START_KBPS_CAP = 12000;
// 4K cap by default: a 5K/6K Retina screen at full size only adds encoding load and delay
const DEFAULT_SETTINGS = { res: '2160', fps: 60, kbps: 30000, mode: 'motion', codec: 'auto', audio: true };
const IS_MAC = /Macintosh/.test(navigator.userAgent);
const IS_SAFARI = /^((?!chrome|android|crios|fxios|edg).)*safari/i.test(navigator.userAgent);

const ERROR_TEXT = {
  auth: 'Hibás jelszó.',
  rate: 'Túl sok próbálkozás. Várj egy kicsit, és próbáld újra.',
  full: 'A szerver vagy a szoba megtelt. Próbáld újra később.',
  notfound: 'Ez a szoba nem létezik, vagy már bezárult.',
  offline: 'Nem sikerült elérni a szervert.',
  timeout: 'A szerver nem válaszolt időben.',
};
const errorText = (err) => ERROR_TEXT[err?.code] || err?.message || 'Ismeretlen hiba.';

// Troubleshooting aid: ?relay=1 forces all media through the TURN relay.
const FORCE_RELAY = new URLSearchParams(location.search).has('relay');
const rtcConfig = (iceServers) => ({ iceServers, bundlePolicy: 'max-bundle', iceTransportPolicy: FORCE_RELAY ? 'relay' : 'all' });

/* ---------------------------- helpers ---------------------------- */

const $ = (sel) => document.querySelector(sel);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Storage can be missing or throw (private mode, blocked site data) – never let it break the page.
const store = {
  area(kind) { return kind === 'session' ? window.sessionStorage : window.localStorage; },
  get(key, fallback, kind) {
    try { const v = this.area(kind).getItem(key); return v == null ? fallback : JSON.parse(v); } catch { return fallback; }
  },
  set(key, value, kind) {
    try { this.area(kind).setItem(key, JSON.stringify(value)); } catch { /* storage unavailable */ }
  },
  remove(key, kind) {
    try { this.area(kind).removeItem(key); } catch { /* storage unavailable */ }
  },
};

function parseCode(input) {
  let s = String(input || '').trim();
  const hash = s.indexOf('#');
  if (hash >= 0) s = s.slice(hash + 1);
  return s.toLowerCase().replace(/[^a-z0-9]/g, '');
}

const randomId = () => Array.from(crypto.getRandomValues(new Uint8Array(8)), (b) => b.toString(16).padStart(2, '0')).join('');

function showView(id) {
  for (const el of document.querySelectorAll('[data-view]')) el.hidden = el.id !== id;
}

let toastTimer;
function toast(message, kind = 'info', ms = 4000) {
  const el = $('#toast');
  el.textContent = message;
  el.className = `toast show ${kind}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = 'toast'; }, ms);
}

const nf1 = new Intl.NumberFormat('hu-HU', { maximumFractionDigits: 1 });
function fmtKbps(kbps) {
  if (kbps == null || !isFinite(kbps)) return '–';
  return kbps >= 1000 ? `${nf1.format(kbps / 1000)} Mbit/s` : `${Math.round(kbps)} kbit/s`;
}
const fmtMs = (ms) => (ms == null || !isFinite(ms) ? '–' : `${Math.round(ms)} ms`);

let audioCtx = null;
function ensureAudioCtx() {
  if (!audioCtx) {
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return null;
    audioCtx = new Ctx({ latencyHint: 'interactive' });
  }
  if (audioCtx.state === 'suspended') audioCtx.resume().catch(() => {});
  return audioCtx;
}

/* --------------------------- signaling --------------------------- */

/**
 * WebSocket to /ws that reconnects by itself. `request()` sends a message and
 * resolves with the first reply of the given types (or rejects on an error reply).
 */
function createSignal({ onMessage, onOpen, onClose }) {
  let ws = null;
  let closed = false;
  let attempt = 0;
  let timer = null;
  const waiters = new Set();
  const openWaiters = new Set();

  function connect() {
    const socket = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
    ws = socket;
    socket.onopen = () => {
      attempt = 0;
      for (const w of openWaiters) w();
      openWaiters.clear();
      onOpen?.();
    };
    socket.onmessage = (e) => {
      let msg;
      try { msg = JSON.parse(e.data); } catch { return; }
      if (!msg || typeof msg !== 'object') return;
      for (const w of waiters) {
        if (w.match(msg)) { waiters.delete(w); w.settle(msg); return; }
      }
      onMessage(msg);
    };
    socket.onclose = () => {
      if (ws !== socket) return;
      ws = null;
      for (const w of waiters) w.fail(Object.assign(new Error('offline'), { code: 'offline' }));
      waiters.clear();
      if (closed) return;
      onClose?.();
      timer = setTimeout(connect, Math.min(10000, 500 * 2 ** attempt++));
    };
    socket.onerror = () => {};
  }
  connect();

  const api = {
    get open() { return ws?.readyState === WebSocket.OPEN; },
    send(msg) {
      if (ws?.readyState !== WebSocket.OPEN) return false;
      ws.send(JSON.stringify(msg));
      return true;
    },
    whenOpen(ms = 10000) {
      if (api.open) return Promise.resolve();
      return new Promise((resolve, reject) => {
        const done = () => { clearTimeout(t); resolve(); };
        const t = setTimeout(() => { openWaiters.delete(done); reject(Object.assign(new Error('offline'), { code: 'offline' })); }, ms);
        openWaiters.add(done);
      });
    },
    request(msg, okTypes, ms = 10000) {
      return new Promise((resolve, reject) => {
        const w = {
          match: (m) => okTypes.includes(m.t) || m.t === 'error',
          settle: (m) => {
            clearTimeout(w.timer);
            if (m.t === 'error') reject(Object.assign(new Error(m.code), { code: m.code }));
            else resolve(m);
          },
          fail: (err) => { clearTimeout(w.timer); reject(err); },
        };
        w.timer = setTimeout(() => { waiters.delete(w); reject(Object.assign(new Error('timeout'), { code: 'timeout' })); }, ms);
        waiters.add(w);
        if (!api.send(msg)) { waiters.delete(w); w.fail(Object.assign(new Error('offline'), { code: 'offline' })); }
      });
    },
    close() {
      closed = true;
      clearTimeout(timer);
      ws?.close();
    },
  };
  return api;
}

/* --------------------------- codecs / SDP --------------------------- */

const isAvailable = (caps, codec) => (caps?.[codec]?.supported ?? true);

async function codecSupport(direction) {
  const Api = direction === 'encode' ? window.RTCRtpSender : window.RTCRtpReceiver;
  const list = Api?.getCapabilities?.('video')?.codecs?.map((c) => c.mimeType.toLowerCase()) ?? null;
  const result = {};
  await Promise.all(CODECS.map(async (codec) => {
    const supported = list ? list.includes(`video/${codec.toLowerCase()}`) : true;
    let hw = false;
    if (supported && navigator.mediaCapabilities) {
      const cfg = { type: 'webrtc', video: { contentType: `video/${codec}`, width: 3840, height: 2160, bitrate: 30e6, framerate: 60 } };
      try {
        const info = direction === 'encode'
          ? await navigator.mediaCapabilities.encodingInfo(cfg)
          : await navigator.mediaCapabilities.decodingInfo(cfg);
        hw = Boolean(info.supported && info.powerEfficient);
      } catch { /* 'webrtc' type not supported by this browser */ }
    }
    result[codec] = { supported, hw };
  }));
  return result;
}

// Prefer a codec that is hardware-accelerated on both ends, then one with a
// hardware encoder (what this machine actually did beats the browser's
// prediction), then H.264 – hardware on most PCs and Macs, fast in software.
function pickCodec(enc, dec) {
  const usable = CODECS.filter((c) => isAvailable(enc, c) && isAvailable(dec, c));
  const seen = gpu.seen;
  const encHw = (c) => seen[c] ?? enc?.[c]?.hw ?? false;
  return usable.find((c) => encHw(c) && dec?.[c]?.hw)
    || usable.find((c) => seen[c] === true)
    || ['H264', 'H265', 'AV1', 'VP9'].find((c) => usable.includes(c) && encHw(c))
    || ['H264'].find((c) => usable.includes(c) && seen[c] !== false)
    || ['H264', 'VP8', 'VP9', 'AV1', 'H265'].find((c) => usable.includes(c))
    || 'VP8';
}

/* ---- graphics card / hardware encoder ---- */

function detectGpu() {
  try {
    const gl = document.createElement('canvas').getContext('webgl');
    if (!gl) return '';
    const ext = gl.getExtension('WEBGL_debug_renderer_info');
    const name = String(gl.getParameter(ext ? ext.UNMASKED_RENDERER_WEBGL : gl.RENDERER) || '');
    gl.getExtension('WEBGL_lose_context')?.loseContext();
    return name;
  } catch {
    return '';
  }
}

// "ANGLE (NVIDIA, NVIDIA GeForce RTX 3070 (0x00002484) Direct3D11 …)" → "NVIDIA GeForce RTX 3070"
function prettyGpu(raw) {
  const m = /^ANGLE \([^,]+, (.+?)(?: \(0x[0-9a-f]+\))?(?: Direct3D| OpenGL| Vulkan|, |\)$)/i.exec(raw);
  return (m ? m[1] : raw).replace(/^ANGLE Metal Renderer: /, '').trim();
}

function gpuVendor(name) {
  if (/nvidia|geforce|quadro/i.test(name)) return 'nvidia';
  if (/\bamd\b|radeon/i.test(name)) return 'amd';
  if (/intel/i.test(name)) return 'intel';
  if (/apple/i.test(name)) return 'apple';
  return '';
}

const HW_ENCODER = { nvidia: 'NVIDIA NVENC', amd: 'AMD VCN', intel: 'Intel Quick Sync', apple: 'Apple VideoToolbox' };

const gpu = {
  raw: '',
  name: '',
  vendor: '',
  key: '',
  seen: {}, // codec → true (hardware) / false (software), learned from real encoding
  init() {
    if (this.key) return;
    this.raw = detectGpu();
    this.name = prettyGpu(this.raw);
    this.vendor = IS_MAC ? 'apple' : gpuVendor(this.raw);
    const browser = /(?:Chrome|Firefox|Version)\/(\d+)/.exec(navigator.userAgent)?.[1] || '';
    this.key = `${this.raw}|${browser}`;
    this.seen = store.get('ss-hw', {})[this.key] || {};
  },
  note(codec, isHw) {
    if (typeof isHw !== 'boolean' || this.seen[codec] === isHw) return;
    this.seen[codec] = isHw;
    const all = store.get('ss-hw', {});
    all[this.key] = this.seen;
    store.set('ss-hw', all);
  },
  get encoderName() { return HW_ENCODER[this.vendor] || 'hardveres kódoló'; },
};

function encoderIsHw(out) {
  if (typeof out.powerEfficientEncoder === 'boolean') return out.powerEfficientEncoder;
  const impl = out.encoderImplementation || '';
  if (/MediaFoundation|VideoToolbox|Vaapi|NVENC|V4L2|D3D12|EncodeAccelerator/i.test(impl)) return true;
  if (/libvpx|OpenH264|libaom|SVT|rav1e/i.test(impl)) return false;
  return null;
}

function mergeFmtp(line, params) {
  const space = line.indexOf(' ');
  const head = line.slice(0, space);
  const map = new Map();
  for (const kv of line.slice(space + 1).split(';')) {
    const [k, v] = kv.split('=');
    if (k && k.trim()) map.set(k.trim(), v);
  }
  for (const [k, v] of Object.entries(params)) map.set(k, String(v));
  return `${head} ${[...map].map(([k, v]) => (v == null ? k : `${k}=${v}`)).join(';')}`;
}

/**
 * Tunes the viewer's answer before the host applies it:
 *  - Opus: stereo, 510 kbit/s, 10 ms frames, no DTX, in-band FEC.
 *  - video: `preferCodec` first, high start bitrate, and libwebrtc's 2.5 Mbit/s
 *    default ceiling lifted (the real cap is set with setParameters()).
 */
function mungeSdp(sdp, { preferCodec, startKbps } = {}) {
  const sections = sdp.split(/\r\n(?=m=)/);
  return sections.map((section) => {
    if (!section.startsWith('m=')) return section;
    const kind = section.slice(2, section.indexOf(' '));
    const lines = section.split('\r\n');
    const names = {};
    for (const l of lines) {
      const m = /^a=rtpmap:(\d+) ([^/\s]+)\//.exec(l);
      if (m) names[m[1]] = m[2].toUpperCase();
    }
    const setParams = (pt, params) => {
      const i = lines.findIndex((l) => l.startsWith(`a=fmtp:${pt} `));
      if (i >= 0) {
        lines[i] = mergeFmtp(lines[i], params);
      } else {
        const r = lines.findIndex((l) => l.startsWith(`a=rtpmap:${pt} `));
        lines.splice(r + 1, 0, mergeFmtp(`a=fmtp:${pt} `, params));
      }
    };

    if (kind === 'audio') {
      for (const [pt, name] of Object.entries(names)) if (name === 'OPUS') setParams(pt, OPUS_PARAMS);
      // 10 ms Opus frames instead of 20 ms: less audio delay
      const ptime = lines.findIndex((l) => l.startsWith('a=ptime:'));
      if (ptime >= 0) lines[ptime] = 'a=ptime:10';
      else if (Object.values(names).includes('OPUS')) lines.splice(lines.findIndex((l) => /^a=rtpmap:\d+ opus\//i.test(l)) + 1, 0, 'a=ptime:10');
    } else if (kind === 'video') {
      if (startKbps) {
        for (const [pt, name] of Object.entries(names)) {
          if (CODECS.includes(name)) setParams(pt, { 'x-google-start-bitrate': startKbps, 'x-google-max-bitrate': 100000 });
        }
      }
      if (preferCodec) {
        const parts = lines[0].split(' ');
        const pts = parts.slice(3);
        const first = pts.filter((pt) => names[pt] === preferCodec);
        if (first.length) lines[0] = [...parts.slice(0, 3), ...first, ...pts.filter((pt) => !first.includes(pt))].join(' ');
      }
    }
    return lines.join('\r\n');
  }).join('\r\n');
}

/* ----------------------------- stats ----------------------------- */

function selectedPair(report) {
  let pair = null;
  report.forEach((s) => {
    if (s.type === 'transport' && s.selectedCandidatePairId) pair = report.get(s.selectedCandidatePairId);
  });
  if (!pair) report.forEach((s) => { if (!pair && s.type === 'candidate-pair' && s.nominated && s.state === 'succeeded') pair = s; });
  return pair;
}

function isRelayed(report, pair) {
  if (!pair) return null;
  const local = report.get(pair.localCandidateId);
  const remote = report.get(pair.remoteCandidateId);
  return local?.candidateType === 'relay' || remote?.candidateType === 'relay';
}

const rate = (cur, prev, key) => (prev && cur[key] != null && prev[key] != null ? cur[key] - prev[key] : null);
const kbpsBetween = (cur, prev, key) => {
  const bytes = cur ? rate(cur, prev, key) : null;
  const ms = cur && prev ? cur.timestamp - prev.timestamp : 0;
  return bytes != null && ms > 0 ? (bytes * 8) / ms : null;
};

/* ============================== HOST ============================== */

const settings = { ...DEFAULT_SETTINGS, ...store.get('ss-settings', {}) };

const host = {
  signal: null,
  password: '',
  code: null,
  token: null,
  iceServers: [],
  msid: null, // groups audio + video into one stream on the viewer side (lip sync)
  video: null,
  audio: null,
  viewers: new Map(),
  seq: 0,
  encCaps: null,
  statsTimer: null,
  meter: null,
  dims: '',
  previewOn: true,
  active: false,
  tick: 0,
};

const isLive = () => Boolean(host.video && host.video.readyState === 'live');

async function captureScreen() {
  const fps = settings.fps;
  const video = { frameRate: { ideal: fps, max: fps }, cursor: 'always' };
  const audio = settings.audio
    ? { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: { ideal: 2 }, sampleRate: { ideal: 48000 }, suppressLocalAudioPlayback: false }
    : false;
  const extras = {
    systemAudio: settings.audio ? 'include' : 'exclude',
    selfBrowserSurface: 'exclude',
    surfaceSwitching: 'include',
    monitorTypeSurfaces: 'include',
  };
  let stream;
  try {
    stream = await navigator.mediaDevices.getDisplayMedia({ video, audio, ...extras });
  } catch (err) {
    // Browsers without display-audio support may reject the audio constraints.
    if (!audio || (err.name !== 'TypeError' && err.name !== 'NotSupportedError')) throw err;
    stream = await navigator.mediaDevices.getDisplayMedia({ video, ...extras });
  }
  const videoTrack = stream.getVideoTracks()[0];
  const audioTrack = stream.getAudioTracks()[0] || null;
  videoTrack.contentHint = CONTENT_HINT[settings.mode];
  if (audioTrack) audioTrack.contentHint = 'music';
  return { video: videoTrack, audio: audioTrack };
}

function hostSignal() {
  if (!host.signal) {
    host.signal = createSignal({
      onMessage: onHostMessage,
      onOpen: () => { if (host.code && host.active) reclaimRoom(); },
      onClose: () => { if (host.code) setSignalStatus('Megszakadt a kapcsolat a szerverrel, újracsatlakozás… (a nézők látnak tovább)', 'warn'); },
    });
  }
  return host.signal;
}

async function openRoom(password) {
  const signal = hostSignal();
  await signal.whenOpen(8000);
  const resume = store.get('ss-host-room', null, 'session');
  const res = await signal.request({ t: 'host', password, resume: resume || undefined }, ['hosted']);
  applyHosted(res);
  return Boolean(resume && res.code === resume.code);
}

// After a lost server connection: take the room back with its token (password as fallback).
async function reclaimRoom() {
  try {
    const res = await host.signal.request({ t: 'host', password: host.password, resume: { code: host.code, token: host.token } }, ['hosted']);
    const changed = res.code !== host.code;
    applyHosted(res);
    if (changed) toast('A korábbi szoba lejárt, új link készült – küldd el újra a nézőknek.', 'warn', 8000);
  } catch (err) {
    setSignalStatus(`Nem sikerült visszacsatlakozni a szobához: ${errorText(err)}`, 'warn');
  }
}

function applyHosted(res) {
  host.code = res.code;
  host.token = res.token;
  host.iceServers = res.iceServers || [];
  store.set('ss-host-room', { code: res.code, token: res.token }, 'session');
  const link = `${location.origin}${location.pathname}#${res.code}`;
  $('#roomCode').textContent = res.code;
  $('#roomLink').value = link;
  $('#btnCopy').disabled = false;
  setSignalStatus('Kapcsolódva. A nézők a linkkel vagy a kóddal csatlakozhatnak.', 'ok');
  host.signal.send({ t: 'state', live: isLive() });
  const present = new Set((res.viewers || []).map((v) => v.id));
  for (const id of [...host.viewers.keys()]) if (!present.has(id)) dropViewer(id);
  for (const v of res.viewers || []) ensureViewer(v.id, v.caps);
}

async function startHosting(e) {
  e?.preventDefault();
  const password = $('#hostPassword').value;
  const errorEl = $('#shareError');
  errorEl.hidden = true;
  ensureAudioCtx(); // must happen inside the click gesture
  gpu.init();
  const encCaps = host.encCaps ? Promise.resolve(host.encCaps) : codecSupport('encode');
  const capture = captureScreen(); // the screen picker needs the gesture: start it right away
  const room = openRoom(password);

  let tracks;
  try {
    tracks = await capture;
  } catch (err) {
    room.then((resumed) => {
      for (const id of [...host.viewers.keys()]) dropViewer(id);
      if (resumed) return; // viewers of a resumed room keep waiting ("szünetel")
      host.signal.send({ t: 'close' });
      host.code = null;
      host.token = null;
      store.remove('ss-host-room', 'session');
    }, () => {});
    showCaptureError(err, showShareError);
    return;
  }
  try {
    await room;
  } catch (err) {
    tracks.video.stop();
    tracks.audio?.stop();
    showShareError(errorText(err));
    if (err.code === 'auth') { $('#hostPassword').select(); store.remove('ss-host-password'); }
    return;
  }

  host.password = password;
  if ($('#rememberPassword').checked) store.set('ss-host-password', password);
  else store.remove('ss-host-password');

  host.active = true;
  showView('view-host');
  window.addEventListener('beforeunload', confirmLeave);
  host.encCaps = await encCaps;
  updateCodecOptions();
  setHostTracks(tracks);
  if (!host.statsTimer) host.statsTimer = setInterval(hostStatsTick, 1000);
}

const PERMISSION_TEXT = IS_MAC
  ? 'A macOS nem engedi a képernyőfelvételt. Rendszerbeállítások → Adatvédelem és biztonság → Képernyőfelvétel (újabb macOS-en: Képernyő- és rendszerhangfelvétel) → kapcsold be a böngészőt, majd indítsd újra.'
  : 'Az operációs rendszer nem engedi a képernyőfelvételt – nézd meg az adatvédelmi beállításokat.';

function showCaptureError(err, show) {
  if (err.name === 'NotAllowedError' && /by system/i.test(err.message)) show(PERMISSION_TEXT);
  else if (err.name !== 'NotAllowedError' && err.name !== 'AbortError') show(`A megosztás nem indult el: ${err.message}`);
}

function showShareError(text) {
  const el = $('#shareError');
  el.textContent = text;
  el.hidden = false;
}

function setSignalStatus(text, kind = '') {
  const el = $('#signalStatus');
  el.textContent = text;
  el.className = `hint ${kind}`;
}

function onHostMessage(msg) {
  switch (msg.t) {
    case 'viewer': ensureViewer(msg.id, msg.caps); break;
    case 'viewer-left': dropViewer(msg.id); break;
    case 'signal': onViewerSignal(msg.from, msg.data); break;
    case 'count': $('#viewerCount').textContent = String(msg.n); break;
    case 'replaced':
      host.code = null;
      stopTracks();
      for (const id of [...host.viewers.keys()]) dropViewer(id);
      window.removeEventListener('beforeunload', confirmLeave);
      toast('A szobát egy másik ablakban vetted át – itt leállt a megosztás.', 'warn', 8000);
      setSignalStatus('A szobát egy másik ablak vette át.', 'warn');
      break;
    default: break;
  }
}

/* ---- one RTCPeerConnection per viewer ---- */

function videoEncoding() {
  return {
    maxBitrate: settings.kbps * 1000,
    maxFramerate: settings.fps,
    scaleResolutionDownBy: scaleFor(host.video),
    priority: 'high',
    networkPriority: 'high',
  };
}

function ensureViewer(id, caps) {
  let v = host.viewers.get(id);
  if (v && v.pc && !['failed', 'closed'].includes(v.pc.connectionState)) return;
  if (!v) {
    v = { id, n: ++host.seq, caps: caps || null, pc: null, stats: null, prev: null };
    host.viewers.set(id, v);
  }
  createViewerPc(v);
  renderViewers();
}

function addSendTransceiver(pc, trackOrKind, encoding) {
  const init = { direction: 'sendonly', streams: [host.msid] };
  try {
    return pc.addTransceiver(trackOrKind, { ...init, sendEncodings: [encoding] });
  } catch {
    return pc.addTransceiver(trackOrKind, init); // browsers that reject some encoding fields
  }
}

function createViewerPc(v) {
  closeViewerPc(v);
  host.msid ||= new MediaStream();
  v.session = randomId();
  v.createdAt = Date.now();
  v.codecSince = Date.now();
  v.hwTrialDone = false;
  v.remote = null;
  v.codec = settings.codec === 'auto' ? pickCodec(host.encCaps, v.caps) : settings.codec;
  v.negotiating = false;
  v.pendingNegotiation = false;
  v.pendingRestart = false;
  v.remoteCandidates = [];
  v.restarts = 0;
  v.prev = null;
  v.stats = null;

  const pc = new RTCPeerConnection(rtcConfig(host.iceServers));
  v.pc = pc;
  v.videoTx = addSendTransceiver(pc, host.video || 'video', videoEncoding());
  v.audioTx = addSendTransceiver(pc, host.audio || 'audio', { maxBitrate: AUDIO_MAX_BPS, priority: 'high', networkPriority: 'high' });
  applyCodecPreference(v);

  pc.onicecandidate = (e) => {
    if (e.candidate && v.pc === pc) {
      host.signal.send({ t: 'signal', to: v.id, data: { type: 'candidate', session: v.session, candidate: e.candidate.toJSON() } });
    }
  };
  pc.onconnectionstatechange = () => onViewerPcState(v, pc);
  negotiate(v);
}

function closeViewerPc(v) {
  clearTimeout(v.answerTimer);
  clearTimeout(v.discTimer);
  if (v.pc) {
    v.pc.onicecandidate = null;
    v.pc.onconnectionstatechange = null;
    v.pc.close();
    v.pc = null;
  }
}

function dropViewer(id) {
  const v = host.viewers.get(id);
  if (!v) return;
  closeViewerPc(v);
  host.viewers.delete(id);
  renderViewers();
}

// Put the chosen codec first; the answer is reordered too (see mungeSdp), so
// this is only a bonus where setCodecPreferences works for send-only transceivers.
function applyCodecPreference(v) {
  const tx = v.videoTx;
  if (!tx?.setCodecPreferences) return;
  const mime = `video/${v.codec}`.toLowerCase();
  for (const Api of [window.RTCRtpReceiver, window.RTCRtpSender]) {
    const codecs = Api?.getCapabilities?.('video')?.codecs;
    if (!codecs?.some((c) => c.mimeType.toLowerCase() === mime)) continue;
    const ordered = [...codecs.filter((c) => c.mimeType.toLowerCase() === mime), ...codecs.filter((c) => c.mimeType.toLowerCase() !== mime)];
    try { tx.setCodecPreferences(ordered); return; } catch { /* try the other capability list */ }
  }
}

async function negotiate(v, { iceRestart = false } = {}) {
  const pc = v.pc;
  if (!pc) return;
  if (v.negotiating) {
    v.pendingNegotiation = true;
    v.pendingRestart ||= iceRestart;
    return;
  }
  v.negotiating = true;
  try {
    const offer = await pc.createOffer({ iceRestart });
    if (v.pc !== pc) return;
    await pc.setLocalDescription(offer);
    host.signal.send({ t: 'signal', to: v.id, data: { type: 'offer', session: v.session, sdp: pc.localDescription.sdp } });
    clearTimeout(v.answerTimer);
    // No answer (lost message, viewer stuck): start over with a fresh connection.
    v.answerTimer = setTimeout(() => {
      if (v.pc === pc && pc.signalingState === 'have-local-offer' && host.viewers.get(v.id) === v) createViewerPc(v);
    }, 15000);
  } catch (err) {
    console.warn('offer failed', err);
    v.negotiating = false;
  }
}

async function onViewerSignal(id, data) {
  const v = host.viewers.get(id);
  if (!v || !v.pc || !data || typeof data !== 'object') return;
  if (data.type === 'recall') {
    if (Date.now() - (v.createdAt || 0) > 5000) createViewerPc(v);
    return;
  }
  if (data.session !== v.session) return;
  const pc = v.pc;
  if (data.type === 'stats') {
    const num = (x) => (Number.isFinite(x) && x >= 0 && x < 1e6 ? x : null);
    v.remote = { lat: num(data.lat), fps: num(data.fps), loss: num(data.loss) };
    return;
  }
  if (data.type === 'answer' && typeof data.sdp === 'string') {
    if (pc.signalingState !== 'have-local-offer') return;
    clearTimeout(v.answerTimer);
    try {
      await pc.setRemoteDescription({ type: 'answer', sdp: mungeSdp(data.sdp, { preferCodec: v.codec, startKbps: Math.min(settings.kbps, START_KBPS_CAP) }) });
    } catch (err) {
      console.warn('answer rejected', err);
      if (v.pc === pc) createViewerPc(v);
      return;
    }
    if (v.pc !== pc) return;
    for (const c of v.remoteCandidates.splice(0)) pc.addIceCandidate(c).catch(() => {});
    v.negotiating = false;
    if (pc.connectionState === 'connected') applySenderParams(v);
    if (v.pendingNegotiation) {
      const iceRestart = v.pendingRestart;
      v.pendingNegotiation = false;
      v.pendingRestart = false;
      negotiate(v, { iceRestart });
    }
  } else if (data.type === 'candidate' && data.candidate) {
    if (pc.remoteDescription) pc.addIceCandidate(data.candidate).catch(() => {});
    else v.remoteCandidates.push(data.candidate);
  }
}

function onViewerPcState(v, pc) {
  if (v.pc !== pc) return;
  const state = pc.connectionState;
  clearTimeout(v.discTimer);
  if (state === 'connected') {
    v.restarts = 0;
    applySenderParams(v);
  } else if (state === 'disconnected') {
    v.discTimer = setTimeout(() => { if (v.pc === pc && pc.connectionState === 'disconnected') restartIce(v); }, 4000);
  } else if (state === 'failed') {
    restartIce(v);
  }
  renderViewers();
}

function restartIce(v) {
  if (v.restarts++ >= 2) createViewerPc(v);
  else negotiate(v, { iceRestart: true });
}

function scaleFor(track) {
  const target = RES_TARGETS[settings.res];
  const s = track?.getSettings?.() || {};
  if (!target || !s.width || !s.height) return 1;
  const [tw, th] = s.width >= s.height ? target : [target[1], target[0]];
  return Math.max(1, s.width / tw, s.height / th);
}

async function applySenderParams(v) {
  if (!v.pc) return;
  for (const [tx, kind] of [[v.videoTx, 'video'], [v.audioTx, 'audio']]) {
    const sender = tx?.sender;
    if (!sender) continue;
    const p = sender.getParameters();
    if (!p.encodings?.length) continue; // not negotiated yet
    const enc = p.encodings[0];
    if (kind === 'video') {
      Object.assign(enc, videoEncoding());
      p.degradationPreference = DEGRADATION[settings.mode];
    } else {
      Object.assign(enc, { maxBitrate: AUDIO_MAX_BPS, priority: 'high', networkPriority: 'high' });
    }
    try {
      await sender.setParameters(p);
    } catch {
      // retry with fresh parameters and only the essential fields
      const q = sender.getParameters();
      if (!q.encodings?.length) continue;
      const e = q.encodings[0];
      e.maxBitrate = enc.maxBitrate;
      if (kind === 'video') {
        e.maxFramerate = enc.maxFramerate;
        e.scaleResolutionDownBy = enc.scaleResolutionDownBy;
      }
      try { await sender.setParameters(q); } catch (err) { console.warn('setParameters failed', err); }
    }
  }
}

function applyAllSenders() {
  for (const v of host.viewers.values()) applySenderParams(v);
}

/* ---- capture lifecycle ---- */

function setHostTracks({ video, audio }) {
  const old = [host.video, host.audio];
  host.video = video;
  host.audio = audio;
  video.addEventListener('ended', () => { if (host.video === video) pauseSharing(); });

  for (const v of host.viewers.values()) {
    if (!v.pc) continue;
    v.videoTx?.sender.replaceTrack(video).catch((err) => console.warn('replaceTrack failed', err));
    v.audioTx?.sender.replaceTrack(audio).catch((err) => console.warn('replaceTrack failed', err));
    applySenderParams(v);
  }
  for (const t of old) if (t && t !== video && t !== audio) t.stop();

  host.dims = '';
  if (host.previewOn) $('#preview').srcObject = new MediaStream([video]);
  $('#previewPaused').hidden = true;
  $('#liveBadge').classList.remove('off');
  $('#liveBadge').textContent = 'ÉLŐ';
  $('#btnStop').hidden = false;
  host.signal?.send({ t: 'state', live: true });
  startMeter(audio);
  updateCaptureInfo();
  renderViewers();
}

function stopTracks() {
  host.video?.stop();
  host.audio?.stop();
  host.video = null;
  host.audio = null;
  stopMeter();
}

function pauseSharing() {
  stopTracks();
  for (const v of host.viewers.values()) {
    v.videoTx?.sender.replaceTrack(null).catch(() => {});
    v.audioTx?.sender.replaceTrack(null).catch(() => {});
  }
  $('#preview').srcObject = null;
  $('#previewPaused').hidden = false;
  $('#liveBadge').classList.add('off');
  $('#liveBadge').textContent = 'SZÜNETEL';
  $('#btnStop').hidden = true;
  $('#captureInfo').textContent = '';
  $('#audioStatus').textContent = '';
  host.signal?.send({ t: 'state', live: false });
  renderViewers();
}

async function switchSource() {
  ensureAudioCtx();
  try {
    setHostTracks(await captureScreen());
  } catch (err) {
    showCaptureError(err, (text) => toast(text, 'error', 8000));
  }
}

function closeRoom() {
  if (host.viewers.size && !confirm('Biztosan bezárod a szobát? Minden néző kilép.')) return;
  host.signal?.send({ t: 'close' });
  window.removeEventListener('beforeunload', confirmLeave);
  store.remove('ss-host-room', 'session');
  stopTracks();
  for (const id of [...host.viewers.keys()]) dropViewer(id);
  setTimeout(() => { location.href = location.pathname; }, 200);
}

function confirmLeave(e) {
  if (!host.viewers.size) return;
  e.preventDefault();
  e.returnValue = '';
}

function updateCaptureInfo() {
  const s = host.video?.getSettings?.() || {};
  const scale = scaleFor(host.video);
  const w = s.width ? Math.round(s.width / scale) : null;
  const h = s.height ? Math.round(s.height / scale) : null;
  const fps = s.frameRate ? Math.round(s.frameRate) : settings.fps;
  $('#captureInfo').textContent = w && h ? `${w}×${h} · ${fps} FPS` : '';

  const el = $('#audioStatus');
  if (!host.video) {
    el.textContent = '';
  } else if (host.audio) {
    el.textContent = 'Hang megosztva (sztereó). A csík mutatja, hogy jön-e hang.';
    el.className = 'hint ok';
  } else if (!settings.audio) {
    el.textContent = 'A hangmegosztás ki van kapcsolva a beállításokban.';
    el.className = 'hint';
  } else {
    if (IS_SAFARI) {
      el.textContent = 'A Safari nem tud hangot megosztani. Ha hang is kell, oszd meg Chrome-ból.';
    } else if (IS_MAC) {
      el.textContent = 'Nincs hang. Macen egy böngészőlap hangja biztosan megosztható: Forrás váltása → „Chrome-lap” fül → lap hangjának megosztása. Teljes képernyőnél akkor van hang, ha a Chrome felajánlja a „Rendszerhang megosztása” opciót (újabb macOS).';
    } else {
      el.textContent = s.displaySurface === 'window'
        ? 'Nincs hang: ablak megosztásakor a Windows nem ad hangot. Válts „Teljes képernyő”-re, és pipáld be a „Rendszerhang megosztása” opciót.'
        : 'Nincs hang! A Forrás váltása gombbal válaszd újra a képernyőt, és pipáld be a „Rendszerhang megosztása” (vagy lapnál a lap hangja) opciót.';
    }
    el.className = 'hint warn';
  }
}

function startMeter(track) {
  stopMeter();
  const ctx = track && ensureAudioCtx();
  if (!ctx) return;
  const src = ctx.createMediaStreamSource(new MediaStream([track]));
  const analyser = ctx.createAnalyser();
  analyser.fftSize = 1024;
  src.connect(analyser); // analysis only – never routed to the speakers
  const buf = new Float32Array(analyser.fftSize);
  const bar = $('#audioMeter');
  let level = 0;
  const loop = () => {
    analyser.getFloatTimeDomainData(buf);
    let peak = 0;
    for (let i = 0; i < buf.length; i++) peak = Math.max(peak, Math.abs(buf[i]));
    level = Math.max(peak, level * 0.9);
    bar.style.transform = `scaleX(${Math.min(1, level * 1.4).toFixed(3)})`;
    host.meter.raf = requestAnimationFrame(loop);
  };
  host.meter = { src, raf: requestAnimationFrame(loop) };
}

function stopMeter() {
  if (!host.meter) return;
  cancelAnimationFrame(host.meter.raf);
  host.meter.src.disconnect();
  host.meter = null;
  $('#audioMeter').style.transform = 'scaleX(0)';
}

function updateSetting(key, value) {
  settings[key] = value;
  store.set('ss-settings', settings);
  if (key === 'fps' && isLive()) {
    host.video.applyConstraints({ frameRate: { ideal: value, max: value } }).catch(() => {});
  }
  if (key === 'mode' && host.video) host.video.contentHint = CONTENT_HINT[value];
  if (key === 'codec') {
    for (const v of host.viewers.values()) {
      if (!v.pc) continue;
      v.hwTrialDone = false;
      switchCodec(v, value === 'auto' ? pickCodec(host.encCaps, v.caps) : value);
    }
    renderViewers();
    return;
  }
  if (key === 'audio') {
    updateCaptureInfo();
    return;
  }
  applyAllSenders();
  updateCaptureInfo();
}

function switchCodec(v, codec) {
  v.codec = codec;
  v.codecSince = Date.now();
  applyCodecPreference(v);
  negotiate(v);
}

/**
 * The browser's hardware prediction can be wrong. Once a viewer has received a
 * few seconds of video, check whether the GPU (NVENC, VideoToolbox, Quick Sync,
 * VCN) really encodes; if not, move to the next candidate and remember the result.
 */
function checkHardware(v, out, codec) {
  const isHw = encoderIsHw(out);
  if (isHw == null || codec !== v.codec || Date.now() - v.codecSince < 3000 || !(out.framesEncoded > 30)) return;
  // Software while another viewer gets the same codec in hardware: the GPU's encoder
  // sessions are used up (GeForce: ~8) – not a property of the codec, so don't learn it.
  if (!isHw && [...host.viewers.values()].some((o) => o !== v && o.stats?.hw === true && o.stats?.codec === codec)) {
    v.hwTrialDone = true;
    return;
  }
  gpu.note(v.codec, isHw);
  if (isHw || settings.codec !== 'auto' || v.hwTrialDone) return;
  const next = pickCodec(host.encCaps, v.caps);
  if (next !== v.codec && gpu.seen[next] !== false) switchCodec(v, next);
  else v.hwTrialDone = true;
}

function updateCodecOptions() {
  for (const opt of $('#setCodec').options) {
    if (opt.value === 'auto') continue;
    const c = host.encCaps?.[opt.value];
    opt.disabled = c ? !c.supported : false;
    const hw = gpu.seen[opt.value] ?? c?.hw;
    opt.textContent = `${CODEC_LABEL[opt.value]}${hw ? ' – hardveres' : ''}${c && !c.supported ? ' (nem támogatott)' : ''}`;
  }
}

async function hostStatsTick() {
  const s = host.video?.getSettings?.() || {};
  const dims = `${s.width}x${s.height}`;
  if (isLive() && dims !== host.dims) {
    host.dims = dims;
    applyAllSenders();
    updateCaptureInfo();
  }

  for (const v of host.viewers.values()) {
    const pc = v.pc;
    if (!pc || pc.connectionState !== 'connected') { v.stats = null; continue; }
    let report;
    try { report = await pc.getStats(); } catch { continue; }
    let out = null;
    let remoteIn = null;
    report.forEach((x) => {
      if (x.type === 'outbound-rtp' && x.kind === 'video') out = x;
      if (x.type === 'remote-inbound-rtp' && x.kind === 'video') remoteIn = x;
    });
    if (!out) continue;
    const pair = selectedPair(report);
    const codec = report.get(out.codecId)?.mimeType?.split('/')[1]?.toUpperCase() || null;
    const dFrames = rate(out, v.prev, 'framesEncoded');
    v.stats = {
      w: out.frameWidth,
      h: out.frameHeight,
      fps: out.framesPerSecond,
      kbps: kbpsBetween(out, v.prev, 'bytesSent'),
      codec,
      hw: encoderIsHw(out),
      impl: out.encoderImplementation,
      encodeMs: dFrames ? (rate(out, v.prev, 'totalEncodeTime') / dFrames) * 1000 : v.stats?.encodeMs ?? null,
      limit: out.qualityLimitationReason,
      rtt: pair?.currentRoundTripTime != null ? pair.currentRoundTripTime * 1000 : (remoteIn?.roundTripTime ?? NaN) * 1000,
      relay: isRelayed(report, pair),
    };
    v.prev = out;
    checkHardware(v, out, codec);
    if (host.tick % 2 === 0) {
      const { encodeMs, hw } = v.stats;
      host.signal?.send({ t: 'signal', to: v.id, data: { type: 'stats', session: v.session, encodeMs, hw, encoder: hw ? gpu.encoderName : null, codec } });
    }
  }
  host.tick++;
  renderViewers();
  renderEncoderStatus();
}

function hwTip() {
  if (IS_MAC) return 'Macen a H.264-et a gép hardveresen kódolja – a Kodek legyen „Automatikus” vagy „H.264”.';
  const driver = gpu.vendor === 'nvidia' ? 'az NVIDIA illesztőprogramot' : 'a videókártya illesztőprogramját';
  return `Chrome/Edge → Beállítások → Rendszer → „Grafikus gyorsítás használata” legyen bekapcsolva, és frissítsd ${driver}.`;
}

function renderEncoderStatus() {
  $('#gpuName').textContent = `Videókártya: ${gpu.name || 'ismeretlen'}`;
  const el = $('#encoderStatus');
  const sending = [...host.viewers.values()].map((v) => v.stats).filter(Boolean);
  const active = sending.filter((x) => x.hw != null);
  if (!isLive() || !sending.length) {
    el.textContent = 'Az első néző csatlakozásakor kiderül, hogy a videókártya kódol-e.';
    el.className = 'hint';
    return;
  }
  if (!active.length) {
    const x = sending[0];
    const ms = x.encodeMs != null ? ` · ${nf1.format(x.encodeMs)} ms/kép` : '';
    el.textContent = `Kódolás: ${CODEC_LABEL[x.codec] || x.codec || '?'}${ms} – ez a böngésző nem árulja el, hogy a videókártya kódol-e.`;
    el.className = 'hint';
    return;
  }
  const hw = active.filter((x) => x.hw);
  const sample = hw[0] || active[0];
  const codec = CODEC_LABEL[sample.codec] || sample.codec || '';
  const ms = sample.encodeMs != null ? ` · ${nf1.format(sample.encodeMs)} ms/kép` : '';
  if (hw.length === active.length) {
    el.textContent = `Hardveres kódolás: ${gpu.encoderName} · ${codec}${ms}`;
    el.className = 'hint ok';
  } else if (hw.length) {
    const limit = gpu.vendor === 'nvidia' ? ' (a GeForce kártyák egyszerre kb. 8 hardveres kódolást bírnak)' : '';
    el.textContent = `${hw.length}/${active.length} néző hardveresen kódolva (${gpu.encoderName}), a többinél a processzor kódol${limit}.`;
    el.className = 'hint warn';
  } else {
    el.textContent = `Szoftveres kódolás (${codec}${ms}): a processzor kódol, ez lassabb és több késést okoz. ${hwTip()}`;
    el.className = 'hint warn';
  }
}

const LIMIT_LABEL = { cpu: 'CPU', bandwidth: 'sávszélesség', other: 'egyéb' };

function renderViewers() {
  const rows = [];
  for (const v of host.viewers.values()) {
    const li = document.createElement('li');
    const head = document.createElement('div');
    head.className = 'viewer-head';
    const name = document.createElement('span');
    name.textContent = `Néző #${v.n}`;
    const state = document.createElement('span');
    const cs = v.pc?.connectionState;
    if (!isLive()) { state.textContent = 'vár (szünet)'; state.className = 'viewer-state'; }
    else if (cs === 'connected') { state.textContent = v.stats?.relay ? 'kapcsolódva · TURN relé' : 'kapcsolódva · közvetlen'; state.className = 'viewer-state ok'; }
    else if (cs === 'failed' || cs === 'disconnected') { state.textContent = 'kapcsolat gond…'; state.className = 'viewer-state warn'; }
    else { state.textContent = 'kapcsolódik…'; state.className = 'viewer-state'; }
    head.append(name, state);
    li.append(head);

    const s = v.stats;
    if (s && isLive()) {
      const stats = document.createElement('div');
      stats.className = 'viewer-stats';
      const codec = CODEC_LABEL[s.codec] || s.codec || CODEC_LABEL[v.codec] || '';
      const enc = s.hw === true ? ` · ${gpu.encoderName}` : s.hw === false ? ' · szoftveres' : '';
      const line1 = document.createElement('div');
      line1.textContent = `${s.w || '?'}×${s.h || '?'} · ${s.fps != null ? Math.round(s.fps) : '–'} FPS · ${fmtKbps(s.kbps)}`;
      const line2 = document.createElement('div');
      line2.textContent = `${codec}${enc} · RTT ${fmtMs(s.rtt)}`;
      if (s.impl) line2.title = s.impl;
      stats.append(line1, line2);
      if (v.remote?.lat != null) {
        const total = v.remote.lat + (s.encodeMs || 0);
        const line = document.createElement('div');
        line.textContent = `Késés: ~${Math.round(total)} ms`;
        if (total > 150) line.className = 'warn';
        stats.append(line);
      }
      if (s.limit && s.limit !== 'none') {
        const line3 = document.createElement('div');
        line3.className = 'warn';
        line3.textContent = `Korlátozza: ${LIMIT_LABEL[s.limit] || s.limit}`;
        stats.append(line3);
      }
      li.append(stats);
    }
    rows.push(li);
  }
  $('#viewerList').replaceChildren(...rows);
  $('#viewerCount').textContent = String(host.viewers.size);
  $('#noViewers').hidden = host.viewers.size > 0;
}

function togglePreview() {
  host.previewOn = !host.previewOn;
  $('#btnPreview').textContent = host.previewOn ? 'Előnézet elrejtése' : 'Előnézet mutatása';
  $('#previewHidden').hidden = host.previewOn;
  $('#preview').srcObject = host.previewOn && host.video ? new MediaStream([host.video]) : null;
}

async function copyLink() {
  const input = $('#roomLink');
  try {
    await navigator.clipboard.writeText(input.value);
  } catch {
    input.select();
    document.execCommand('copy');
  }
  toast('Link kimásolva – küldd el a nézőknek!', 'info', 2500);
}

function initHostUi() {
  const fields = { setRes: 'res', setFps: 'fps', setKbps: 'kbps', setMode: 'mode', setCodec: 'codec' };
  for (const [id, key] of Object.entries(fields)) {
    const el = $(`#${id}`);
    el.value = String(settings[key]);
    if (el.value !== String(settings[key])) {
      settings[key] = DEFAULT_SETTINGS[key];
      el.value = String(settings[key]);
    }
    el.addEventListener('change', () => updateSetting(key, typeof DEFAULT_SETTINGS[key] === 'number' ? Number(el.value) : el.value));
  }
  const audio = $('#setAudio');
  audio.checked = settings.audio;
  audio.addEventListener('change', () => updateSetting('audio', audio.checked));

  const saved = store.get('ss-host-password', '');
  if (saved) $('#hostPassword').value = saved;

  $('#shareForm').addEventListener('submit', startHosting);
  $('#btnSwitch').addEventListener('click', switchSource);
  $('#btnResume').addEventListener('click', switchSource);
  $('#btnStop').addEventListener('click', pauseSharing);
  $('#btnPreview').addEventListener('click', togglePreview);
  $('#btnClose').addEventListener('click', closeRoom);
  $('#btnCopy').addEventListener('click', copyLink);
  $('#roomLink').addEventListener('focus', (e) => e.target.select());

  if (!navigator.mediaDevices?.getDisplayMedia || !window.RTCPeerConnection) {
    $('#btnShare').disabled = true;
    $('#shareUnsupported').hidden = false;
  } else if (IS_MAC) {
    $('#macHint').hidden = false;
  }
}

/* ============================= VIEWER ============================= */

const viewer = {
  signal: null,
  code: null,
  id: null,
  iceServers: [],
  caps: null,
  pc: null,
  session: null,
  pendingCandidates: [],
  chain: Promise.resolve(),
  closed: false,
  live: false,
  retryTimer: null,
  failTimer: null,
  prev: null,
  prevAudio: null,
  statsTimer: null,
  bufferMs: store.get('ss-buffer', 0),
  muted: false,
  wakeLock: null,
  idleTimer: null,
  hostStats: null, // the sender's encoder info (codec, hardware, encode time)
  frameDelays: [], // receive → display time of recent frames (ms)
  frameTimer: null,
  tick: 0,
};

const video = () => $('#remoteVideo');

// Per-frame receive → display delay (jitter buffer + decode + render), measured by the browser.
function startFrameTiming() {
  const el = video();
  if (viewer.frameTimer != null || !el.requestVideoFrameCallback) return;
  const onFrame = (_now, md) => {
    if (md.receiveTime != null && md.expectedDisplayTime != null) {
      viewer.frameDelays.push(md.expectedDisplayTime - md.receiveTime);
      if (viewer.frameDelays.length > 60) viewer.frameDelays.shift();
    }
    viewer.frameTimer = el.requestVideoFrameCallback(onFrame);
  };
  viewer.frameTimer = el.requestVideoFrameCallback(onFrame);
}

function median(list) {
  if (!list.length) return null;
  const sorted = [...list].sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

async function startViewing(code) {
  showView('view-viewer');
  $('#viewerRoom').textContent = code;
  if (location.hash.slice(1) !== code) history.replaceState(null, '', `#${code}`);
  if (!window.RTCPeerConnection) {
    setOverlay('Ez a böngésző nem támogatja a WebRTC-t. Használj friss Chrome-ot, Edge-et, Firefoxot vagy Safarit.', { spinner: false });
    return;
  }
  viewer.code = code;
  setOverlay('Csatlakozás a szobához…');
  viewer.caps = await Promise.race([codecSupport('decode'), sleep(1500).then(() => null)]);
  viewer.signal = createSignal({
    onMessage: onViewerMessage,
    onOpen: joinRoom,
    onClose: () => {
      if (!viewer.closed && !hasPicture()) setOverlay('Megszakadt a kapcsolat a szerverrel, újracsatlakozás…');
    },
  });
  viewer.statsTimer = setInterval(viewerStatsTick, 1000);
  window.addEventListener('pagehide', () => viewer.signal?.send({ t: 'leave' }));
}

async function joinRoom() {
  clearTimeout(viewer.retryTimer);
  if (viewer.closed) return;
  try {
    const res = await viewer.signal.request({ t: 'join', code: viewer.code, caps: viewer.caps }, ['joined']);
    viewer.id = res.id;
    viewer.iceServers = res.iceServers || [];
    viewer.live = Boolean(res.live);
    if (!res.hostPresent) setOverlay('A megosztó épp nincs bent. Várakozás…');
    else if (!viewer.live) setOverlay('A megosztás szünetel. Amint folytatódik, itt lesz a kép.');
    else if (!hasPicture()) setOverlay('Csatlakozva. Kép betöltése…');
  } catch (err) {
    if (err.code === 'offline') return; // reconnect will call joinRoom again
    const delay = err.code === 'rate' ? 30000 : 5000;
    setOverlay(`${errorText(err)} Újrapróbálom…`, { actions: [{ label: 'Vissza a főoldalra', onClick: goHome }] });
    viewer.retryTimer = setTimeout(joinRoom, delay);
  }
}

function onViewerMessage(msg) {
  switch (msg.t) {
    case 'signal':
      if (msg.from === 'host' && msg.data && typeof msg.data === 'object') {
        viewer.chain = viewer.chain.then(() => onHostSignal(msg.data)).catch((err) => console.warn('signal handling failed', err));
      }
      break;
    case 'state':
    case 'host-back':
      viewer.live = Boolean(msg.live);
      if (!viewer.live) setOverlay('A megosztás szünetel. Amint folytatódik, itt lesz a kép.');
      else if (hasPicture()) { hideOverlay(); setLive(true); }
      else setOverlay('Kép betöltése…');
      if (!viewer.live) setLive(false);
      break;
    case 'host-away':
      // Media may still flow peer-to-peer; only cover the picture if it stopped.
      if (!hasPicture() || viewer.pc?.connectionState !== 'connected') setOverlay('A megosztó kapcsolata megszakadt. Várakozás…');
      break;
    case 'count':
      $('#viewerCountText').textContent = `${msg.n} néző`;
      break;
    case 'closed':
      viewer.closed = true;
      clearTimeout(viewer.retryTimer);
      closeViewerConnection();
      setLive(false);
      setOverlay(msg.reason === 'timeout' ? 'A megosztó nem tért vissza, a szoba bezárult.' : 'A megosztó befejezte a megosztást. Köszi, hogy néztél!', {
        spinner: false,
        actions: [{ label: 'Vissza a főoldalra', onClick: goHome, primary: true }],
      });
      viewer.signal?.close();
      break;
    default: break;
  }
}

function sendToHost(data) {
  viewer.signal?.send({ t: 'signal', to: 'host', data: { ...data, session: viewer.session } });
}

function newViewerPc(session) {
  closeViewerConnection();
  viewer.session = session;
  viewer.pendingCandidates = [];
  viewer.prev = null;
  viewer.prevAudio = null;
  viewer.hostStats = null;
  viewer.frameDelays = [];
  const pc = new RTCPeerConnection(rtcConfig(viewer.iceServers));
  viewer.pc = pc;
  pc.ontrack = (e) => {
    if (viewer.pc !== pc) return;
    tuneReceiver(e.receiver);
    const stream = e.streams[0] || (viewer.fallbackStream ||= new MediaStream());
    if (!e.streams[0]) stream.addTrack(e.track);
    attachStream(stream);
  };
  pc.onicecandidate = (e) => {
    if (e.candidate && viewer.pc === pc) sendToHost({ type: 'candidate', candidate: e.candidate.toJSON() });
  };
  pc.onconnectionstatechange = () => {
    if (viewer.pc !== pc) return;
    clearTimeout(viewer.failTimer);
    if (pc.connectionState === 'failed') {
      setOverlay('A videókapcsolat megszakadt. Újrapróbálom…');
      // The host restarts ICE on its own; ask for a brand new connection if that does not help.
      viewer.failTimer = setTimeout(() => {
        if (viewer.pc === pc && pc.connectionState === 'failed') sendToHost({ type: 'recall' });
      }, 8000);
    } else if (pc.connectionState === 'connected') {
      if (!viewer.live) setOverlay('A megosztás szünetel. Amint folytatódik, itt lesz a kép.');
      else if (hasPicture()) hideOverlay();
    }
  };
  return pc;
}

function closeViewerConnection() {
  clearTimeout(viewer.failTimer);
  if (viewer.pc) {
    viewer.pc.ontrack = null;
    viewer.pc.onicecandidate = null;
    viewer.pc.onconnectionstatechange = null;
    viewer.pc.close();
    viewer.pc = null;
  }
  viewer.fallbackStream = null;
}

async function onHostSignal(data) {
  if (data.type === 'offer' && typeof data.sdp === 'string') {
    if (!viewer.pc || viewer.session !== data.session || viewer.pc.signalingState === 'closed') newViewerPc(data.session);
    const pc = viewer.pc;
    await pc.setRemoteDescription({ type: 'offer', sdp: data.sdp });
    for (const c of viewer.pendingCandidates.splice(0)) await pc.addIceCandidate(c).catch(() => {});
    const answer = await pc.createAnswer();
    try {
      await pc.setLocalDescription({ type: 'answer', sdp: mungeSdp(answer.sdp) });
    } catch {
      await pc.setLocalDescription(answer); // a browser that refuses the tuned SDP still gets audio
    }
    if (viewer.pc === pc) sendToHost({ type: 'answer', sdp: pc.localDescription.sdp });
  } else if (data.type === 'stats') {
    if (data.session === viewer.session) viewer.hostStats = data;
  } else if (data.type === 'candidate' && data.candidate) {
    if (data.session !== viewer.session || !viewer.pc) return;
    if (viewer.pc.remoteDescription) await viewer.pc.addIceCandidate(data.candidate).catch(() => {});
    else viewer.pendingCandidates.push(data.candidate);
  }
}

function tuneReceiver(receiver) {
  // Smallest possible jitter buffer for "Minimal latency", or a fixed extra
  // buffer for smoother playback on unstable networks.
  if ('jitterBufferTarget' in receiver) receiver.jitterBufferTarget = viewer.bufferMs;
  else if ('playoutDelayHint' in receiver) receiver.playoutDelayHint = viewer.bufferMs / 1000;
}

async function attachStream(stream) {
  const el = video();
  if (el.srcObject !== stream) el.srcObject = stream;
  for (const r of viewer.pc?.getReceivers() || []) tuneReceiver(r);
  startFrameTiming();
  await playVideo();
}

async function playVideo() {
  const el = video();
  el.muted = viewer.muted;
  try {
    await el.play();
    $('#btnUnmute').hidden = true;
  } catch {
    // Autoplay with sound blocked: start muted and ask for one click.
    el.muted = true;
    try { await el.play(); } catch { /* will retry on user gesture */ }
    if (!viewer.muted) $('#btnUnmute').hidden = false;
  }
  syncMuteUi();
}

const hasPicture = () => video().readyState >= 2 && video().videoWidth > 0;

function onVideoPlaying() {
  if (viewer.live) { hideOverlay(); setLive(true); }
  requestWakeLock();
}

function setLive(on) {
  const badge = $('#viewerLive');
  badge.hidden = false;
  badge.classList.toggle('off', !on);
  badge.textContent = on ? 'ÉLŐ' : 'NINCS KÉP';
}

function setOverlay(text, { spinner = true, actions = [] } = {}) {
  $('#overlay').hidden = false;
  $('#spinner').hidden = !spinner;
  $('#overlayText').textContent = text;
  $('#overlayActions').replaceChildren(...actions.map((a) => {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = `btn ${a.primary ? 'btn-primary' : ''}`;
    b.textContent = a.label;
    b.addEventListener('click', a.onClick);
    return b;
  }));
  showControls();
}

function hideOverlay() { $('#overlay').hidden = true; }
function goHome() { location.href = location.pathname; }

async function requestWakeLock() {
  try {
    if ('wakeLock' in navigator && !viewer.wakeLock && document.visibilityState === 'visible') {
      viewer.wakeLock = await navigator.wakeLock.request('screen');
      viewer.wakeLock.addEventListener('release', () => { viewer.wakeLock = null; });
    }
  } catch { /* not allowed – fine */ }
}

async function viewerStatsTick() {
  const pc = viewer.pc;
  if (!pc || pc.connectionState === 'closed') { $('#viewerInfo').textContent = ''; return; }
  let report;
  try { report = await pc.getStats(); } catch { return; }
  let v = null;
  let a = null;
  report.forEach((s) => {
    if (s.type === 'inbound-rtp' && s.kind === 'video') v = s;
    if (s.type === 'inbound-rtp' && s.kind === 'audio') a = s;
  });
  if (!v) return;
  const p = viewer.prev;
  const pair = selectedPair(report);
  const rtt = pair?.currentRoundTripTime != null ? pair.currentRoundTripTime * 1000 : null;
  const dEmitted = rate(v, p, 'jitterBufferEmittedCount');
  const jb = dEmitted ? (rate(v, p, 'jitterBufferDelay') / dEmitted) * 1000 : null;
  const dDecoded = rate(v, p, 'framesDecoded');
  const decode = dDecoded ? (rate(v, p, 'totalDecodeTime') / dDecoded) * 1000 : null;
  const dLost = rate(v, p, 'packetsLost') ?? 0;
  const dRecv = rate(v, p, 'packetsReceived') ?? 0;
  const loss = dRecv + dLost > 0 ? (dLost / (dRecv + dLost)) * 100 : 0;
  // network (RTT/2) + receive→display as measured per frame (fallback: jitter buffer + decode)
  const toDisplay = median(viewer.frameDelays) ?? (jb != null ? jb + (decode || 0) : null);
  const netLatency = rtt != null && toDisplay != null ? rtt / 2 + toDisplay : null;
  const encodeMs = Number.isFinite(viewer.hostStats?.encodeMs) ? viewer.hostStats.encodeMs : 0;
  const latency = netLatency != null ? netLatency + encodeMs : null;
  const codec = report.get(v.codecId)?.mimeType?.split('/')[1]?.toUpperCase();
  const fps = v.framesPerSecond;
  const kbps = kbpsBetween(v, p, 'bytesReceived');
  const audioKbps = kbpsBetween(a, viewer.prevAudio, 'bytesReceived');
  const relay = isRelayed(report, pair);
  viewer.prev = v;
  viewer.prevAudio = a;
  if (viewer.tick++ % 2 === 0 && netLatency != null) {
    sendToHost({ type: 'stats', lat: Math.round(netLatency), fps: fps != null ? Math.round(fps) : null, loss: Math.round(loss * 10) / 10 });
  }

  const res = v.frameWidth && v.frameHeight ? `${v.frameWidth}×${v.frameHeight}` : '–';
  $('#viewerInfo').textContent = hasPicture()
    ? `${res} · ${fps != null ? Math.round(fps) : '–'} FPS${latency != null ? ` · ~${Math.round(latency)} ms` : ''}`
    : '';

  if ($('#stats').hidden) return;
  const hw = v.powerEfficientDecoder === true ? ' (HW)' : v.powerEfficientDecoder === false ? ' (SW)' : '';
  const rows = [
    ['Felbontás', res],
    ['FPS', fps != null ? String(Math.round(fps)) : '–'],
    ['Bitráta', fmtKbps(kbps)],
    ['Kodek', codec ? `${CODEC_LABEL[codec] || codec}${hw}` : '–'],
    ['Küldő kódolója', senderEncoderText(viewer.hostStats), viewer.hostStats?.hw === false],
    ['Becsült késés', latency != null ? `~${Math.round(latency)} ms` : '–', latency > 150],
    ['RTT', fmtMs(rtt)],
    ['Fogadás → kép', fmtMs(toDisplay)],
    ['Jitter puffer', fmtMs(jb)],
    ['Dekódolás', decode != null ? `${nf1.format(decode)} ms` : '–'],
    ['Csomagvesztés', `${nf1.format(loss)} %`, loss > 2],
    ['Eldobott képk.', String(v.framesDropped ?? 0)],
    ['Kapcsolat', relay == null ? '–' : relay ? 'TURN relé' : 'Közvetlen P2P'],
    ['Hang', a && audioKbps ? fmtKbps(audioKbps) : 'nincs'],
  ];
  const dl = document.createElement('dl');
  for (const [k, val, warn] of rows) {
    const dt = document.createElement('dt');
    dt.textContent = k;
    const dd = document.createElement('dd');
    dd.textContent = val;
    if (warn) dd.className = 'warn';
    dl.append(dt, dd);
  }
  $('#stats').replaceChildren(dl);
}

function senderEncoderText(st) {
  if (!st) return '–';
  const name = st.hw === true ? st.encoder || 'hardveres' : st.hw === false ? 'szoftveres' : 'ismeretlen';
  return `${name}${Number.isFinite(st.encodeMs) ? ` · ${nf1.format(st.encodeMs)} ms` : ''}`;
}

function syncMuteUi() {
  const el = video();
  $('#btnMute').classList.toggle('muted', el.muted || el.volume === 0);
  $('#volume').value = String(el.muted ? 0 : el.volume);
}

function toggleMute() {
  const el = video();
  viewer.muted = !el.muted;
  if (!viewer.muted && el.volume === 0) el.volume = 1;
  el.muted = viewer.muted;
  $('#btnUnmute').hidden = true;
  if (el.paused) playVideo(); else syncMuteUi();
}

function toggleFullscreen() {
  const player = $('#player');
  const el = video();
  if (document.fullscreenElement || document.webkitFullscreenElement) {
    (document.exitFullscreen || document.webkitExitFullscreen).call(document);
  } else if (player.requestFullscreen) {
    player.requestFullscreen({ navigationUI: 'hide' }).catch(() => {});
  } else if (player.webkitRequestFullscreen) {
    player.webkitRequestFullscreen();
  } else if (el.webkitEnterFullscreen) {
    el.webkitEnterFullscreen(); // iOS
  }
}

function toggleStats() {
  const box = $('#stats');
  box.hidden = !box.hidden;
  $('#btnStats').setAttribute('aria-pressed', String(!box.hidden));
  if (!box.hidden) viewerStatsTick();
}

function showControls() {
  const player = $('#player');
  player.classList.add('ui-visible');
  player.classList.remove('idle');
  clearTimeout(viewer.idleTimer);
  viewer.idleTimer = setTimeout(() => {
    if (!$('#overlay').hidden || !$('#btnUnmute').hidden) return;
    player.classList.remove('ui-visible');
    player.classList.add('idle');
  }, 2500);
}

function initViewerUi() {
  const el = video();
  const player = $('#player');
  el.addEventListener('playing', onVideoPlaying);
  el.addEventListener('resize', () => { if (viewer.live && hasPicture()) { hideOverlay(); setLive(true); } });
  el.addEventListener('volumechange', syncMuteUi);
  el.addEventListener('dblclick', toggleFullscreen);
  player.addEventListener('pointermove', showControls);
  player.addEventListener('pointerdown', showControls);

  const vol = $('#volume');
  vol.value = String(store.get('ss-volume', 1));
  el.volume = Number(vol.value);
  vol.addEventListener('input', () => {
    el.volume = Number(vol.value);
    el.muted = viewer.muted = el.volume === 0;
    store.set('ss-volume', el.volume);
    if (!el.muted) $('#btnUnmute').hidden = true;
  });

  const lat = $('#latency');
  lat.value = String(viewer.bufferMs);
  if (lat.value !== String(viewer.bufferMs)) { viewer.bufferMs = 0; lat.value = '0'; }
  lat.addEventListener('change', () => {
    viewer.bufferMs = Number(lat.value);
    store.set('ss-buffer', viewer.bufferMs);
    for (const r of viewer.pc?.getReceivers() || []) tuneReceiver(r);
  });

  $('#btnMute').addEventListener('click', toggleMute);
  $('#btnFullscreen').addEventListener('click', toggleFullscreen);
  $('#btnStats').addEventListener('click', toggleStats);
  $('#btnUnmute').addEventListener('click', () => {
    viewer.muted = false;
    el.muted = false;
    if (el.volume === 0) el.volume = 1;
    $('#btnUnmute').hidden = true;
    el.play().catch(() => {});
    syncMuteUi();
  });

  document.addEventListener('keydown', (e) => {
    if ($('#view-viewer').hidden || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.target instanceof HTMLInputElement || e.target instanceof HTMLSelectElement) return;
    const k = e.key.toLowerCase();
    if (k === 'f') toggleFullscreen();
    else if (k === 'm') toggleMute();
    else if (k === 's') toggleStats();
    else return;
    e.preventDefault();
  });
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible' && !el.paused) requestWakeLock(); });
}

/* ============================== boot ============================== */

function init() {
  initHostUi();
  initViewerUi();

  $('#joinForm').addEventListener('submit', (e) => {
    e.preventDefault();
    const code = parseCode($('#joinCode').value);
    if (code.length < 4) { toast('Érvénytelen szobakód.', 'warn'); return; }
    startViewing(code);
  });

  window.addEventListener('hashchange', () => { if (!host.active) location.reload(); });

  const code = parseCode(location.hash);
  if (code) startViewing(code);
  else showView('view-home');
}

if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
else init();
