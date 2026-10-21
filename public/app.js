'use strict';

/* =====================================================================
 * mozizás – low-latency 4K screen sharing over WebRTC (PeerJS signaling)
 *
 * Topology: the host keeps one RTCPeerConnection per viewer (P2P mesh).
 * Each viewer opens a PeerJS DataConnection to the host for control
 * messages; the host then places a PeerJS MediaConnection (the call) to
 * the viewer with the screen + system audio tracks.
 * ===================================================================== */

const CONFIG = {
  idPrefix: 'mozizas-',
  // Add your own TURN server here if some viewers can't connect directly.
  iceServers: [
    { urls: ['stun:stun.l.google.com:19302', 'stun:stun1.l.google.com:19302'] },
    { urls: 'stun:stun.cloudflare.com:3478' },
    { urls: ['turn:eu-0.turn.peerjs.com:3478', 'turn:us-0.turn.peerjs.com:3478'], username: 'peerjs', credential: 'peerjsp' },
  ],
};

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
const CODE_ALPHABET = 'abcdefghjkmnpqrstuvwxyz23456789';

/* ---------------------------- helpers ---------------------------- */

const $ = (sel) => document.querySelector(sel);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const store = {
  get(key, fallback, area = localStorage) {
    try { const v = area.getItem(key); return v == null ? fallback : JSON.parse(v); } catch { return fallback; }
  },
  set(key, value, area = localStorage) {
    try { area.setItem(key, JSON.stringify(value)); } catch { /* storage unavailable */ }
  },
  remove(key, area = localStorage) {
    try { area.removeItem(key); } catch { /* storage unavailable */ }
  },
};

function randomCode(n = 6) {
  const bytes = crypto.getRandomValues(new Uint8Array(n));
  return Array.from(bytes, (b) => CODE_ALPHABET[b % CODE_ALPHABET.length]).join('');
}

function parseCode(input) {
  let s = String(input || '').trim();
  const hash = s.indexOf('#');
  if (hash >= 0) s = s.slice(hash + 1);
  return s.toLowerCase().replace(/[^a-z0-9]/g, '');
}

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

function peerOptions() {
  const opts = {
    debug: 1,
    config: { iceServers: CONFIG.iceServers, bundlePolicy: 'max-bundle', sdpSemantics: 'unified-plan' },
  };
  // Optional self-hosted PeerJS server: ?server=https://peer.example.com:443/
  const server = new URLSearchParams(location.search).get('server');
  if (server) {
    try {
      const u = new URL(server.includes('://') ? server : `https://${server}`);
      opts.host = u.hostname;
      opts.secure = u.protocol === 'https:' || u.protocol === 'wss:';
      opts.port = Number(u.port) || (opts.secure ? 443 : 80);
      opts.path = u.pathname || '/';
    } catch {
      console.warn('Invalid ?server= parameter, using the PeerJS cloud server.');
    }
  }
  return opts;
}

function createPeer(id) {
  return new Promise((resolve, reject) => {
    const peer = new Peer(id, peerOptions());
    const onError = (err) => { peer.destroy(); reject(err); };
    peer.once('open', () => { peer.off('error', onError); resolve(peer); });
    peer.once('error', onError);
  });
}

function keepSignaling(peer, onStatus) {
  let timer = null;
  peer.on('disconnected', () => {
    onStatus?.(false);
    clearInterval(timer);
    timer = setInterval(() => {
      if (peer.destroyed || !peer.disconnected) { clearInterval(timer); return; }
      try { peer.reconnect(); } catch { /* retry on next tick */ }
    }, 3000);
  });
  peer.on('open', () => { clearInterval(timer); onStatus?.(true); });
}

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

