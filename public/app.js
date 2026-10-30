'use strict';

/* =====================================================================
 * Screen Share – the website: share a screen with its sound, or watch one, with the least delay.
 *
 * No WebRTC. The picture and the sound go with our own protocol – the one the Windows and Mac
 * apps speak (native/core): H.264 and Opus from WebCodecs, encrypted end to end (X25519, AES-GCM),
 * every frame passed on the moment it is ready, through the server's WebSocket (direct-web.js).
 * What a browser lacks comes from our C/C++ code as WebAssembly (ss-web.js): the X25519 key
 * exchange, an Opus decoder. Signaling: a WebSocket to this site's server (/ws).
 * ===================================================================== */

const RES_TARGETS = { native: null, 2160: [3840, 2160], 1440: [2560, 1440], 1080: [1920, 1080], 720: [1280, 720] };
const RES_LABEL = { native: 'natív', 2160: '4K', 1440: '1440p', 1080: '1080p', 720: '720p' };
const CONTENT_HINT = { motion: 'motion', detail: 'detail', balanced: '' };
const DEFAULT_SETTINGS = { res: 'auto', fps: 'auto', kbps: 'auto', mode: 'motion', audio: true, audioSource: 'display' };
const SETTINGS_VERSION = 3; // older saved settings are dropped so everyone starts from the best defaults
const NUMERIC_SETTINGS = new Set(['fps', 'kbps']);
// "auto": a GPU encoder up to 4K 60 – the processor what it manages without falling behind
const HW_TARGET = { res: '2160', fps: 60, kbps: 120000 };
const SW_TARGET = { res: '1080', fps: 30, kbps: 20000 };
const DIRECT_CODEC = 'avc1.640034'; // H.264 High 5.2 (up to 4K 60): every GPU decodes it
const DIRECT_MIN_KBPS = 3000;
// iPhone / iPad – iPadOS Safari presents itself as a Mac, the touch screen gives it away
const IS_IOS = /iPad|iPhone|iPod/.test(navigator.userAgent) || (/Macintosh/.test(navigator.userAgent) && navigator.maxTouchPoints > 1);
const IS_MAC = !IS_IOS && /Macintosh/.test(navigator.userAgent);
const IS_SAFARI = /^((?!chrome|android|crios|fxios|edg).)*safari/i.test(navigator.userAgent);
// started from the home-screen icon (iOS / Android web app)
const STANDALONE = Boolean(window.matchMedia?.('(display-mode: standalone)').matches || navigator.standalone);
// the website inside the iOS app (ios/App/WatchView.swift); sharing is its own tab there
const IN_IOS_APP = /ScreenShareApp\//.test(navigator.userAgent);
const IOS_APP_URL = 'https://github.com/Shreklyx/screeen/releases/download/desktop-latest/ScreenShare-iOS.ipa';
const SITE_URL = `${location.origin}${location.pathname}`;

const ERROR_TEXT = {
  auth: 'Hibás jelszó.',
  rate: 'Túl sok próbálkozás. Várj egy kicsit, és próbáld újra.',
  full: 'A szerver vagy a szoba megtelt. Próbáld újra később.',
  notfound: 'Ez a szoba nem létezik, vagy már bezárult.',
  offline: 'Nem sikerült elérni a szervert.',
  timeout: 'A szerver nem válaszolt időben.',
};
const errorText = (err) => ERROR_TEXT[err?.code] || err?.message || 'Ismeretlen hiba.';

const PATH_LABEL = { lan: 'helyi hálózat', internet: 'interneten át', relay: 'a szerveren át', turn: 'TURN relén át', tcp: 'a szerveren át' };
// why a viewer gets no picture (yet) – shown to both sides
const WHY_TEXT = {
  'viewer-browser': 'a néző böngészője túl régi (friss Chrome, Edge, Firefox vagy Safari kell)',
  server: 'a szerver még nem ismeri – futtasd rajta újra: sudo bash deploy/install.sh',
  encoder: 'nincs használható H.264 kódoló',
  udp: 'a két gép nem érte el egymást',
  'udp-blocked': 'semmilyen úton nem jött létre kapcsolat – tűzfal?',
  timeout: 'a kapcsolat nem jött létre időben',
  unavailable: 'az átvitel nem indult el',
  failed: 'a kapcsolat megszakadt',
};

/* ---------------------------- helpers ---------------------------- */

const $ = (sel) => document.querySelector(sel);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const clockNow = () => performance.timeOrigin + performance.now();
const evenPx = (x) => Math.max(2, Math.round(x / 2) * 2);

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
  document.body.classList.toggle('viewing', id === 'view-viewer');
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

function median(list) {
  if (!list.length) return null;
  const sorted = [...list].sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
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
    const socket = new WebSocket(`${location.origin.replace(/^http/, 'ws')}/ws`);
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

/* ------------------------ the transport ------------------------ */

let DIRECT = null; // direct-web.js, once this browser is known to manage it
let canDirect = { ok: false }; // what it can do (direct-web.js supported())
let directReady = Promise.resolve();

/** Sharing: the screen, an H.264 encoder and the transport. null when it works, else why not. */
async function shareProblem() {
  if (!navigator.mediaDevices?.getDisplayMedia) return 'display';
  if (typeof VideoEncoder !== 'function' || typeof VideoFrame !== 'function') return 'encoder';
  await directReady;
  if (!canDirect.ok) return 'encoder';
  try {
    const c = { codec: DIRECT_CODEC, width: 1920, height: 1080, bitrate: 8e6, framerate: 30, avc: { format: 'annexb' } };
    if ((await VideoEncoder.isConfigSupported(c)).supported !== true) return 'encoder';
  } catch {
    return 'encoder';
  }
  return null;
}

/* ============================== HOST ============================== */

const savedSettings = store.get('ss-settings', {});
const settings = { ...DEFAULT_SETTINGS, ...(savedSettings.v === SETTINGS_VERSION ? savedSettings : {}), v: SETTINGS_VERSION };

const host = {
  signal: null,
  password: '',
  code: null,
  token: null,
  iceServers: [],
  serverDirect: false,
  video: null,
  audio: null,
  audioDenied: false, // macOS refused system audio recording
  viewers: new Map(),
  seq: 0,
  statsTimer: null,
  meter: null,
  dims: '',
  previewOn: true,
  active: false,
  tick: 0,
};

const isLive = () => Boolean(host.video && host.video.readyState === 'live');
const hwEncoding = () => direct.hw !== false; // until it is known: the GPU (VideoEncoder tells)
const autoTarget = () => (hwEncoding() ? HW_TARGET : SW_TARGET);
const captureFps = () => (settings.fps === 'auto' ? HW_TARGET.fps : settings.fps);

const RAW_AUDIO = { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: { ideal: 2 }, sampleRate: { ideal: 48000 } };
const useDeviceAudio = () => settings.audio && settings.audioSource !== 'display';

// Audio from an input device instead of the shared screen (the Hangforrás list).
async function captureDeviceAudio() {
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: { ...RAW_AUDIO, deviceId: { exact: settings.audioSource } } });
    const track = stream.getAudioTracks()[0];
    track.contentHint = 'music';
    return track;
  } catch (err) {
    toast(`A kiválasztott hangforrás nem érhető el (${err.name}). Hang nélkül megy tovább.`, 'warn', 8000);
    return null;
  }
}

async function captureScreen() {
  const fps = captureFps();
  const video = { frameRate: { ideal: fps, max: fps }, cursor: 'always' };
  // Safari never shares the computer's sound, and a second try would no longer count as
  // a click (Safari refuses screen capture outside one): ask for the picture only.
  const audio = settings.audio && !useDeviceAudio() && !IS_SAFARI ? { ...RAW_AUDIO, suppressLocalAudioPlayback: false } : false;
  const extras = {
    systemAudio: audio ? 'include' : 'exclude',
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
  let audioTrack = stream.getAudioTracks()[0] || null;
  videoTrack.contentHint = CONTENT_HINT[settings.mode];
  // macOS hands over an already ended track when "System Audio Recording" is refused
  const audioDenied = audioTrack?.readyState === 'ended';
  if (audioDenied) audioTrack = null;
  if (audioTrack) audioTrack.contentHint = 'music';
  const result = { video: videoTrack, audio: audioTrack, audioDenied };
  if (useDeviceAudio()) result.audio = await captureDeviceAudio();
  return result;
}

// Swap only the audio (another source picked while sharing).
function setHostAudio(track) {
  const old = host.audio;
  host.audio = track;
  if (old && old !== track) old.stop();
  updateDirectPipelines();
  startMeter(track);
  updateCaptureInfo();
}

async function populateAudioSources() {
  const select = $('#setAudioSource');
  if (!navigator.mediaDevices?.enumerateDevices) return;
  const inputs = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === 'audioinput' && d.deviceId && d.deviceId !== 'default' && d.deviceId !== 'communications');
  const options = [new Option('Képernyő / rendszerhang', 'display')];
  inputs.forEach((d, i) => options.push(new Option(d.label || `Hangbemenet ${i + 1}`, d.deviceId)));
  if (!inputs.some((d) => d.label)) options.push(new Option('Eszközök listázása…', 'list'));
  select.replaceChildren(...options);
  select.value = settings.audioSource;
  if (select.value !== settings.audioSource) select.value = 'display';
}

