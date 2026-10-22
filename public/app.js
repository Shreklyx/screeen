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
const CODECS = ['AV1', 'VP9', 'H264', 'VP8'];
const CODEC_LABEL = { AV1: 'AV1', VP9: 'VP9', H264: 'H.264', VP8: 'VP8' };
const OPUS_PARAMS = {
  stereo: 1, 'sprop-stereo': 1, maxaveragebitrate: 510000, maxplaybackrate: 48000,
  'sprop-maxcapturerate': 48000, useinbandfec: 1, usedtx: 0, cbr: 0,
};
const AUDIO_MAX_BPS = 510000;
const START_KBPS_CAP = 12000;
const DEFAULT_SETTINGS = { res: 'native', fps: 60, kbps: 30000, mode: 'motion', codec: 'auto', audio: true };

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
// hardware encoder, then the cheapest software codec.
function pickCodec(enc, dec) {
  const usable = CODECS.filter((c) => isAvailable(enc, c) && isAvailable(dec, c));
  return usable.find((c) => enc?.[c]?.hw && dec?.[c]?.hw)
    || ['H264', 'VP9', 'AV1', 'VP8'].find((c) => usable.includes(c) && enc?.[c]?.hw)
    || ['H264', 'VP8', 'VP9', 'AV1'].find((c) => usable.includes(c))
    || 'VP8';
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
 *  - Opus: stereo, 510 kbit/s, no DTX, in-band FEC.
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
    if (err.name !== 'NotAllowedError' && err.name !== 'AbortError') showShareError(`A megosztás nem indult el: ${err.message}`);
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
    if (err.name !== 'NotAllowedError' && err.name !== 'AbortError') toast(`Nem sikerült forrást váltani: ${err.message}`, 'error');
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
    el.textContent = s.displaySurface === 'window'
      ? 'Nincs hang: ablak megosztásakor a Windows nem ad hangot. Válts „Teljes képernyő”-re, és pipáld be a „Rendszerhang megosztása” opciót.'
      : 'Nincs hang! A Forrás váltása gombbal válaszd újra a képernyőt, és pipáld be a „Rendszerhang megosztása” (vagy lapnál a lap hangja) opciót.';
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
      v.codec = value === 'auto' ? pickCodec(host.encCaps, v.caps) : value;
      applyCodecPreference(v);
      negotiate(v);
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

function updateCodecOptions() {
  for (const opt of $('#setCodec').options) {
    if (opt.value === 'auto') continue;
    const c = host.encCaps?.[opt.value];
    opt.disabled = c ? !c.supported : false;
    opt.textContent = `${CODEC_LABEL[opt.value]}${c?.hw ? ' – hardveres' : ''}${c && !c.supported ? ' (nem támogatott)' : ''}`;
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
    const codec = report.get(out.codecId)?.mimeType?.split('/')[1];
    v.stats = {
      w: out.frameWidth,
      h: out.frameHeight,
      fps: out.framesPerSecond,
      kbps: kbpsBetween(out, v.prev, 'bytesSent'),
      codec: codec ? codec.toUpperCase() : null,
      hw: out.powerEfficientEncoder,
      impl: out.encoderImplementation,
      limit: out.qualityLimitationReason,
      rtt: pair?.currentRoundTripTime != null ? pair.currentRoundTripTime * 1000 : (remoteIn?.roundTripTime ?? NaN) * 1000,
      relay: isRelayed(report, pair),
    };
    v.prev = out;
  }
  renderViewers();
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
      const codec = s.codec || CODEC_LABEL[v.codec] || '';
      const hw = s.hw === true ? ' (HW)' : s.hw === false ? ' (SW)' : '';
      const line1 = document.createElement('div');
      line1.textContent = `${s.w || '?'}×${s.h || '?'} · ${s.fps != null ? Math.round(s.fps) : '–'} FPS · ${fmtKbps(s.kbps)}`;
      const line2 = document.createElement('div');
      line2.textContent = `${codec}${hw} · RTT ${fmtMs(s.rtt)}`;
      if (s.impl) line2.title = s.impl;
      stats.append(line1, line2);
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
};

const video = () => $('#remoteVideo');

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
  const latency = rtt != null && jb != null ? rtt / 2 + jb + (decode || 0) : null;
  const codec = report.get(v.codecId)?.mimeType?.split('/')[1]?.toUpperCase();
  const fps = v.framesPerSecond;
  const kbps = kbpsBetween(v, p, 'bytesReceived');
  const audioKbps = kbpsBetween(a, viewer.prevAudio, 'bytesReceived');
  const relay = isRelayed(report, pair);
  viewer.prev = v;
  viewer.prevAudio = a;

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
    ['Kodek', codec ? `${codec}${hw}` : '–'],
    ['Becsült késés', latency != null ? `~${Math.round(latency)} ms` : '–'],
    ['RTT', fmtMs(rtt)],
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