// A silent placeholder keeps an audio transceiver in every call, so system
// audio can later be swapped in with replaceTrack() without renegotiation.
let silentTrack = null;
function getSilentTrack() {
  if (silentTrack && silentTrack.readyState === 'live') return silentTrack;
  const ctx = ensureAudioCtx();
  silentTrack = ctx ? ctx.createMediaStreamDestination().stream.getAudioTracks()[0] : null;
  return silentTrack;
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
 * Tunes an SDP blob:
 *  - Opus: stereo, 510 kbit/s, no DTX, in-band FEC.
 *  - video: puts `preferCodec` first, and (in the viewer's answer) raises the
 *    start bitrate and lifts libwebrtc's 2.5 Mbit/s default ceiling; the real
 *    cap is applied with RTCRtpSender.setParameters() on the host.
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
  const bytes = rate(cur, prev, key);
  const ms = cur && prev ? cur.timestamp - prev.timestamp : 0;
  return bytes != null && ms > 0 ? (bytes * 8) / ms : null;
};

/* ============================== HOST ============================== */

const settings = { ...DEFAULT_SETTINGS, ...store.get('mozizas-settings', {}) };

const host = {
  peer: null,
  code: null,
  video: null,
  audio: null,
  viewers: new Map(),
  seq: 0,
  encCaps: null,
  statsTimer: null,
  meter: null,
  dims: '',
  previewOn: true,
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

async function startHosting() {
  if (!window.Peer) { toast('Nem sikerült betölteni a kapcsolódási modult. Frissítsd az oldalt.', 'error'); return; }
  ensureAudioCtx(); // must happen inside the click gesture
  const encCaps = host.encCaps ? Promise.resolve(host.encCaps) : codecSupport('encode');

  let tracks;
  try {
    tracks = await captureScreen();
  } catch (err) {
    if (err.name !== 'NotAllowedError' && err.name !== 'AbortError') toast(`A megosztás nem indult el: ${err.message}`, 'error');
    return;
  }

  showView('view-host');
  window.addEventListener('beforeunload', confirmLeave);
  host.encCaps = await encCaps;
  updateCodecOptions();
  setHostTracks(tracks);
  if (!host.statsTimer) host.statsTimer = setInterval(hostStatsTick, 1000);

  while (!host.peer) {
    try {
      await openHostPeer();
    } catch (err) {
      setSignalStatus(`Nem sikerült kapcsolódni a szerverhez (${err.type || err.message}). Újrapróbálom…`, 'warn');
      await sleep(5000);
    }
  }
}

async function openHostPeer() {
  let code = store.get('mozizas-room', null, sessionStorage) || randomCode();
  for (let attempt = 0; ; attempt++) {
    try {
      host.peer = await createPeer(CONFIG.idPrefix + code);
      break;
    } catch (err) {
      // After a page reload the old ID may still be held for a moment.
      if (err.type !== 'unavailable-id' || attempt >= 3) throw err;
      if (attempt >= 1) code = randomCode();
      await sleep(1500);
    }
  }
  host.code = code;
  store.set('mozizas-room', code, sessionStorage);

  const peer = host.peer;
  peer.on('connection', onViewerConnection);
  peer.on('call', (call) => call.close());
  peer.on('error', (err) => {
    if (err.type === 'peer-unavailable') return; // a viewer left before we called
    console.warn('PeerJS:', err.type, err);
  });
  keepSignaling(peer, (online) => setSignalStatus(
    online ? 'Kapcsolódva. A nézők a linkkel vagy a kóddal csatlakozhatnak.' : 'Szerverkapcsolat megszakadt, újrapróbálom… (a már bent lévő nézők látnak tovább)',
    online ? 'ok' : 'warn',
  ));

  const link = `${location.origin}${location.pathname}${location.search}#${code}`;
  $('#roomCode').textContent = code;
  $('#roomLink').value = link;
  $('#btnCopy').disabled = false;
  setSignalStatus('Kapcsolódva. A nézők a linkkel vagy a kóddal csatlakozhatnak.', 'ok');
}

function setSignalStatus(text, kind = '') {
  const el = $('#signalStatus');
  el.textContent = text;
  el.className = `hint ${kind}`;
}

function onViewerConnection(conn) {
  conn.on('open', () => {
    const prev = host.viewers.get(conn.peer); // same viewer reconnecting
    const v = { id: conn.peer, conn, call: null, n: prev?.n ?? ++host.seq, caps: conn.metadata?.caps ?? null, codec: null, prev: null, stats: null, lastCall: 0 };
    host.viewers.set(conn.peer, v);
    prev?.call?.close();
    prev?.conn.close();
    sendTo(v, { type: 'state', live: isLive() });
    if (isLive()) callViewer(v);
    viewersChanged();
  });
  conn.on('data', (msg) => {
    const v = host.viewers.get(conn.peer);
    if (!v || v.conn !== conn || !msg) return;
    if (msg.type === 'bye') dropViewer(conn.peer, conn);
    if (msg.type === 'recall' && isLive() && Date.now() - v.lastCall > 3000) callViewer(v);
  });
  conn.on('close', () => dropViewer(conn.peer, conn));
  conn.on('error', () => dropViewer(conn.peer, conn));
}

function dropViewer(id, conn) {
  const v = host.viewers.get(id);
  if (!v || (conn && v.conn !== conn)) return;
  host.viewers.delete(id);
  v.call?.close();
  v.conn.close();
  viewersChanged();
}

function sendTo(v, msg) {
  try { if (v.conn.open) v.conn.send(msg); } catch { /* channel closing */ }
}
function broadcast(msg) { for (const v of host.viewers.values()) sendTo(v, msg); }

function viewersChanged() {
  broadcast({ type: 'viewers', count: host.viewers.size });
  renderViewers();
}

function callViewer(v) {
  const old = v.call;
  const codec = settings.codec === 'auto' ? pickCodec(host.encCaps, v.caps) : settings.codec;
  const audio = host.audio || getSilentTrack();
  const stream = new MediaStream(audio ? [host.video, audio] : [host.video]);
  const call = host.peer.call(v.id, stream, {
    metadata: { codec, startKbps: Math.min(settings.kbps, START_KBPS_CAP) },
    sdpTransform: (sdp) => mungeSdp(sdp, { preferCodec: codec }),
  });
  if (!call) return;
  v.call = call;
  v.codec = codec;
  v.prev = null;
  v.lastCall = Date.now();
  old?.close();

  const pc = call.peerConnection;
  pc.addEventListener('connectionstatechange', () => {
    if (v.call !== call) return;
    if (pc.connectionState === 'connected') applySenderParams(v);
    renderViewers();
  });
  call.on('close', () => { if (v.call === call) { v.call = null; renderViewers(); } });
  call.on('error', (err) => console.warn('call error', err));
}

function scaleFor(track) {
  const target = RES_TARGETS[settings.res];
  const s = track?.getSettings?.() || {};
  if (!target || !s.width || !s.height) return 1;
  const [tw, th] = s.width >= s.height ? target : [target[1], target[0]];
  return Math.max(1, s.width / tw, s.height / th);
}

async function applySenderParams(v) {
  const pc = v.call?.peerConnection;
  if (!pc) return;
  for (const t of pc.getTransceivers()) {
    const kind = t.receiver.track?.kind;
    const sender = t.sender;
    const p = sender.getParameters();
    if (!p.encodings?.length) continue; // not negotiated yet
    const enc = p.encodings[0];
    if (kind === 'video') {
      enc.maxBitrate = settings.kbps * 1000;
      enc.maxFramerate = settings.fps;
      enc.scaleResolutionDownBy = scaleFor(host.video);
      p.degradationPreference = DEGRADATION[settings.mode];
    } else {
      enc.maxBitrate = AUDIO_MAX_BPS;
    }
    enc.priority = 'high';
    enc.networkPriority = 'high';
    try {
      await sender.setParameters(p);
    } catch {
      delete p.degradationPreference;
      delete enc.priority;
      delete enc.networkPriority;
      try { await sender.setParameters(p); } catch (err) { console.warn('setParameters failed', err); }
    }
  }
}

function applyAllSenders() {
  for (const v of host.viewers.values()) applySenderParams(v);
}

function setHostTracks({ video, audio }) {
  const old = [host.video, host.audio];
  host.video = video;
  host.audio = audio;
  video.addEventListener('ended', () => { if (host.video === video) pauseSharing(); });

  for (const v of host.viewers.values()) {
    const pc = v.call?.peerConnection;
    if (pc && pc.connectionState !== 'closed' && pc.connectionState !== 'failed') {
      for (const t of pc.getTransceivers()) {
        const kind = t.receiver.track?.kind;
        const track = kind === 'video' ? video : (audio || getSilentTrack());
        t.sender.replaceTrack(track).catch((err) => console.warn('replaceTrack failed', err));
      }
      applySenderParams(v);
    } else {
      callViewer(v);
    }
  }
  for (const t of old) if (t && t !== video && t !== audio) t.stop();

  host.dims = '';
  if (host.previewOn) $('#preview').srcObject = new MediaStream([video]);
  $('#previewPaused').hidden = true;
  $('#liveBadge').classList.remove('off');
  $('#liveBadge').textContent = 'ÉLŐ';
  $('#btnStop').hidden = false;
  broadcast({ type: 'state', live: true });
  startMeter(audio);
  updateCaptureInfo();
}

function pauseSharing() {
  host.video?.stop();
  host.audio?.stop();
  host.video = null;
  host.audio = null;
  stopMeter();
  $('#preview').srcObject = null;
  $('#previewPaused').hidden = false;
  $('#liveBadge').classList.add('off');
  $('#liveBadge').textContent = 'SZÜNETEL';
  $('#btnStop').hidden = true;
  $('#captureInfo').textContent = '';
  $('#audioStatus').textContent = '';
  broadcast({ type: 'state', live: false });
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
  broadcast({ type: 'closed' });
  window.removeEventListener('beforeunload', confirmLeave);
  store.remove('mozizas-room', sessionStorage);
  setTimeout(() => {
    host.video?.stop();
    host.audio?.stop();
    host.peer?.destroy();
    location.href = location.pathname + location.search;
  }, 300);
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
  if (host.audio) {
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
  store.set('mozizas-settings', settings);
  if (key === 'fps' && isLive()) {
    host.video.applyConstraints({ frameRate: { ideal: value, max: value } }).catch(() => {});
  }
  if (key === 'mode' && host.video) host.video.contentHint = CONTENT_HINT[value];
  if (key === 'codec') {
    if (isLive()) for (const v of host.viewers.values()) callViewer(v);
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
  const dims = (() => { const s = host.video?.getSettings?.() || {}; return `${s.width}x${s.height}`; })();
  if (isLive() && dims !== host.dims) {
    host.dims = dims;
    applyAllSenders();
    updateCaptureInfo();
  }

  for (const v of host.viewers.values()) {
    const pc = v.call?.peerConnection;
    if (!pc || pc.connectionState !== 'connected') { v.stats = null; continue; }
    let report;
    try { report = await pc.getStats(); } catch { continue; }
    let out = null;
    let remoteIn = null;
    report.forEach((s) => {
      if (s.type === 'outbound-rtp' && s.kind === 'video') out = s;
      if (s.type === 'remote-inbound-rtp' && s.kind === 'video') remoteIn = s;
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
  const list = $('#viewerList');
  const rows = [];
  for (const v of host.viewers.values()) {
    const li = document.createElement('li');
    const head = document.createElement('div');
    head.className = 'viewer-head';
    const name = document.createElement('span');
    name.textContent = `Néző #${v.n}`;
    const state = document.createElement('span');
    const cs = v.call?.peerConnection?.connectionState;
    if (!isLive()) { state.textContent = 'vár (szünet)'; state.className = 'viewer-state'; }
    else if (cs === 'connected') { state.textContent = v.stats?.relay ? 'kapcsolódva · TURN' : 'kapcsolódva · közvetlen'; state.className = 'viewer-state ok'; }
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
  list.replaceChildren(...rows);
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

  $('#btnShare').addEventListener('click', startHosting);
  $('#btnSwitch').addEventListener('click', switchSource);
  $('#btnResume').addEventListener('click', switchSource);
  $('#btnStop').addEventListener('click', pauseSharing);
  $('#btnPreview').addEventListener('click', togglePreview);
  $('#btnClose').addEventListener('click', closeRoom);
  $('#btnCopy').addEventListener('click', copyLink);
  $('#roomLink').addEventListener('focus', (e) => e.target.select());

  if (!navigator.mediaDevices?.getDisplayMedia) {
    $('#btnShare').disabled = true;
    $('#shareUnsupported').hidden = false;
  }
}

/* ============================= VIEWER ============================= */

const viewer = {
  peer: null,
  code: null,
  hostId: null,
  conn: null,
  call: null,
  caps: null,
  closed: false,
  live: false,
  attempts: 0,
  retryTimer: null,
  prev: null,
  prevAudio: null,
  statsTimer: null,
  bufferMs: store.get('mozizas-buffer', 0),
  muted: false,
  wakeLock: null,
  idleTimer: null,
};

const video = () => $('#remoteVideo');

async function startViewing(code) {
  showView('view-viewer');
  $('#viewerRoom').textContent = code;
  if (!window.Peer) { setOverlay('Nem sikerült betölteni a kapcsolódási modult. Frissítsd az oldalt.', { spinner: false, actions: [reloadAction()] }); return; }
  if (location.hash.slice(1) !== code) history.replaceState(null, '', `#${code}`);
  viewer.code = code;
  viewer.hostId = CONFIG.idPrefix + code;
  setOverlay('Csatlakozás a szobához…');

  viewer.caps = await Promise.race([codecSupport('decode'), sleep(1500).then(() => null)]);
  try {
    viewer.peer = await createPeer(`${CONFIG.idPrefix}v-${randomCode(10)}`);
  } catch (err) {
    setOverlay(`Nem sikerült kapcsolódni a szerverhez (${err.type || err.message}).`, { spinner: false, actions: [reloadAction()] });
    return;
  }
  viewer.peer.on('call', onHostCall);
  viewer.peer.on('error', onViewerPeerError);
  keepSignaling(viewer.peer);
  window.addEventListener('pagehide', () => {
    try { if (viewer.conn?.open) viewer.conn.send({ type: 'bye' }); } catch { /* closing */ }
  });
  viewer.statsTimer = setInterval(viewerStatsTick, 1000);
  connectToHost();
}

function connectToHost() {
  clearTimeout(viewer.retryTimer);
  if (viewer.closed || !viewer.peer || viewer.peer.destroyed) return;
  const old = viewer.conn;
  viewer.conn = null; // detach first so the old connection's 'close' is ignored
  old?.close();
  const conn = viewer.peer.connect(viewer.hostId, { reliable: true, serialization: 'json', metadata: { caps: viewer.caps } });
  viewer.conn = conn;
  let opened = false;
  const openTimeout = setTimeout(() => { if (viewer.conn === conn && !opened) scheduleReconnect(); }, 15000);
  const onGone = () => {
    clearTimeout(openTimeout);
    if (viewer.conn !== conn) return;
    if (opened) hostLost(); else scheduleReconnect();
  };
  conn.on('open', () => {
    clearTimeout(openTimeout);
    if (viewer.conn !== conn) return;
    opened = true;
    viewer.attempts = 0;
    if (!viewer.call) setOverlay('Csatlakozva. Várakozás a képre…');
  });
  conn.on('data', onHostMessage);
  conn.on('close', onGone);
  conn.on('error', onGone);
}

function scheduleReconnect() {
  if (viewer.closed) return;
  clearTimeout(viewer.retryTimer);
  const delay = Math.min(10000, 2000 + viewer.attempts * 1000);
  viewer.attempts++;
  viewer.retryTimer = setTimeout(connectToHost, delay);
}

function hostLost() {
  if (viewer.closed) return;
  viewer.call?.close();
  viewer.call = null;
  setLive(false);
  setOverlay('A kapcsolat megszakadt. Újracsatlakozás…');
  scheduleReconnect();
}

function onViewerPeerError(err) {
  if (err.type === 'peer-unavailable') {
    if (viewer.closed) return;
    setOverlay('A szoba nem található, vagy a megosztó még nem indította el. Folyamatosan újrapróbálom…', {
      actions: [{ label: 'Vissza a főoldalra', onClick: goHome }],
    });
    scheduleReconnect();
    return;
  }
  console.warn('PeerJS:', err.type, err);
}

function onHostCall(call) {
  if (call.peer !== viewer.hostId) { call.close(); return; }
  const old = viewer.call;
  viewer.call = call;
  viewer.prev = null;
  viewer.prevAudio = null;
  old?.close();

  const meta = call.metadata || {};
  call.answer(undefined, { sdpTransform: (sdp) => mungeSdp(sdp, { preferCodec: meta.codec, startKbps: meta.startKbps }) });
  const pc = call.peerConnection;
  pc.addEventListener('track', (e) => tuneReceiver(e.receiver));
  pc.addEventListener('connectionstatechange', () => {
    if (viewer.call !== call) return;
    if (pc.connectionState === 'failed') {
      setOverlay('A videókapcsolat megszakadt. Újrapróbálom…');
      try { if (viewer.conn?.open) viewer.conn.send({ type: 'recall' }); } catch { /* closing */ }
    }
  });
  call.on('stream', (stream) => { if (viewer.call === call) attachStream(stream); });
  call.on('close', () => { if (viewer.call === call) viewer.call = null; });
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
  for (const r of viewer.call?.peerConnection?.getReceivers() || []) tuneReceiver(r);
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

function onHostMessage(msg) {
  if (!msg || typeof msg !== 'object') return;
  if (msg.type === 'state') {
    viewer.live = Boolean(msg.live);
    if (!viewer.live) setOverlay('A megosztás szünetel. Amint folytatódik, itt lesz a kép.');
    else if (hasPicture()) hideOverlay();
    else setOverlay('Kép betöltése…');
    setLive(viewer.live && hasPicture());
  } else if (msg.type === 'viewers') {
    $('#viewerCountText').textContent = `${msg.count} néző`;
  } else if (msg.type === 'closed') {
    viewer.closed = true;
    clearTimeout(viewer.retryTimer);
    viewer.call?.close();
    setLive(false);
    setOverlay('A megosztó bezárta a szobát. Köszi, hogy néztél!', { spinner: false, actions: [{ label: 'Vissza a főoldalra', onClick: goHome, primary: true }] });
    setTimeout(() => viewer.peer?.destroy(), 500);
  }
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
  const box = $('#overlayActions');
  box.replaceChildren(...actions.map((a) => {
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
const reloadAction = () => ({ label: 'Újrapróbálás', onClick: () => location.reload(), primary: true });
function goHome() { location.href = location.pathname + location.search; }

async function requestWakeLock() {
  try {
    if ('wakeLock' in navigator && !viewer.wakeLock && document.visibilityState === 'visible') {
      viewer.wakeLock = await navigator.wakeLock.request('screen');
      viewer.wakeLock.addEventListener('release', () => { viewer.wakeLock = null; });
    }
  } catch { /* not allowed – fine */ }
}

async function viewerStatsTick() {
  const pc = viewer.call?.peerConnection;
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
    ['Kapcsolat', relay == null ? '–' : relay ? 'TURN relé' : 'Közvetlen P2P', relay === true],
    ['Hang', a ? fmtKbps(audioKbps) : 'nincs'],
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
  vol.value = String(store.get('mozizas-volume', 1));
  el.volume = Number(vol.value);
  vol.addEventListener('input', () => {
    el.volume = Number(vol.value);
    el.muted = viewer.muted = el.volume === 0;
    store.set('mozizas-volume', el.volume);
    if (!el.muted) $('#btnUnmute').hidden = true;
  });

  const lat = $('#latency');
  lat.value = String(viewer.bufferMs);
  if (lat.value !== String(viewer.bufferMs)) { viewer.bufferMs = 0; lat.value = '0'; }
  lat.addEventListener('change', () => {
    viewer.bufferMs = Number(lat.value);
    store.set('mozizas-buffer', viewer.bufferMs);
    for (const r of viewer.call?.peerConnection?.getReceivers() || []) tuneReceiver(r);
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

  window.addEventListener('hashchange', () => location.reload());

  const code = parseCode(location.hash);
  if (code) startViewing(code);
  else showView('view-home');
}

if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
else init();