async function onAudioSourceChange(e) {
  const value = e.target.value;
  if (value === 'list') {
    // device names are only visible after an audio permission
    try {
      const s = await navigator.mediaDevices.getUserMedia({ audio: true });
      s.getTracks().forEach((t) => t.stop());
    } catch { /* permission refused */ }
    await populateAudioSources();
    return;
  }
  settings.audioSource = value;
  store.set('ss-settings', settings);
  if (!isLive() || !settings.audio) return;
  if (useDeviceAudio()) setHostAudio(await captureDeviceAudio());
  else switchSource(); // display audio only comes with a new screen pick
}

function hostSignal() {
  if (!host.signal) {
    host.signal = createSignal({
      onMessage: onHostMessage,
      onOpen: () => { if (host.code && host.active) reclaimRoom(); },
      onClose: () => { if (host.code) setSignalStatus('Megszakadt a kapcsolat a szerverrel, újracsatlakozás…', 'warn'); },
    });
  }
  return host.signal;
}

async function openRoom(password) {
  const signal = hostSignal();
  await signal.whenOpen(8000);
  const resume = store.get('ss-host-room', null, 'session');
  const res = await signal.request({ t: 'host', password, resume: resume || undefined, name: chatName() }, ['hosted']);
  applyHosted(res);
  return Boolean(resume && res.code === resume.code);
}

// After a lost server connection: take the room back with its token (password as fallback).
async function reclaimRoom() {
  try {
    const res = await host.signal.request({ t: 'host', password: host.password, resume: { code: host.code, token: host.token }, name: chatName() }, ['hosted']);
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
  host.serverDirect = res.direct === true;
  store.set('ss-host-room', { code: res.code, token: res.token }, 'session');
  $('#roomCode').textContent = res.code;
  $('#roomLink').value = `${SITE_URL}#${res.code}`;
  $('#btnCopy').disabled = false;
  setSignalStatus(host.serverDirect ? 'Kapcsolódva. A nézők a linkkel vagy a kóddal csatlakozhatnak.' : `A nézők nem kapnak képet: ${WHY_TEXT.server}`, host.serverDirect ? 'ok' : 'warn');
  host.signal.send({ t: 'state', live: isLive() });
  const present = new Set((res.viewers || []).map((v) => v.id));
  for (const id of [...host.viewers.keys()]) if (!present.has(id)) dropViewer(id);
  for (const v of res.viewers || []) ensureViewer(v.id, v.caps, v.name);
  hostChat().joined(res, 'host');
}

async function startHosting(e) {
  e?.preventDefault();
  const password = $('#hostPassword').value;
  $('#shareError').hidden = true;
  ensureAudioCtx(); // must happen inside the click gesture
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
    case 'viewer': ensureViewer(msg.id, msg.caps, msg.name); break;
    case 'viewer-name': {
      const v = host.viewers.get(msg.id);
      if (v && typeof msg.name === 'string') { v.name = msg.name; renderViewers(); }
      break;
    }
    case 'chat': hostChat().message(msg, 'host'); break;
    case 'chat-limit': chatTooFast(); break;
    case 'viewer-left': dropViewer(msg.id); break;
    case 'signal': onViewerSignal(msg.from, msg.data); break;
    case 'direct': onDirectSession(msg); break;
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

/* ---- the viewers ---- */

function ensureViewer(id, caps, name) {
  let v = host.viewers.get(id);
  if (v && typeof name === 'string' && name) v.name = name;
  if (v?.direct && !['failed', 'lost', 'closed'].includes(v.direct.state)) return;
  if (!v) {
    v = { id, n: ++host.seq, caps: caps || null, name: typeof name === 'string' ? name : '', stats: null, direct: null, remote: null };
    host.viewers.set(id, v);
  }
  if (!v.caps?.direct || !host.serverDirect) {
    // an old browser (or an old server): it cannot get the picture – it is told why
    v.directWhy = !host.serverDirect ? 'server' : 'viewer-browser';
    host.signal.send({ t: 'signal', to: v.id, data: { type: 'unsupported', why: v.directWhy } });
  } else {
    startDirectViewer(v);
  }
  renderViewers();
}

function dropViewer(id) {
  const v = host.viewers.get(id);
  if (!v) return;
  clearTimeout(v.retryTimer);
  host.viewers.delete(id);
  stopDirectViewer(v);
  updateDirectPipelines();
  renderViewers();
}

function onViewerSignal(id, data) {
  const v = host.viewers.get(id);
  if (!v || !data || typeof data !== 'object') return;
  if (data.type === 'recall') {
    if (Date.now() - (v.createdAt || 0) > 3000) startDirectViewer(v);
    return;
  }
  if (data.session !== v.session) return;
  if (data.type === 'direct-answer' && v.direct?.state === 'connecting') {
    DIRECT.connect(v.id, { pub: data.pub, cands: data.cands, tcp: data.tcp === true });
  } else if (data.type === 'direct-fail') {
    directFailed(v, typeof data.why === 'string' && WHY_TEXT[data.why] ? data.why : 'failed');
  } else if (data.type === 'stats') {
    const num = (x) => (Number.isFinite(x) && x >= 0 && x < 1e6 ? x : null);
    v.remote = { lat: num(data.lat), fps: num(data.fps), loss: num(data.loss), measured: data.measured === true };
  }
}

/* ---- capture lifecycle ---- */

function setHostTracks({ video, audio, audioDenied = false }) {
  const old = [host.video, host.audio];
  Object.assign(host, { video, audio, audioDenied });
  video.addEventListener('ended', () => { if (host.video === video) pauseSharing(); });
  for (const t of old) if (t && t !== video && t !== audio) t.stop();

  direct.forceKey = true;
  updateDirectPipelines();

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
  updateDirectPipelines();
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

// What "Automatikus" picked for this machine, in words.
function renderAutoProfile() {
  const t = autoTarget();
  const res = settings.res === 'auto' ? t.res : settings.res;
  const fps = settings.fps === 'auto' ? t.fps : settings.fps;
  const kbps = settings.kbps === 'auto' ? t.kbps : settings.kbps;
  const why = direct.hw === false ? 'a processzor kódol' : direct.hw ? 'a videókártya kódol' : 'a videókártya kódol, ha tud';
  $('#autoProfile').textContent = `Most: ${RES_LABEL[res] || res} · ${fps} FPS · max. ${fmtKbps(kbps)} – ${why}.`;
}

function updateCaptureInfo() {
  const s = host.video?.getSettings?.() || {};
  const cfg = direct.video?.config;
  const w = cfg?.width || s.width;
  const h = cfg?.height || s.height;
  const fps = Math.round(cfg?.framerate || s.frameRate || captureFps());
  $('#captureInfo').textContent = w && h ? `${w}×${h} · ${fps} FPS` : '';
  renderAutoProfile();

  const el = $('#audioStatus');
  if (!host.video) {
    el.textContent = '';
  } else if (host.audio && !direct.canEncodeAudio) {
    el.textContent = 'Ez a böngésző nem tud hangot kódolni (Opus): a nézők hang nélkül kapják a képet. Hanghoz oszd meg Chrome-ból, Edge-ből vagy az asztali alkalmazásból.';
    el.className = 'hint warn';
  } else if (host.audio) {
    el.textContent = 'Hang megosztva (sztereó). A csík mutatja, hogy jön-e hang.';
    el.className = 'hint ok';
  } else if (!settings.audio) {
    el.textContent = 'A hangmegosztás ki van kapcsolva a beállításokban.';
    el.className = 'hint';
  } else {
    if (useDeviceAudio()) {
      el.textContent = 'A kiválasztott hangforrás nem érhető el – válassz másikat a Hangforrás listában.';
    } else if (host.audioDenied) {
      el.textContent = 'A macOS nem engedi a rendszerhangot: Rendszerbeállítások → Adatvédelem és biztonság → Képernyő- és rendszerhangfelvétel → kapcsold be a böngészőt, majd indítsd újra.';
    } else if (IS_SAFARI) {
      el.textContent = 'A Safari a gép hangját nem tudja megosztani. Hanghoz válassz egy hangbemenetet a Hangforrás listában, vagy oszd meg Chrome-ból / a Mac alkalmazásból (az a teljes rendszerhangot viszi).';
    } else if (IS_MAC) {
      el.textContent = 'Nincs hang. A Mac teljes rendszerhangjához használd a Screen Share alkalmazást (semmit nem kell mellé telepíteni). Böngészőből egy lap hangja megosztható: Forrás váltása → „Chrome-lap” fül → lap hangjának megosztása.';
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
    const fps = captureFps();
    host.video.applyConstraints({ frameRate: { ideal: fps, max: fps } }).catch(() => {});
  }
  if (key === 'mode' && host.video) host.video.contentHint = CONTENT_HINT[value];
  if (key === 'kbps' && direct.kbps) direct.kbps = Math.min(direct.kbps, directTargetKbps());
  if (key === 'audio') updateDirectPipelines();
  updateCaptureInfo();
}

function hostStatsTick() {
  const s = host.video?.getSettings?.() || {};
  const dims = `${s.width}x${s.height}`;
  if (isLive() && dims !== host.dims) {
    host.dims = dims;
    updateCaptureInfo();
  }
  for (const v of host.viewers.values()) {
    v.stats = v.direct ? directStats(v) : null;
    if (v.stats && host.tick % 2 === 0) {
      const { encodeMs, hw } = v.stats;
      host.signal?.send({ t: 'signal', to: v.id, data: { type: 'stats', session: v.session, encodeMs, hw, encoder: hw ? 'videókártya (WebCodecs)' : null, codec: 'H264' } });
    }
  }
  host.tick++;
  renderViewers();
  renderEncoderStatus();
  updateCaptureInfo();
}

function renderEncoderStatus() {
  const el = $('#encoderStatus');
  const pipe = direct.video;
  if (!isLive() || !pipe?.config) {
    el.textContent = direct.failed ? `A kép nem kódolható: ${direct.failed}` : 'Az első néző csatlakozásakor indul a kódolás (H.264).';
    el.className = direct.failed ? 'hint warn' : 'hint';
    return;
  }
  const ms = pipe.encodeMs != null ? ` · ${nf1.format(pipe.encodeMs)} ms/kép` : '';
  const size = `${pipe.config.width}×${pipe.config.height}`;
  if (direct.hw) {
    el.textContent = `Hardveres kódolás (videókártya) · H.264 · ${size}${ms}`;
    el.className = 'hint ok';
  } else {
    el.textContent = `Szoftveres kódolás (processzor) · H.264 · ${size}${ms}. Ez lassabb: kapcsold be a böngészőben a grafikus gyorsítást, vagy oszd meg az asztali alkalmazásból.`;
    el.className = 'hint warn';
  }
}

function renderViewers() {
  const rows = [];
  for (const v of host.viewers.values()) {
    const li = document.createElement('li');
    const head = document.createElement('div');
    head.className = 'viewer-head';
    const name = document.createElement('span');
    name.textContent = v.name || `Néző #${v.n}`;
    const state = document.createElement('span');
    if (!isLive()) { state.textContent = 'vár (szünet)'; state.className = 'viewer-state'; }
    else if (v.direct?.state === 'connected') { state.textContent = `néz · ${PATH_LABEL[v.direct.path] || ''}`; state.className = 'viewer-state ok'; }
    else if (!v.direct && v.directWhy && !v.caps?.direct) { state.textContent = 'nem tud nézni'; state.className = 'viewer-state warn'; }
    else { state.textContent = 'kapcsolódik…'; state.className = 'viewer-state'; }
    head.append(name, state);
    li.append(head);
    if (isLive() && !v.direct && v.directWhy) {
      const note = document.createElement('div');
      note.className = 'viewer-stats warn';
      note.textContent = v.caps?.direct ? `Kapcsolat újra… (${WHY_TEXT[v.directWhy] || v.directWhy})` : `Nem kap képet: ${WHY_TEXT[v.directWhy]}`;
      li.append(note);
    }
    const s = v.stats;
    if (s && isLive()) {
      const stats = document.createElement('div');
      stats.className = 'viewer-stats';
      const line1 = document.createElement('div');
      line1.textContent = `${s.w || '?'}×${s.h || '?'} · ${s.fps != null ? Math.round(s.fps) : '–'} FPS · ${fmtKbps(s.kbps)}`;
      const line2 = document.createElement('div');
      line2.textContent = `H.264 · ${s.hw === true ? 'videókártya' : s.hw === false ? 'szoftveres' : '…'} · RTT ${fmtMs(s.rtt)}`;
      stats.append(line1, line2);
      if (v.remote?.lat != null) {
        // measured end to end (capture → on the viewer's screen)
        const total = v.remote.measured ? v.remote.lat : v.remote.lat + (s.encodeMs || 0);
        const line = document.createElement('div');
        line.textContent = `Késés: ~${Math.round(total)} ms`;
        if (total > 150) line.className = 'warn';
        stats.append(line);
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

async function initHostUi() {
  hostChat(); // (its form works from the start)
  const fields = { setRes: 'res', setFps: 'fps', setKbps: 'kbps', setMode: 'mode' };
  for (const [id, key] of Object.entries(fields)) {
    const el = $(`#${id}`);
    el.value = String(settings[key]);
    if (el.value !== String(settings[key])) {
      settings[key] = DEFAULT_SETTINGS[key];
      el.value = String(settings[key]);
    }
    el.addEventListener('change', () => updateSetting(key, NUMERIC_SETTINGS.has(key) && el.value !== 'auto' ? Number(el.value) : el.value));
  }
  const audio = $('#setAudio');
  audio.checked = settings.audio;
  audio.addEventListener('change', () => updateSetting('audio', audio.checked));

  const saved = store.get('ss-host-password', '');
  if (saved) $('#hostPassword').value = saved;

  populateAudioSources();
  $('#setAudioSource').addEventListener('change', onAudioSourceChange);
  navigator.mediaDevices?.addEventListener?.('devicechange', populateAudioSources);

  if (IS_IOS) {
    $('#iosInstall').hidden = STANDALONE || IN_IOS_APP; // already installed
  } else {
    $('#downloads').hidden = false;
    $(IS_MAC ? '#dlMac' : '#dlWin').classList.add('btn-primary');
  }

  $('#shareForm').addEventListener('submit', startHosting);
  $('#btnSwitch').addEventListener('click', switchSource);
  $('#btnResume').addEventListener('click', switchSource);
  $('#btnStop').addEventListener('click', pauseSharing);
  $('#btnPreview').addEventListener('click', togglePreview);
  $('#btnClose').addEventListener('click', closeRoom);
  $('#btnCopy').addEventListener('click', copyLink);
  $('#roomLink').addEventListener('focus', (e) => e.target.select());

  const problem = await shareProblem();
  if (problem) {
    $('#btnShare').disabled = true;
    const note = $('#shareUnsupported');
    note.hidden = false;
    if (IN_IOS_APP) {
      note.textContent = 'A képernyőd megosztása: lent a „Megosztás” fülön.';
    } else if (IS_IOS) {
      // Apple allows no screen capture in any iOS browser – the iOS app can (ReplayKit)
      note.replaceChildren(
        'iPhone-on és iPaden a böngésző nem oszthat meg képernyőt (Apple-korlát). Megosztani a ',
        Object.assign(document.createElement('a'), { href: IOS_APP_URL, textContent: 'Screen Share iOS alkalmazással' }),
        ' lehet (telepítés: lent).',
      );
    } else if (problem === 'display') {
      note.textContent = 'Ez a böngésző nem tud képernyőt megosztani. Számítógépen használj friss Chrome-ot, Edge-et, Firefoxot vagy Safarit – vagy az asztali alkalmazást.';
    } else {
      note.textContent = 'Ez a böngésző nem tud H.264 képet kódolni (WebCodecs), ami a megosztáshoz kell. Frissítsd, vagy használj Chrome-ot, Edge-et – vagy az asztali alkalmazást.';
    }
  } else if (IS_MAC) {
    $('#macHint').hidden = false;
  }
}

/* ===================== DIRECT MODE: the sharer ===================== */
// The sharer encodes once for every viewer (WebCodecs, on the GPU if it can), the frames go over
// our encrypted transport, and each viewer decodes and draws every frame the moment it arrives –
// no jitter buffer, no pacing queue.

const direct = {
  video: null, // the encoding pipelines
  audio: null,
  forceKey: false,
  wantKey: false,
  lastKey: 0,
  kbps: 0,
  lastCut: 0,
  lastRaise: 0,
  hw: null, // the encoder runs on the GPU (null: not known yet)
  failed: null, // why the picture cannot be encoded at all
  canEncodeAudio: typeof AudioEncoder === 'function',
};

/** The TURN server (from the room's ICE servers) – the apps' last-resort relay. */
function turnInfo(iceServers) {
  for (const s of iceServers || []) {
    if (!s.username || !s.credential) continue;
    for (const url of [].concat(s.urls || [])) {
      const m = /^turn:([^:?]+)(?::(\d+))?(?:\?transport=(\w+))?$/i.exec(url);
      if (m && (!m[3] || m[3].toLowerCase() === 'udp')) return { host: m[1], port: Number(m[2] || 3478), username: s.username, credential: s.credential };
    }
  }
  return null;
}

function startDirectViewer(v) {
  clearTimeout(v.retryTimer);
  stopDirectViewer(v);
  if (!DIRECT) return;
  v.session = randomId();
  v.createdAt = Date.now();
  v.stats = null;
  v.remote = null;
  v.direct = { state: 'starting', path: null, rtt: null, report: null, minRtt: Infinity };
  host.signal?.send({ t: 'direct', to: v.id });
  v.directTimer = setTimeout(() => { if (v.direct && v.direct.state !== 'connected') directFailed(v, 'timeout'); }, 12000);
}

function stopDirectViewer(v) {
  clearTimeout(v.directTimer);
  if (!v.direct) return;
  v.direct = null;
  DIRECT?.remove(v.id);
  updateDirectPipelines();
}

// A failed attempt is simply made again, after a pause.
function directFailed(v, why = 'failed') {
  if (host.viewers.get(v.id) !== v) return;
  stopDirectViewer(v);
  v.directWhy = why;
  v.directRetries = (v.directRetries || 0) + 1;
  clearTimeout(v.retryTimer);
  v.retryTimer = setTimeout(() => {
    if (host.viewers.get(v.id) === v && !v.direct) startDirectViewer(v);
  }, Math.min(15000, 1500 * v.directRetries));
  renderViewers();
}

async function onDirectSession(msg) {
  const v = host.viewers.get(msg.viewer);
  const d = v?.direct;
  if (!d || d.state !== 'starting') return;
  const res = await DIRECT.add('host', v.id, { session: msg.session, secret: msg.secret, port: msg.port, turn: turnInfo(host.iceServers) });
  if (v.direct !== d) return;
  if (!res || res.error) {
    directFailed(v, 'unavailable');
    return;
  }
  d.state = 'connecting';
  host.signal.send({ t: 'signal', to: v.id, data: { type: 'direct-offer', session: v.session, udp: msg.session, pub: res.pub, cands: res.cands, tcp: res.tcp === true, codec: DIRECT_CODEC } });
}

function onHostDirectEvent(name, id, info) {
  if (name === 'keyframe') {
    direct.wantKey = true;
    return;
  }
  const v = host.viewers.get(id);
  if (!v?.direct || !info) return;
  if (name === 'state') {
    Object.assign(v.direct, { state: info.state, path: info.path, rtt: info.rtt });
    if (info.state === 'connected') {
      clearTimeout(v.directTimer);
      if (!direct.kbps) direct.kbps = info.path === 'lan' ? directTargetKbps() : Math.min(directTargetKbps(), 25000);
      DIRECT.bitrate(direct.kbps * 1000);
      updateDirectPipelines();
      direct.forceKey = true;
    } else if (info.state === 'failed') {
      directFailed(v, info.diag && !info.diag.server && !info.diag.turn && !info.diag.tcp ? 'udp-blocked' : 'udp');
    } else if (info.state === 'lost' || info.state === 'closed') {
      directFailed(v, 'failed'); // the viewer's network changed or its page reloaded: connect again
    }
    renderViewers();
  } else if (name === 'report') {
    v.direct.report = info;
    if (Number.isFinite(info.rtt)) v.direct.minRtt = Math.min(v.direct.minRtt, info.rtt);
    if (info.fps > 0) {
      v.directRetries = 0;
      v.directWhy = null;
    }
    adaptDirectBitrate();
  }
}

const directViewers = () => [...host.viewers.values()].filter((v) => v.direct?.state === 'connected');
// the most beautiful picture the network allows: high (GPU) / what a processor manages
const directTargetKbps = () => (settings.kbps === 'auto' ? autoTarget().kbps : settings.kbps);

// No bandwidth estimate: lower the bitrate when a viewer loses packets, has to skip frames or its
// round trip grows (queues filling up), raise it slowly while all is well.
function adaptDirectBitrate() {
  const viewers = directViewers();
  const reports = viewers.map((v) => v.direct.report).filter(Boolean);
  if (!reports.length) return;
  const target = directTargetKbps();
  const now = Date.now();
  const congested = reports.some((r) => r.loss > 2 || r.skips > 0)
    || viewers.some((v) => v.direct.report?.rtt != null && v.direct.report.rtt > v.direct.minRtt + 40);
  if (congested && now - direct.lastCut > 2000) {
    direct.kbps = Math.max(DIRECT_MIN_KBPS, direct.kbps * 0.7);
    direct.lastCut = now;
  } else if (!congested && now - direct.lastCut > 3000 && now - direct.lastRaise > 1000 && reports.every((r) => r.loss < 0.5)) {
    direct.kbps = Math.min(target, direct.kbps * 1.12 + 500);
    direct.lastRaise = now;
  }
  direct.kbps = Math.min(direct.kbps, target);
  DIRECT.bitrate(direct.kbps * 1000);
}

/** Starts/stops the encoders to match the tracks and whether any viewer is connected. */
function updateDirectPipelines() {
  if (!DIRECT) return;
  if (!isLive() || !directViewers().length) {
    stopDirectVideo();
    stopDirectAudio();
    return;
  }
  if (direct.video?.track !== host.video) startDirectVideo(host.video);
  const audio = settings.audio && direct.canEncodeAudio ? host.audio : null;
  if ((direct.audio?.track || null) !== (audio || null)) startDirectAudio(audio);
}

/* ---- the sharer's picture ---- */

/** The track's frames, one at a time: MediaStreamTrackProcessor (Chrome, Edge), else from a
 *  video element as it shows them (Firefox, Safari). onFrame owns (closes) each frame. */
function readFrames(track, onFrame) {
  if (typeof MediaStreamTrackProcessor === 'function') {
    const reader = new MediaStreamTrackProcessor({ track, maxBufferSize: 1 }).readable.getReader();
    let stopped = false;
    (async () => {
      try {
        for (;;) {
          const { value: frame, done } = await reader.read();
          if (done) break;
          if (stopped) {
            frame.close();
            break;
          }
          await onFrame(frame);
        }
      } catch (err) {
        if (!stopped) console.warn('frames', err);
      }
    })();
    return () => {
      stopped = true;
      reader.cancel().catch(() => {});
    };
  }
  const el = document.createElement('video');
  el.muted = true;
  el.playsInline = true;
  el.srcObject = new MediaStream([track]);
  el.play().catch(() => {});
  let stopped = false;
  let busy = false;
  let timer = null;
  let callback = null;
  const grab = async () => {
    if (stopped || busy || el.readyState < 2 || !el.videoWidth) return;
    busy = true; // a frame that would wait for the encoder is only delay: skipped
    try {
      await onFrame(new VideoFrame(el, { timestamp: Math.round(performance.now() * 1000) }));
    } catch (err) {
      console.warn('frames', err);
    } finally {
      busy = false;
    }
  };
  if (el.requestVideoFrameCallback) {
    const next = () => {
      if (stopped) return;
      grab();
      callback = el.requestVideoFrameCallback(next);
    };
    callback = el.requestVideoFrameCallback(next);
  } else {
    timer = setInterval(grab, 1000 / captureFps());
  }
  return () => {
    stopped = true;
    clearInterval(timer);
    if (callback != null) el.cancelVideoFrameCallback?.(callback);
    el.pause();
    el.srcObject = null;
  };
}

function startDirectVideo(track) {
  stopDirectVideo();
  if (!track || track.readyState !== 'live') return;
  const pipe = { track, stop: null, encoder: null, config: null, sizeKey: '', captured: new Map(), encodeMs: null, inFlight: 0, limit: 1, lastOutput: performance.now(), scale: 1, slowSince: 0, fastSince: 0, stopped: false };
  direct.video = pipe;
  direct.forceKey = true;
  pipe.stop = readFrames(track, async (frame) => {
    if (pipe.stopped) {
      frame.close();
      return;
    }
    try {
      await encodeDirectFrame(pipe, frame);
    } catch (err) {
      if (pipe.stopped) return;
      console.warn('direct video', err);
      direct.failed = 'nincs használható H.264 kódoló ebben a böngészőben';
      stopDirectVideo();
      renderEncoderStatus();
    }
  });
}

function stopDirectVideo() {
  const pipe = direct.video;
  if (!pipe) return;
  direct.video = null;
  pipe.stopped = true;
  pipe.stop?.();
  try { pipe.encoder?.close(); } catch { /* already closed */ }
}

function directVideoConfig(pipe, frame) {
  const w = frame.displayWidth;
  const h = frame.displayHeight;
  const target = RES_TARGETS[settings.res === 'auto' ? autoTarget().res : settings.res];
  let scale = 1;
  if (target) {
    const [tw, th] = w >= h ? target : [target[1], target[0]];
    scale = Math.max(1, w / tw, h / th);
  }
  scale *= pipe.scale; // the encoder could not keep up at the full size
  return {
    codec: DIRECT_CODEC,
    width: evenPx(w / scale),
    height: evenPx(h / scale),
    bitrate: Math.round((direct.kbps || directTargetKbps()) * 1000),
    framerate: settings.fps === 'auto' ? autoTarget().fps : settings.fps,
    latencyMode: 'realtime',
    bitrateMode: 'variable',
    avc: { format: 'annexb' },
    ...(CONTENT_HINT[settings.mode] ? { contentHint: CONTENT_HINT[settings.mode] } : {}),
  };
}

async function configureDirectEncoder(pipe, want) {
  const supported = async (c) => {
    try {
      return (await VideoEncoder.isConfigSupported(c)).supported === true;
    } catch {
      return false;
    }
  };
  let config = { ...want, hardwareAcceleration: 'prefer-hardware' };
  const hw = direct.hw !== false && (await supported(config));
  if (!hw) {
    // no GPU encoder: the processor manages at most 1080p 30 without falling behind
    const scale = Math.sqrt(Math.max(1, (want.width * want.height) / (1920 * 1080)));
    config = { ...want, width: evenPx(want.width / scale), height: evenPx(want.height / scale), framerate: Math.min(want.framerate, 30), hardwareAcceleration: 'no-preference' };
    if (!(await supported(config))) {
      delete config.contentHint; // (a browser that does not know it)
      if (!(await supported(config))) throw new Error('no H.264 encoder');
    }
  }
  if (pipe.stopped) return;
  if (!pipe.encoder || pipe.encoder.state === 'closed') {
    pipe.encoder = new VideoEncoder({
      output: (chunk) => onDirectChunk(pipe, chunk),
      error: (err) => {
        console.warn('direct encoder', err);
        if (direct.hw) direct.hw = false; // a GPU encoder that fails while running: the processor from now on
        pipe.encoder = null;
        pipe.sizeKey = '';
      },
    });
  }
  pipe.encoder.configure(config);
  pipe.config = config;
  pipe.inFlight = 0;
  pipe.lastOutput = performance.now();
  direct.hw = hw;
  direct.failed = null;
}

async function encodeDirectFrame(pipe, frame) {
  try {
    const want = directVideoConfig(pipe, frame);
    const sizeKey = `${want.width}x${want.height}@${want.framerate}`;
    if (!pipe.encoder || pipe.encoder.state !== 'configured' || sizeKey !== pipe.sizeKey) {
      await configureDirectEncoder(pipe, want);
      pipe.sizeKey = sizeKey;
      direct.forceKey = true;
    } else if (Math.abs(want.bitrate - pipe.config.bitrate) > pipe.config.bitrate * 0.1) {
      pipe.config = { ...pipe.config, bitrate: want.bitrate };
      pipe.encoder.configure(pipe.config);
    }
    if (pipe.stopped || !pipe.encoder) return;
    // The encoder is still busy: a frame that waits is only delay – skip it. (An encoder
    // that holds a frame or two back before its first output gets that much room.)
    if (pipe.inFlight >= pipe.limit || pipe.encoder.encodeQueueSize > 0) {
      if (performance.now() - pipe.lastOutput > 250 && pipe.limit < 4) pipe.limit++;
      return;
    }
    const now = Date.now();
    const keyFrame = direct.forceKey || (direct.wantKey && now - direct.lastKey >= 250);
    if (keyFrame) {
      direct.forceKey = false;
      direct.wantKey = false;
      direct.lastKey = now;
    }
    pipe.captured.set(frame.timestamp, { at: clockNow(), start: performance.now() });
    if (pipe.captured.size > 30) pipe.captured.delete(pipe.captured.keys().next().value);
    pipe.inFlight++;
    pipe.encoder.encode(frame, { keyFrame });
  } finally {
    frame.close();
  }
}

function onDirectChunk(pipe, chunk) {
  if (pipe.stopped) return;
  const data = new Uint8Array(chunk.byteLength);
  chunk.copyTo(data);
  const c = pipe.captured.get(chunk.timestamp);
  pipe.captured.delete(chunk.timestamp);
  pipe.inFlight = Math.max(0, pipe.inFlight - 1);
  pipe.lastOutput = performance.now();
  if (c) {
    const ms = performance.now() - c.start;
    pipe.encodeMs = pipe.encodeMs == null ? ms : pipe.encodeMs * 0.9 + ms * 0.1;
    adaptDirectSize(pipe);
  }
  DIRECT.send({ stream: 0, key: chunk.type === 'key', ts: chunk.timestamp, capture: c?.at ?? clockNow() }, data);
}

// Encoding slower than the frame interval adds delay: a smaller picture until it keeps up again.
function adaptDirectSize(pipe) {
  const now = performance.now();
  const interval = 1000 / (pipe.config?.framerate || 60);
  if (pipe.encodeMs > interval * 0.9) {
    pipe.fastSince = 0;
    pipe.slowSince ||= now;
    if (now - pipe.slowSince > 2000 && pipe.scale < 3) {
      pipe.scale *= 1.5;
      pipe.slowSince = 0;
    }
  } else if (pipe.encodeMs < interval * 0.35 && pipe.scale > 1) {
    pipe.slowSince = 0;
    pipe.fastSince ||= now;
    if (now - pipe.fastSince > 10000) {
      pipe.scale = Math.max(1, pipe.scale / 1.5);
      pipe.fastSince = 0;
    }
  } else {
    pipe.slowSince = 0;
    pipe.fastSince = 0;
  }
}

/* ---- the sharer's sound (Opus, 2.5 ms packets – its least delay) ---- */

/** The track's sound as AudioData: MediaStreamTrackProcessor, else an AudioWorklet (Firefox,
 *  Safari). onData owns (closes) each. Returns a stop function (async: the worklet loads). */
async function readAudio(track, onData) {
  if (typeof MediaStreamTrackProcessor === 'function') {
    const reader = new MediaStreamTrackProcessor({ track }).readable.getReader();
    let stopped = false;
    (async () => {
      try {
        for (;;) {
          const { value, done } = await reader.read();
          if (done || stopped) {
            value?.close();
            break;
          }
          onData(value);
        }
      } catch (err) {
        if (!stopped) console.warn('sound', err);
      }
    })();
    return () => {
      stopped = true;
      reader.cancel().catch(() => {});
    };
  }
  const ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
  await ctx.audioWorklet.addModule('/capture-audio.js');
  const src = ctx.createMediaStreamSource(new MediaStream([track]));
  const node = new AudioWorkletNode(ctx, 'capture-audio', { numberOfOutputs: 1 });
  const mute = ctx.createGain();
  mute.gain.value = 0; // (connected only so that the browser runs it)
  src.connect(node).connect(mute).connect(ctx.destination);
  let next = null; // the next sample's time, µs: a sample-accurate timeline
  node.port.onmessage = ({ data: { planes } }) => {
    const frames = planes[0]?.length || 0;
    if (!frames) return;
    const arrived = performance.now() * 1000 - (frames / 48000) * 1e6;
    if (next == null || Math.abs(next - arrived) > 20000) next = arrived;
    const data = new Float32Array(frames * planes.length);
    planes.forEach((p, c) => data.set(p, c * frames));
    onData(new AudioData({ format: 'f32-planar', sampleRate: 48000, numberOfFrames: frames, numberOfChannels: planes.length, timestamp: Math.round(next), data }));
    next += (frames / 48000) * 1e6;
  };
  ctx.resume().catch(() => {});
  return () => {
    node.port.onmessage = null;
    src.disconnect();
    ctx.close().catch(() => {});
  };
}

function startDirectAudio(track) {
  stopDirectAudio();
  if (!track || track.readyState !== 'live') return;
  const pipe = { track, stop: null, encoders: [], channels: 0, rate: 0, stopped: false };
  direct.audio = pipe;
  readAudio(track, (data) => {
    try {
      if (!pipe.stopped) encodeDirectAudio(pipe, data);
    } catch (err) {
      console.warn('direct audio', err);
    } finally {
      data.close();
    }
  }).then((stop) => {
    pipe.stop = stop;
    if (pipe.stopped) stop();
  }, (err) => console.warn('direct audio', err));
}

function stopDirectAudio() {
  const pipe = direct.audio;
  if (!pipe) return;
  direct.audio = null;
  pipe.stopped = true;
  pipe.stop?.();
  for (const e of pipe.encoders) {
    try { e.close(); } catch { /* already closed */ }
  }
}

function encodeDirectAudio(pipe, data) {
  const channels = Math.min(8, data.numberOfChannels);
  const pairs = Math.ceil(channels / 2);
  if (channels !== pipe.channels || data.sampleRate !== pipe.rate) {
    for (const e of pipe.encoders) {
      try { e.close(); } catch { /* already closed */ }
    }
    pipe.encoders = Array.from({ length: pairs }, (_, pair) => {
      const ch = Math.min(2, channels - pair * 2);
      const enc = new AudioEncoder({
        output: (chunk) => sendDirectAudio(pipe, pair, channels, chunk),
        error: (err) => console.warn('direct audio encoder', err),
      });
      enc.configure({ codec: 'opus', sampleRate: data.sampleRate, numberOfChannels: ch, bitrate: ch * 128000, opus: { frameDuration: 2500, application: 'lowdelay' } });
      return enc;
    });
    pipe.channels = channels;
    pipe.rate = data.sampleRate;
  }
  const frames = data.numberOfFrames;
  for (let pair = 0; pair < pairs; pair++) {
    const ch = Math.min(2, channels - pair * 2);
    const buf = new Float32Array(frames * ch);
    for (let c = 0; c < ch; c++) data.copyTo(buf.subarray(c * frames, (c + 1) * frames), { planeIndex: pair * 2 + c, format: 'f32-planar' });
    const part = new AudioData({ format: 'f32-planar', sampleRate: data.sampleRate, numberOfFrames: frames, numberOfChannels: ch, timestamp: data.timestamp, data: buf });
    if (pipe.encoders[pair].state === 'configured') pipe.encoders[pair].encode(part);
    part.close();
  }
}

function sendDirectAudio(pipe, pair, channels, chunk) {
  if (pipe.stopped) return;
  const data = new Uint8Array(chunk.byteLength);
  chunk.copyTo(data);
  DIRECT.send({ stream: 1, sub: pair | (channels << 4), ts: chunk.timestamp, capture: clockNow() }, data);
}

function directStats(v) {
  const d = v.direct;
  if (d.state !== 'connected') return null;
  const r = d.report;
  const cfg = direct.video?.config;
  return {
    w: cfg?.width,
    h: cfg?.height,
    fps: r?.fps ?? null,
    kbps: r?.kbps ?? null,
    hw: direct.hw,
    encodeMs: direct.video?.encodeMs ?? null,
    rtt: r?.rtt ?? d.rtt,
  };
}

/* ============================= VIEWER ============================= */

const viewer = {
  signal: null,
  code: null,
  id: null,
  iceServers: [],
  caps: null,
  session: null,
  closed: false,
  live: false,
  hostPresent: false,
  retryTimer: null,
  offerTimer: null,
  statsTimer: null,
  volume: Math.min(1, Math.max(0, Number(store.get('ss-volume', 1)) || 0)),
  muted: false,
  wakeLock: null,
  idleTimer: null,
  hostStats: null, // the sender's encoder info (hardware, encode time)
  direct: null, // the connection to the sharer, see "DIRECT MODE: the viewer"
  tick: 0,
  unread: 0, // chat messages that came while the panel was closed
};

const canvas = () => $('#directCanvas');
const hasPicture = () => Boolean(viewer.direct?.drawn);

// 5.1/7.1 speakers in a Chromium browser: the sharer's surround sound plays there on every speaker.
function surroundSpeakers() {
  if (!navigator.userAgentData?.brands?.some((b) => /Chromium/i.test(b.brand))) return false;
  try {
    const ctx = new AudioContext();
    const channels = ctx.destination.maxChannelCount;
    ctx.close().catch(() => {});
    return channels >= 6;
  } catch {
    return false;
  }
}

async function startViewing(code) {
  showView('view-viewer');
  $('#viewerRoom').textContent = code;
  if (location.hash.slice(1) !== code) history.replaceState(null, '', `#${code}`);
  viewer.code = code;
  setOverlay('Csatlakozás a szobához…');
  await directReady;
  if (!canDirect.ok) {
    setOverlay(IS_IOS
      ? 'Ez az iOS-verzió még nem tudja fogadni a képet (iOS 16.4 vagy újabb kell). Frissítsd az iPhone-t / iPadet.'
      : 'Ez a böngésző túl régi a nézéshez (WebCodecs kell). Frissítsd, vagy nyisd meg friss Chrome-ban, Edge-ben, Firefoxban vagy Safariban – vagy az asztali alkalmazásban.', {
      spinner: false,
      actions: [{ label: 'Vissza a főoldalra', onClick: goHome }],
    });
    return;
  }
  viewer.caps = { direct: true, ...(surroundSpeakers() ? { surround: true } : {}) };
  viewer.signal = createSignal({
    onMessage: onViewerMessage,
    onOpen: joinRoom,
    onClose: () => {
      if (!viewer.closed && !hasPicture()) setOverlay('Megszakadt a kapcsolat a szerverrel, újracsatlakozás…');
    },
  });
  viewer.statsTimer = setInterval(directStatsTick, 1000);
  window.addEventListener('pagehide', () => viewer.signal?.send({ t: 'leave' }));
}

async function joinRoom() {
  clearTimeout(viewer.retryTimer);
  if (viewer.closed) return;
  try {
    const res = await viewer.signal.request({ t: 'join', code: viewer.code, caps: viewer.caps, name: chatName() }, ['joined']);
    viewer.id = res.id;
    viewerChat().joined(res, res.id);
    viewer.iceServers = res.iceServers || [];
    viewer.live = Boolean(res.live);
    viewer.hostPresent = Boolean(res.hostPresent);
    if (!viewer.hostPresent) setOverlay('A megosztó épp nincs bent. Várakozás…');
    else if (!viewer.live) setOverlay('A megosztás szünetel. Amint folytatódik, itt lesz a kép.');
    else if (!hasPicture()) setOverlay('Csatlakozva. Kép betöltése…');
    watchForOffer();
  } catch (err) {
    if (err.code === 'offline') return; // reconnect will call joinRoom again
    const delay = err.code === 'rate' ? 30000 : 5000;
    setOverlay(`${errorText(err)} Újrapróbálom…`, { actions: [{ label: 'Vissza a főoldalra', onClick: goHome }] });
    viewer.retryTimer = setTimeout(joinRoom, delay);
  }
}

// A sharer that sends nothing our way (an old iPhone app speaks only WebRTC): say so.
function watchForOffer() {
  clearTimeout(viewer.offerTimer);
  if (!viewer.live || !viewer.hostPresent || viewer.direct) return;
  viewer.offerTimer = setTimeout(() => {
    if (viewer.direct || viewer.closed || !viewer.live || !viewer.hostPresent) return;
    setOverlay('A megosztó nem küld képet ezen a módon – ha iPhone-ról oszt meg, frissítse a Screen Share alkalmazást. Várakozás…');
  }, 15000);
}

function onViewerMessage(msg) {
  switch (msg.t) {
    case 'signal':
      if (msg.from === 'host' && msg.data && typeof msg.data === 'object') onHostSignal(msg.data).catch((err) => console.warn('signal handling failed', err));
      break;
    case 'state':
    case 'host-back':
      viewer.live = Boolean(msg.live);
      viewer.hostPresent = true;
      if (!viewer.live) setOverlay('A megosztás szünetel. Amint folytatódik, itt lesz a kép.');
      else if (hasPicture()) { hideOverlay(); setLive(true); }
      else setOverlay('Kép betöltése…');
      if (!viewer.live) setLive(false);
      watchForOffer();
      break;
    case 'host-away':
      viewer.hostPresent = false;
      // the picture may still come; only cover it if it stopped
      if (!hasPicture() || viewer.direct?.state !== 'connected') setOverlay('A megosztó kapcsolata megszakadt. Várakozás…');
      break;
    case 'direct':
      onViewerDirectSession(msg);
      break;
    case 'count':
      $('#viewerCountText').textContent = `${msg.n} néző`;
      break;
    case 'chat':
      viewerChat().message(msg, viewer.id);
      break;
    case 'chat-limit':
      chatTooFast();
      break;
    case 'closed':
      viewer.closed = true;
      clearTimeout(viewer.retryTimer);
      clearTimeout(viewer.offerTimer);
      closeDirectView();
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

async function onHostSignal(data) {
  if (data.type === 'direct-offer') {
    await onDirectOffer(data);
  } else if (data.type === 'unsupported') {
    clearTimeout(viewer.offerTimer);
    setOverlay(data.why === 'server'
      ? 'A szerver még nem ismeri a saját átvitelt – a szervert frissíteni kell (sudo bash deploy/install.sh).'
      : 'Ez a böngésző nem tudja fogadni a képet. Nyisd meg friss Chrome-ban, Edge-ben, Firefoxban vagy Safariban, vagy az alkalmazásban.', { spinner: false });
  } else if (data.type === 'stats') {
    if (data.session === viewer.session) viewer.hostStats = data;
  }
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

function senderEncoderText(st) {
  if (!st) return '–';
  const name = st.hw === true ? st.encoder || 'hardveres' : st.hw === false ? 'szoftveres' : 'ismeretlen';
  return `${name}${Number.isFinite(st.encodeMs) ? ` · ${nf1.format(st.encodeMs)} ms` : ''}`;
}

/* ---- sound level, full screen, statistics ---- */

function syncMuteUi() {
  $('#btnMute').classList.toggle('muted', viewer.muted || viewer.volume === 0);
  $('#volume').value = String(viewer.muted ? 0 : viewer.volume);
  syncDirectVolume();
}

function toggleMute() {
  viewer.muted = !viewer.muted;
  if (!viewer.muted && viewer.volume === 0) viewer.volume = 1;
  $('#btnUnmute').hidden = true;
  viewer.direct?.audio?.ctx.resume().catch(() => {});
  syncMuteUi();
}

function toggleFullscreen() {
  const watch = $('#watch'); // the picture and the chat next to it (if open)
  if (document.fullscreenElement || document.webkitFullscreenElement) {
    (document.exitFullscreen || document.webkitExitFullscreen).call(document);
  } else if (watch.requestFullscreen) {
    watch.requestFullscreen({ navigationUI: 'hide' }).catch(() => {});
  } else if (watch.webkitRequestFullscreen) {
    watch.webkitRequestFullscreen();
  } else {
    watch.classList.toggle('pseudo-full'); // an iPhone: the whole window
  }
}

function toggleStats() {
  const box = $('#stats');
  box.hidden = !box.hidden;
  $('#btnStats').setAttribute('aria-pressed', String(!box.hidden));
  if (!box.hidden) directStatsTick();
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
  const player = $('#player');
  viewerChat();
  canvas().addEventListener('dblclick', toggleFullscreen);
  player.addEventListener('pointermove', showControls);
  player.addEventListener('pointerdown', showControls);

  const vol = $('#volume');
  vol.value = String(viewer.volume);
  vol.addEventListener('input', () => {
    viewer.volume = Number(vol.value);
    viewer.muted = viewer.volume === 0;
    store.set('ss-volume', viewer.volume);
    if (!viewer.muted) $('#btnUnmute').hidden = true;
    viewer.direct?.audio?.ctx.resume().catch(() => {});
    syncMuteUi();
  });

  $('#btnMute').addEventListener('click', toggleMute);
  $('#btnFullscreen').addEventListener('click', toggleFullscreen);
  $('#btnStats').addEventListener('click', toggleStats);
  $('#btnChat').addEventListener('click', () => toggleChat());
  $('#btnChatClose').addEventListener('click', () => toggleChat(false));
  $('#chatPop').addEventListener('click', () => toggleChat(true));
  // open from the start on a wide screen (unless closed last time); below the picture on a narrow one
  toggleChat(store.get('ss-chat-open', null) ?? window.matchMedia('(min-width: 1100px)').matches, false);
  $('#btnUnmute').addEventListener('click', () => {
    viewer.muted = false;
    if (viewer.volume === 0) viewer.volume = 1;
    $('#btnUnmute').hidden = true;
    viewer.direct?.audio?.ctx.resume().catch(() => {});
    syncMuteUi();
  });

  document.addEventListener('keydown', (e) => {
    if ($('#view-viewer').hidden || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.target instanceof HTMLInputElement || e.target instanceof HTMLSelectElement) return;
    const k = e.key.toLowerCase();
    if (k === 'f') toggleFullscreen();
    else if (k === 'm') toggleMute();
    else if (k === 's') toggleStats();
    else if (k === 'c') toggleChat();
    else if (k === 'escape' && $('#watch').classList.contains('pseudo-full')) $('#watch').classList.remove('pseudo-full');
    else return;
    e.preventDefault();
  });
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible' && hasPicture()) requestWakeLock(); });
}

/* ===================== DIRECT MODE: the viewer ===================== */
// Every frame is decoded and drawn the moment it arrives, on a low-latency canvas; the sound plays
// with a buffer that grows only when the network makes it run dry (direct-audio.js).

function onViewerDirectSession(msg) {
  if (!DIRECT) return;
  clearTimeout(viewer.offerTimer);
  closeDirectView();
  viewer.direct = {
    udp: msg.session,
    state: 'starting',
    ready: DIRECT.add('viewer', 'host', { session: msg.session, secret: msg.secret, port: msg.port, turn: turnInfo(viewer.iceServers) }),
    codec: DIRECT_CODEC,
    decoder: null,
    needKey: true,
    ctx: null,
    pending: new Map(),
    lat: [],
    frames: 0,
    fpsAt: performance.now(),
    drawn: false,
    offset: null,
    rtt: null,
    path: null,
    report: null,
    audio: null,
    audioOpening: false,
  };
}

async function onDirectOffer(data) {
  // the server's session message normally comes first; wait a moment if not
  for (let i = 0; i < 40 && viewer.direct?.udp !== data.udp; i++) await sleep(50);
  viewer.session = data.session;
  const d = viewer.direct;
  if (!d || d.udp !== data.udp) {
    closeDirectView();
    sendToHost({ type: 'direct-fail', why: 'unavailable' });
    return;
  }
  const res = await d.ready;
  if (viewer.direct !== d) return;
  if (!res || res.error) {
    closeDirectView();
    sendToHost({ type: 'direct-fail', why: 'unavailable' });
    return;
  }
  if (typeof data.codec === 'string') d.codec = data.codec;
  d.state = 'connecting';
  DIRECT.connect('host', { pub: data.pub, cands: data.cands, tcp: data.tcp === true });
  sendToHost({ type: 'direct-answer', pub: res.pub, cands: res.cands, tcp: res.tcp === true });
  if (!hasPicture()) setOverlay('Kapcsolódás a megosztóhoz…');
}

function closeDirectView() {
  const d = viewer.direct;
  if (!d) return;
  viewer.direct = null;
  try { d.decoder?.close(); } catch { /* already closed */ }
  d.latest?.close();
  d.latest = null;
  closeDirectAudio(d);
  DIRECT?.remove('host');
}

function onViewerDirectEvent(name, id, info) {
  const d = viewer.direct;
  if (!d || id !== 'host' || !info) return;
  if (name === 'state') {
    Object.assign(d, { state: info.state, path: info.path, rtt: info.rtt ?? d.rtt });
    if (info.state === 'failed') {
      closeDirectView();
      const why = info.diag && !info.diag.server && !info.diag.turn && !info.diag.tcp ? 'udp-blocked' : 'udp';
      sendToHost({ type: 'direct-fail', why });
      setOverlay(`A kapcsolat nem jött létre: ${WHY_TEXT[why]}. Újrapróbálom…`);
    } else if (info.state === 'lost' || info.state === 'closed') {
      closeDirectView();
      if (!viewer.closed) {
        setOverlay('A kapcsolat megszakadt. Újracsatlakozás…');
        sendToHost({ type: 'recall' });
      }
    }
  } else if (name === 'clock') {
    d.offset = info.offset;
    d.rtt = info.rtt;
  } else if (name === 'stats') {
    d.report = info;
  }
}

function decodeDirectVideo(d, meta, data) {
  if (!d.decoder || d.decoder.state === 'closed') {
    d.decoder = new VideoDecoder({
      output: (frame) => queueDirectFrame(d, frame),
      error: (err) => {
        console.warn('direct decoder', err);
        d.decoder = null;
        DIRECT.keyframe();
      },
    });
    d.decoder.configure({ codec: d.codec, optimizeForLatency: true, hardwareAcceleration: 'no-preference' });
    d.needKey = true;
  }
  if (d.needKey && !meta.key) {
    DIRECT.keyframe();
    return;
  }
  // the decoder falls behind (a slow computer, a picture too big for it): the frames up to the next
  // keyframe are left out – the picture stays live instead of the delay growing
  if (d.decoder.decodeQueueSize > 6 && !meta.key) {
    d.needKey = true;
    DIRECT.keyframe();
    return;
  }
  d.needKey = false;
  d.pending.set(meta.ts, meta.capture);
  if (d.pending.size > 60) d.pending.delete(d.pending.keys().next().value);
  d.decoder.decode(new EncodedVideoChunk({ type: meta.key ? 'key' : 'delta', timestamp: meta.ts, data }));
}

// Only the newest decoded frame is drawn: when drawing falls behind (a busy computer), older
// frames are dropped instead of waiting in line.
function queueDirectFrame(d, frame) {
  if (d.latest) {
    d.pending.delete(d.latest.timestamp);
    d.latest.close();
  }
  d.latest = frame;
  if (d.drawQueued) return;
  d.drawQueued = true;
  if (!d.drawPort) {
    const channel = new MessageChannel();
    channel.port1.onmessage = () => {
      d.drawQueued = false;
      const next = d.latest;
      d.latest = null;
      if (next) drawDirectFrame(d, next);
    };
    d.drawPort = channel.port2;
  }
  d.drawPort.postMessage(0);
}

// Drawn at once, outside the page's frame cycle: a low-latency ("desynchronized") canvas
// goes to the screen without waiting for the browser's compositor.
function drawDirectFrame(d, frame) {
  if (viewer.direct !== d) {
    frame.close();
    return;
  }
  const el = canvas();
  const w = frame.displayWidth;
  const h = frame.displayHeight;
  if (el.width !== w || el.height !== h) {
    el.width = w;
    el.height = h;
  }
  d.ctx ||= el.getContext('2d', { desynchronized: true, alpha: false });
  d.ctx.drawImage(frame, 0, 0, w, h);
  const capture = d.pending.get(frame.timestamp);
  d.pending.delete(frame.timestamp);
  frame.close();
  d.frames++;
  if (capture && d.offset != null) {
    d.lat.push(clockNow() + d.offset - capture);
    if (d.lat.length > 60) d.lat.shift();
  }
  if (!d.drawn) {
    d.drawn = true;
    el.hidden = false;
    if (viewer.live) {
      hideOverlay();
      setLive(true);
    }
    requestWakeLock();
  }
}

/* ---- the viewer's sound: Opus from WebCodecs, or our decoder (WebAssembly) ---- */

async function openDirectAudio(d, channels) {
  closeDirectAudio(d);
  const ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
  try {
    await ctx.audioWorklet.addModule('/direct-audio.js');
  } catch (err) {
    console.warn('direct audio', err);
    ctx.close().catch(() => {});
    return null;
  }
  const wasm = canDirect.audio === 'wasm' ? await globalThis.ScreenShareWasm.load().catch(() => null) : null;
  if (viewer.direct !== d || (canDirect.audio === 'wasm' && !wasm)) {
    ctx.close().catch(() => {});
    return null;
  }
  const max = ctx.destination.maxChannelCount;
  const out = channels >= 8 && max >= 8 ? 8 : channels >= 6 && max >= 6 ? 6 : 2;
  ctx.destination.channelCount = out;
  ctx.destination.channelInterpretation = 'discrete';
  const node = new AudioWorkletNode(ctx, 'direct-audio', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [out], processorOptions: { channels } });
  const gain = ctx.createGain();
  node.connect(gain).connect(ctx.destination);
  d.audio = { ctx, node, gain, channels, out, decoders: [], wasm };
  syncDirectVolume();
  ctx.resume().catch(() => {});
  // a browser opened from a link (no click yet) keeps the sound off: one click turns it on
  setTimeout(() => { if (ctx.state === 'suspended' && d.audio?.ctx === ctx && !viewer.muted) $('#btnUnmute').hidden = false; }, 400);
  return d.audio;
}

function closeDirectAudio(d) {
  const a = d.audio;
  if (!a) return;
  d.audio = null;
  for (const dec of a.decoders) {
    try { dec?.close(); } catch { /* already closed */ }
  }
  a.ctx.close().catch(() => {});
}

function syncDirectVolume() {
  const a = viewer.direct?.audio;
  if (a) a.gain.gain.value = viewer.muted ? 0 : viewer.volume;
}

/** A decoder for one channel pair: { decode(meta, data), close() } – planes to the player. */
function pairDecoder(a, pair, channels) {
  const send = (ts, planes) => a.node.port.postMessage({ pair, ts, frames: planes[0].length, planes }, planes.map((p) => p.buffer));
  if (a.wasm) {
    const dec = a.wasm.opusDecoder(channels);
    return {
      state: 'configured',
      decode(meta, data) {
        const pcm = dec.decode(data);
        if (!pcm) return;
        const frames = pcm.length / channels;
        const planes = Array.from({ length: channels }, (_, c) => {
          const plane = new Float32Array(frames);
          for (let i = 0; i < frames; i++) plane[i] = pcm[i * channels + c];
          return plane;
        });
        send(meta.ts, planes);
      },
      close() { dec.close(); this.state = 'closed'; },
    };
  }
  const dec = new AudioDecoder({
    output: (ad) => {
      const planes = Array.from({ length: ad.numberOfChannels }, (_, c) => {
        const plane = new Float32Array(ad.numberOfFrames);
        ad.copyTo(plane, { planeIndex: c, format: 'f32-planar' });
        return plane;
      });
      send(ad.timestamp, planes);
      ad.close();
    },
    error: (err) => console.warn('direct audio decoder', err),
  });
  dec.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: channels });
  return {
    get state() { return dec.state; },
    decode(meta, data) {
      if (dec.state === 'configured') dec.decode(new EncodedAudioChunk({ type: 'key', timestamp: meta.ts, data }));
    },
    close() { dec.close(); },
  };
}

async function playDirectAudio(d, meta, data) {
  if (!canDirect.audio) return; // this browser has no way to play it
  const channels = Math.max(1, Math.min(8, meta.sub >> 4));
  const pair = meta.sub & 15;
  if (pair * 2 >= channels) return;
  let a = d.audio;
  if (!a || a.channels !== channels) {
    if (d.audioOpening) return; // the first packets while the player starts
    d.audioOpening = true;
    try {
      a = await openDirectAudio(d, channels);
    } finally {
      d.audioOpening = false;
    }
    if (!a) return;
  }
  let dec = a.decoders[pair];
  if (!dec || dec.state === 'closed') {
    dec = pairDecoder(a, pair, Math.min(2, channels - pair * 2));
    a.decoders[pair] = dec;
  }
  dec.decode(meta, data);
}

function onDirectMedia(meta, data) {
  const d = viewer.direct;
  if (!d || host.code || !meta) return;
  if (meta.stream === 0) decodeDirectVideo(d, meta, data);
  else playDirectAudio(d, meta, data);
}

function onDirectEvent(name, id, info) {
  if (host.code) onHostDirectEvent(name, id, info);
  else onViewerDirectEvent(name, id, info);
}

function renderStatsRows(rows) {
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

function directStatsTick() {
  const d = viewer.direct;
  if (!d) {
    $('#viewerInfo').textContent = '';
    return;
  }
  const now = performance.now();
  const fps = (d.frames * 1000) / Math.max(1, now - d.fpsAt);
  d.frames = 0;
  d.fpsAt = now;
  const lat = median(d.lat);
  const r = d.report;
  const el = canvas();
  const res = d.drawn ? `${el.width}×${el.height}` : '–';
  if (viewer.tick++ % 2 === 0 && lat != null) sendToHost({ type: 'stats', lat: Math.round(lat), fps: Math.round(fps), loss: r?.loss ?? 0, measured: true });
  $('#viewerInfo').textContent = d.drawn ? `${res} · ${Math.round(fps)} FPS${lat != null ? ` · ${Math.round(lat)} ms` : ''}` : '';
  if ($('#stats').hidden) return;
  const a = d.audio;
  const own = 'saját C++ kód (WebAssembly)';
  renderStatsRows([
    ['Átvitel', 'saját protokoll, végig titkosítva (WebRTC nélkül)'],
    ['Kulcscsere', `X25519 · ${canDirect.keys === 'wasm' ? own : 'WebCrypto'}`],
    ['Felbontás', res],
    ['FPS', d.drawn ? String(Math.round(fps)) : '–'],
    ['Bitráta', fmtKbps(r?.kbps ?? null)],
    ['Kodek', 'H.264'],
    ['Küldő kódolója', senderEncoderText(viewer.hostStats), viewer.hostStats?.hw === false],
    ['Késés (mért)', lat != null ? `${Math.round(lat)} ms` : '–', lat > 100],
    ['RTT', fmtMs(d.rtt)],
    ['Kihagyott képek', String(r?.skips ?? 0), (r?.skips ?? 0) > 0],
    ['Kapcsolat', PATH_LABEL[d.path] || '–'],
    ['Hang', !canDirect.audio ? 'ez a böngésző nem tudja lejátszani' : a ? `Opus · ${a.wasm ? own : 'WebCodecs'} · ${a.channels > 2 ? `${a.channels} csatorna → ${a.out} hangszóró` : 'sztereó'}` : 'nincs', !canDirect.audio],
  ]);
}

/* ============================== the room's chat ============================== */
// Everyone in the room – the sharer and every viewer – writes to everyone. The server passes it on
// with the sender's name (the one given here, or its own: Megosztó / Néző N) and keeps the last 50
// for those who come later. The sharer's chat is a box next to the preview; the viewer's a panel
// next to the picture (in full screen too), with a count of what came while it was closed.

const CHAT_KEEP = 200; // messages kept on the page
const chatClock = new Intl.DateTimeFormat('hu-HU', { hour: '2-digit', minute: '2-digit' });
const chatName = () => store.get('ss-name', '') || undefined;

/** A colour of its own for each writer (the sharer has the accent colour). */
function chatHue(from) {
  let h = 0;
  for (const ch of String(from)) h = (Math.imul(h, 31) + ch.codePointAt(0)) >>> 0;
  return h % 360;
}

function chatTooFast() {
  toast('Lassabban – túl sok üzenet egyszerre. Pár másodperc múlva mehet.', 'warn');
}

/** One chat on the page: its list, the box to write in, the name. `send` gives a message to the server. */
function createChat({ list, empty, form, input, nameInput, send, onMessage }) {
  let me = null;
  const add = (m) => {
    const li = document.createElement('li');
    li.className = `chat-msg${m.from === me ? ' mine' : ''}${m.from === 'host' ? ' from-host' : ''}`;
    const meta = document.createElement('div');
    meta.className = 'chat-meta';
    const name = document.createElement('b');
    name.textContent = String(m.name || '');
    if (m.from !== 'host') name.style.color = `hsl(${chatHue(m.from)} 80% 74%)`;
    const time = document.createElement('time');
    time.textContent = Number.isFinite(m.at) ? chatClock.format(new Date(m.at)) : '';
    meta.append(name, time);
    const text = document.createElement('p');
    text.textContent = String(m.text || ''); // (as text: never HTML)
    li.append(meta, text);
    const atEnd = list.scrollHeight - list.scrollTop - list.clientHeight < 48;
    list.append(li);
    while (list.children.length > CHAT_KEEP) list.firstElementChild.remove();
    empty.hidden = true;
    if (atEnd || m.from === me) list.scrollTop = list.scrollHeight;
  };
  const chat = {
    /** In the room (again): who we are there, what was said so far, the name the server gave us. */
    joined(res, id) {
      me = id;
      list.replaceChildren();
      empty.hidden = false;
      for (const m of Array.isArray(res.chat) ? res.chat : []) add(m);
      list.scrollTop = list.scrollHeight;
      if (typeof res.name === 'string') nameInput.placeholder = res.name;
    },
    message(m, id) {
      me = id;
      add(m);
      onMessage?.(m, m.from === me);
    },
  };
  form.addEventListener('submit', (e) => {
    e.preventDefault();
    const text = input.value.trim();
    if (!text) return;
    if (send({ t: 'chat', text })) input.value = '';
    else toast('Nincs kapcsolat a szerverrel – az üzenet nem ment el.', 'warn');
  });
  input.addEventListener('keydown', (e) => { if (e.key === 'Escape') input.blur(); });
  nameInput.value = store.get('ss-name', '');
  nameInput.addEventListener('change', () => {
    const name = nameInput.value.trim().slice(0, 24);
    nameInput.value = name;
    store.set('ss-name', name);
    if (name) send({ t: 'name', name });
  });
  nameInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); nameInput.blur(); } });
  return chat;
}

let hostChatBox = null;
function hostChat() {
  hostChatBox ??= createChat({
    list: $('#hostChatList'),
    empty: $('#hostChatEmpty'),
    form: $('#hostChatForm'),
    input: $('#hostChatInput'),
    nameInput: $('#hostChatName'),
    send: (m) => Boolean(host.code && host.signal?.send(m)),
    onMessage: (m, mine) => { if (!mine && document.visibilityState === 'hidden') toast(`${m.name}: ${m.text}`, 'info', 6000); },
  });
  return hostChatBox;
}

let viewerChatBox = null;
function viewerChat() {
  viewerChatBox ??= createChat({
    list: $('#viewerChatList'),
    empty: $('#viewerChatEmpty'),
    form: $('#viewerChatForm'),
    input: $('#viewerChatInput'),
    nameInput: $('#viewerChatName'),
    send: (m) => Boolean(viewer.id && viewer.signal?.send(m)),
    onMessage: (m, mine) => {
      if (mine || !$('#viewerChat').hidden) return;
      viewer.unread = (viewer.unread || 0) + 1;
      renderChatBadge();
      popChat(m);
    },
  });
  return viewerChatBox;
}

function renderChatBadge() {
  const badge = $('#chatBadge');
  badge.hidden = !viewer.unread;
  badge.textContent = viewer.unread > 99 ? '99+' : String(viewer.unread || '');
}

/** The viewer's chat panel on (true) or off (false), or the other way (no argument). */
function toggleChat(open = $('#viewerChat').hidden, remember = true) {
  const panel = $('#viewerChat');
  panel.hidden = !open;
  $('#btnChat').setAttribute('aria-pressed', String(open));
  if (remember) store.set('ss-chat-open', open);
  if (!open) return;
  viewer.unread = 0;
  renderChatBadge();
  $('#chatPop').replaceChildren();
  const list = $('#viewerChatList');
  list.scrollTop = list.scrollHeight;
  // ready to type – not on a touch screen, where that would push the keyboard up
  if (remember && window.matchMedia('(hover: hover)').matches) $('#viewerChatInput').focus({ preventScroll: true });
}

/** With the panel closed, a new message shows over the picture for a few seconds (in full screen too). */
function popChat(m) {
  const pop = $('#chatPop');
  const line = document.createElement('div');
  const name = document.createElement('b');
  name.textContent = `${m.name}: `;
  if (m.from !== 'host') name.style.color = `hsl(${chatHue(m.from)} 80% 74%)`;
  line.append(name, document.createTextNode(m.text));
  pop.append(line);
  while (pop.children.length > 3) pop.firstElementChild.remove();
  setTimeout(() => line.remove(), 7000);
}

/* ============================== boot ============================== */

function init() {
  if (IS_IOS) document.documentElement.classList.add('is-ios'); // no volume control: iOS keeps media at full volume
  if (window.ScreenShareWebDirect) {
    directReady = window.ScreenShareWebDirect.supported().then((s) => {
      canDirect = s;
      if (!s.ok) return;
      DIRECT = window.ScreenShareWebDirect.create({ tcpUrl: `${location.origin.replace(/^http/, 'ws')}/direct` });
      DIRECT.onEvent(onDirectEvent);
      DIRECT.onMedia(onDirectMedia);
    }).catch(() => {});
  }
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
