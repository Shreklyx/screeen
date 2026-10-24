'use strict';

// Screen Share desktop app (Windows / macOS).
//
// The UI and the WebRTC code are the website itself (loaded from the server),
// so the app always matches the server. What the app adds on top of a browser:
//  - its own screen/window picker, and the system audio comes along
//    automatically (Windows, and macOS 13+ – no extra audio driver needed),
//  - 5.1 / 7.1 surround system audio on Windows (native/ss-audio.cpp),
//  - laptops with two GPUs use the strong one (NVIDIA NVENC instead of the
//    integrated chip), H.265 hardware encoding is allowed,
//  - sharing is never throttled when the window is minimised or covered, and the
//    capture/encode processes get a higher priority while sharing (a game
//    running next to it cannot starve them),
//  - the computer does not go to sleep while sharing,
//  - direct mode between two apps: the picture goes without WebRTC, over the app's own
//    UDP transport (direct.js) – no jitter buffer, the least delay.

const { app, BrowserWindow, Menu, desktopCapturer, dialog, ipcMain, powerSaveBlocker, screen, session, shell, systemPreferences } = require('electron');
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { DirectEndpoint } = require('./direct');

const DEFAULT_URL = 'https://mozizas.hu/';
const IS_MAC = process.platform === 'darwin';
const IS_WIN = process.platform === 'win32';
// System audio of the whole computer: Windows, and macOS 13+ (CoreAudio tap / ScreenCaptureKit).
const SYSTEM_AUDIO = IS_WIN || (IS_MAC && Number(os.release().split('.')[0]) >= 22);
// 5.1 / 7.1 capture helper (Windows): every channel of what the computer plays.
const AUDIO_HELPER = process.env.SS_AUDIO_HELPER
  || (IS_WIN ? path.join(app.isPackaged ? process.resourcesPath : path.join(__dirname, 'native'), 'ss-audio.exe') : '');
const HAS_AUDIO_HELPER = Boolean(AUDIO_HELPER) && fs.existsSync(AUDIO_HELPER);

/* ------------------------------------------------ Chromium switches (before 'ready') */

// Two-GPU laptops: run on the discrete GPU (NVIDIA → NVENC hardware encoding).
app.commandLine.appendSwitch('force_high_performance_gpu');
// Never slow down capture/encoding in the background.
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('disable-backgrounding-occluded-windows');
// H.265 in WebRTC (hardware HEVC on NVIDIA and Apple) – used when both ends support it.
app.commandLine.appendSwitch('enable-features', 'WebRtcAllowH265Send,WebRtcAllowH265Receive,PlatformHEVCEncoderSupport');
// Windows: a covered window must not be treated as hidden.
app.commandLine.appendSwitch('disable-features', 'CalculateNativeWinOcclusion');
// Less delay:
//  - viewers show every frame the moment it is decoded: zero playout delay, sent with the
//    video (RTP header extension); viewers that pick a buffer ("Simább") leave it out,
//  - a frame leaves at 3× the target bitrate instead of 1.1× (libwebrtc's default spreads
//    one 4K frame over most of the frame interval), and the send queue is kept under 200 ms.
app.commandLine.appendSwitch('force-fieldtrials', [
  'WebRTC-ForceSendPlayoutDelay/min_ms:0,max_ms:0/',
  'WebRTC-Video-Pacing/factor:3,max_delay:200ms/',
].join(''));

/* ------------------------------------------------ settings */

const configFile = () => path.join(app.getPath('userData'), 'config.json');

function loadConfig() {
  try {
    return JSON.parse(fs.readFileSync(configFile(), 'utf8'));
  } catch {
    return {};
  }
}

function saveConfig(patch) {
  const next = { ...loadConfig(), ...patch };
  try {
    fs.mkdirSync(path.dirname(configFile()), { recursive: true });
    fs.writeFileSync(configFile(), JSON.stringify(next, null, 2));
  } catch (err) {
    console.error('config save failed', err);
  }
}

function normalizeUrl(raw) {
  try {
    const u = new URL(String(raw).includes('://') ? raw : `https://${raw}`);
    if (u.protocol !== 'https:' && u.protocol !== 'http:') return null;
    return `${u.origin}/`;
  } catch {
    return null;
  }
}

const arg = (name) => process.argv.find((a) => a.startsWith(`--${name}=`))?.slice(name.length + 3);
const SMOKE = process.argv.includes('--smoke-test');
const SMOKE_CAPTURE = process.argv.includes('--smoke-capture');

let serverUrl = DEFAULT_URL;
const serverOrigin = () => new URL(serverUrl).origin;
const isServerUrl = (url) => {
  try {
    return new URL(url).origin === serverOrigin();
  } catch {
    return false;
  }
};
const isLocalPage = (url) => String(url).startsWith('file:');

/* ------------------------------------------------ main window */

let mainWindow = null;

function loadServer() {
  mainWindow.loadURL(serverUrl);
}

// Last window position, unless that monitor is gone.
function savedBounds() {
  const b = loadConfig().bounds;
  if (!b || !b.width || !b.height) return {};
  const visible = screen.getAllDisplays().some(({ workArea: w }) =>
    b.x < w.x + w.width - 80 && b.x + b.width > w.x + 80 && b.y >= w.y - 20 && b.y < w.y + w.height - 80);
  return visible ? b : { width: b.width, height: b.height };
}

function createMainWindow() {
  const bounds = savedBounds();
  mainWindow = new BrowserWindow({
    width: bounds.width || 1360,
    height: bounds.height || 880,
    x: bounds.x,
    y: bounds.y,
    minWidth: 860,
    minHeight: 560,
    backgroundColor: '#0b0b10',
    title: 'Screen Share',
    show: false,
    autoHideMenuBar: IS_WIN,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      sandbox: true,
      nodeIntegration: false,
      backgroundThrottling: false,
      spellcheck: false,
      additionalArguments: [`--ss-version=${app.getVersion()}`, ...(HAS_AUDIO_HELPER ? ['--ss-surround'] : [])],
    },
  });

  mainWindow.once('ready-to-show', () => {
    if (!SMOKE) mainWindow.show();
  });
  mainWindow.on('close', () => saveConfig({ bounds: mainWindow.getNormalBounds() }));
  mainWindow.on('closed', () => {
    mainWindow = null;
    stopAudioHelper();
    stopDirect();
  });

  const wc = mainWindow.webContents;
  wc.on('did-start-navigation', (details) => {
    if (details.isMainFrame && !details.isSameDocument) {
      stopAudioHelper();
      stopDirect();
    }
  });
  wc.on('render-process-gone', () => {
    stopAudioHelper();
    stopDirect();
  });
  // Server not reachable → local page with a retry button.
  wc.on('did-fail-load', (_e, code, description, url, isMainFrame) => {
    if (!isMainFrame || code === -3 /* aborted */ || isLocalPage(url)) return;
    mainWindow.loadFile(path.join(__dirname, 'pages', 'offline.html'), { query: { url: serverUrl, error: description } });
  });
  // Only the server and our own pages open inside the app; everything else in the browser.
  wc.setWindowOpenHandler(({ url }) => {
    if (/^https?:/i.test(url)) shell.openExternal(url);
    return { action: 'deny' };
  });
  wc.on('will-navigate', (e, url) => {
    if (isServerUrl(url) || isLocalPage(url)) return;
    e.preventDefault();
    if (/^https?:/i.test(url)) shell.openExternal(url);
  });

  loadServer();
}

/* ------------------------------------------------ permissions */

const ALLOWED_PERMISSIONS = new Set(['media', 'display-capture', 'fullscreen', 'clipboard-sanitized-write', 'screen-wake-lock', 'speaker-selection']);

function setupPermissions(ses) {
  ses.setPermissionRequestHandler((wc, permission, callback, details) => {
    const url = details?.requestingUrl || wc.getURL();
    let ok = isServerUrl(url) && ALLOWED_PERMISSIONS.has(permission);
    // audio inputs (the Hangforrás list) yes, camera no
    if (permission === 'media' && (details?.mediaTypes || []).includes('video')) ok = false;
    callback(ok);
  });
  ses.setPermissionCheckHandler((_wc, permission, origin) => origin === serverOrigin() && ALLOWED_PERMISSIONS.has(permission));
}

/* ------------------------------------------------ screen picker */

let pickerWindow = null;
// Sources from the last listing: the chosen one is captured directly, without enumerating again.
let listedSources = new Map();

async function listSources() {
  const own = mainWindow?.getMediaSourceId();
  const sources = await desktopCapturer.getSources({ types: ['screen', 'window'], thumbnailSize: { width: 384, height: 216 }, fetchWindowIcons: true });
  listedSources = new Map(sources.map((s) => [s.id, s]));
  const screens = sources.filter((s) => s.id.startsWith('screen:'));
  const screenName = (s) => (screens.length > 1 ? `${screens.indexOf(s) + 1}. képernyő` : 'Teljes képernyő');
  return sources
    .filter((s) => s.id !== own && (s.id.startsWith('screen:') || s.name))
    .map((s) => ({
      id: s.id,
      name: s.id.startsWith('screen:') ? screenName(s) : s.name,
      type: s.id.startsWith('screen:') ? 'screen' : 'window',
      thumbnail: s.thumbnail.isEmpty() ? null : s.thumbnail.toDataURL(),
      icon: s.appIcon && !s.appIcon.isEmpty() ? s.appIcon.toDataURL() : null,
    }));
}

function screenPermissionOk() {
  if (!IS_MAC) return true;
  const status = systemPreferences.getMediaAccessStatus('screen');
  if (status === 'granted' || status === 'not-determined') return true;
  const choice = dialog.showMessageBoxSync(mainWindow, {
    type: 'warning',
    buttons: ['Beállítások megnyitása', 'Mégse'],
    defaultId: 0,
    message: 'A macOS nem engedi a képernyőfelvételt',
    detail: 'Rendszerbeállítások → Adatvédelem és biztonság → Képernyőfelvétel (újabb macOS-en: Képernyő- és rendszerhangfelvétel): kapcsold be a Screen Share-t, majd indítsd újra az alkalmazást.',
  });
  if (choice === 0) shell.openExternal('x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture');
  return false;
}

/** Shows the picker; resolves to { id, audio } or null when cancelled. */
function pickSource(audioRequested) {
  if (SMOKE) {
    return listSources().then((list) => {
      const screen = list.find((s) => s.type === 'screen') || list[0];
      return screen ? { id: screen.id, audio: audioRequested && SYSTEM_AUDIO } : null;
    });
  }
  if (pickerWindow) {
    pickerWindow.focus();
    return Promise.resolve(null);
  }
  return new Promise((resolve) => {
    let done = false;
    const finish = (value) => {
      if (done) return;
      done = true;
      ipcMain.removeHandler('picker:sources');
      ipcMain.removeAllListeners('picker:choose');
      if (pickerWindow && !pickerWindow.isDestroyed()) pickerWindow.close();
      pickerWindow = null;
      resolve(value);
    };
    pickerWindow = new BrowserWindow({
      parent: mainWindow,
      modal: true,
      width: 860,
      height: 620,
      minWidth: 600,
      minHeight: 420,
      backgroundColor: '#14141c',
      title: 'Mit osztasz meg?',
      autoHideMenuBar: true,
      webPreferences: {
        preload: path.join(__dirname, 'picker-preload.js'),
        contextIsolation: true,
        sandbox: true,
        nodeIntegration: false,
        additionalArguments: [
          `--ss-audio=${audioRequested && SYSTEM_AUDIO ? '1' : '0'}`,
          `--ss-old-mac=${IS_MAC && !SYSTEM_AUDIO ? '1' : '0'}`,
        ],
      },
    });
    ipcMain.handle('picker:sources', () => listSources());
    ipcMain.on('picker:choose', (e, choice) => {
      if (e.sender !== pickerWindow?.webContents) return;
      finish(choice && typeof choice.id === 'string' ? { id: choice.id, audio: Boolean(choice.audio) } : null);
    });
    pickerWindow.on('closed', () => finish(null));
    pickerWindow.loadFile(path.join(__dirname, 'pages', 'picker.html'));
  });
}

function setupDisplayMedia(ses) {
  ses.setDisplayMediaRequestHandler(async (request, callback) => {
    try {
      if (!isServerUrl(request.securityOrigin) || !screenPermissionOk()) return callback(null);
      const choice = await pickSource(request.audioRequested);
      if (!choice) return callback(null);
      let source = listedSources.get(choice.id);
      for (let attempt = 0; !source && attempt < 2; attempt++) {
        if (attempt) await new Promise((r) => setTimeout(r, 300));
        const sources = await desktopCapturer.getSources({ types: ['screen', 'window'], thumbnailSize: { width: 0, height: 0 } });
        source = sources.find((s) => s.id === choice.id);
      }
      if (!source) return callback(null);
      const streams = { video: source };
      if (process.env.SS_DEBUG) console.log('display media:', JSON.stringify({ choice, source: source.id, audioRequested: request.audioRequested }));
      // The whole system sound ("loopback"). macOS asks once for "System Audio Recording";
      // if it is refused, the page gets an ended audio track and says what to do.
      if (SYSTEM_AUDIO && choice.audio && request.audioRequested) streams.audio = 'loopback';
      callback(streams);
    } catch (err) {
      console.error('display media request failed', err);
      callback(null);
    }
  });
}

/* ------------------------------------------------ while sharing */

let sleepBlocker = null;
let priorityTimer = null;

// Capture, encoding and networking get more CPU time than e.g. a game next to them.
function setProcessPriority(boost) {
  const level = boost ? os.constants.priority.PRIORITY_ABOVE_NORMAL : os.constants.priority.PRIORITY_NORMAL;
  for (const m of app.getAppMetrics()) {
    if (!['Browser', 'GPU', 'Tab', 'Utility'].includes(m.type)) continue;
    try {
      os.setPriority(m.pid, level);
    } catch {
      // raising priority needs extra rights on macOS – fine
    }
  }
}

function setSharing(on) {
  if (on && sleepBlocker == null) {
    sleepBlocker = powerSaveBlocker.start('prevent-display-sleep');
    setProcessPriority(true);
    priorityTimer = setInterval(() => setProcessPriority(true), 10000); // processes started later too
  } else if (!on && sleepBlocker != null) {
    powerSaveBlocker.stop(sleepBlocker);
    sleepBlocker = null;
    clearInterval(priorityTimer);
    priorityTimer = null;
    setProcessPriority(false);
  }
}

/* ------------------------------------------------ 5.1 / 7.1 system audio (Windows) */

// Chromium records the system sound in stereo. When the output device is 5.1/7.1,
// the page asks for the helper, which streams every channel (see native/ss-audio.cpp).
let audioHelper = null; // { id, proc, wc }
let audioHelperSeq = 0;

function stopAudioHelper(id) {
  if (!audioHelper || (id != null && audioHelper.id !== id)) return;
  const { proc } = audioHelper;
  audioHelper = null;
  proc.stdin.end(); // the helper exits when its stdin closes
  setTimeout(() => {
    if (proc.exitCode == null && proc.signalCode == null) proc.kill();
  }, 1000);
}

/** Starts the helper; resolves to { id, rate, channels, mask } or { error }. */
function startAudioHelper(wc) {
  stopAudioHelper();
  return new Promise((resolve) => {
    let proc;
    try {
      proc = spawn(AUDIO_HELPER, [], { stdio: ['pipe', 'pipe', 'ignore'], windowsHide: true });
    } catch (err) {
      resolve({ error: err.message });
      return;
    }
    const self = { id: ++audioHelperSeq, proc, wc };
    audioHelper = self;
    let settled = false;
    const settle = (value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      if (value.error) stopAudioHelper(self.id);
      resolve(value);
    };
    const timer = setTimeout(() => settle({ error: 'timeout' }), 3000);
    proc.on('error', (err) => settle({ error: err.message }));
    proc.on('exit', () => {
      settle({ error: 'exited' });
      if (audioHelper === self) audioHelper = null;
    });
    proc.stdin.on('error', () => {}); // already gone
    let pending = Buffer.alloc(0);
    proc.stdout.on('data', (chunk) => {
      pending = pending.length ? Buffer.concat([pending, chunk]) : chunk;
      while (pending.length >= 8) {
        const size = pending.readUInt32LE(4);
        if (pending.length < 8 + size) break;
        const tag = pending.toString('latin1', 0, 4);
        const body = pending.subarray(8, 8 + size);
        pending = pending.subarray(8 + size);
        if (audioHelper !== self || wc.isDestroyed()) continue;
        if (tag === 'PCM1') {
          wc.send('audio:data', new Uint8Array(body)); // exact-size copy for the IPC
        } else if (tag === 'FMT1' && size >= 12) {
          const format = { id: self.id, rate: body.readUInt32LE(0), channels: body.readUInt32LE(4), mask: body.readUInt32LE(8) };
          wc.send('audio:format', format);
          settle(format);
        } else if (tag === 'ERR1') {
          settle({ error: body.toString('utf8') });
        }
      }
    });
  });
}

/* ------------------------------------------------ direct mode (app ↔ app, no WebRTC) */

let direct = null; // { ep, role, wc }

function stopDirect() {
  direct?.ep.close();
  direct = null;
}

async function openDirect(wc, role) {
  if (direct && direct.role === role && direct.wc === wc) return direct;
  stopDirect();
  const ep = new DirectEndpoint({ role, serverHost: new URL(serverUrl).hostname, forceRelay: process.env.SS_DIRECT_RELAY === '1' });
  const self = { ep, role, wc };
  direct = self;
  const live = () => direct === self && !wc.isDestroyed();
  for (const name of ['state', 'keyframe', 'report', 'clock', 'stats']) {
    ep.on(name, (...args) => { if (live()) wc.send('direct:event', name, ...args); });
  }
  ep.on('media', (meta, data) => { if (live()) wc.send('direct:media', meta, data); });
  ep.on('log', (line) => { if (process.env.SS_DEBUG) console.log(line); });
  await ep.open();
  return self;
}

/* ------------------------------------------------ IPC from the pages */

function setupIpc() {
  ipcMain.on('app:sharing', (e, on) => {
    if (isServerUrl(e.senderFrame?.url)) setSharing(Boolean(on));
  });
  ipcMain.on('app:retry', (e) => {
    if (isLocalPage(e.senderFrame?.url) && mainWindow) loadServer();
  });
  ipcMain.on('app:settings', () => openSettings());
  ipcMain.handle('audio:start', (e) => {
    if (!HAS_AUDIO_HELPER || e.sender !== mainWindow?.webContents || !isServerUrl(e.senderFrame?.url)) return { error: 'unavailable' };
    return startAudioHelper(e.sender);
  });
  ipcMain.on('audio:stop', (e, id) => {
    if (audioHelper?.wc === e.sender) stopAudioHelper(Number(id));
  });
  ipcMain.handle('direct:add', async (e, role, id, info) => {
    if (e.sender !== mainWindow?.webContents || !isServerUrl(e.senderFrame?.url) || !['host', 'viewer'].includes(role) || !info) return { error: 'unavailable' };
    try {
      const d = await openDirect(e.sender, role);
      return await d.ep.addPeer(String(id), { session: Number(info.session), secret: String(info.secret), port: Number(info.port) || 0 });
    } catch (err) {
      return { error: err.message };
    }
  });
  ipcMain.on('direct:connect', (e, id, answer) => {
    if (direct?.wc !== e.sender) return;
    try {
      direct.ep.connectPeer(String(id), answer || {});
    } catch {
      e.sender.send('direct:event', 'state', String(id), { state: 'failed', path: null, rtt: null });
    }
  });
  ipcMain.on('direct:remove', (e, id) => { if (direct?.wc === e.sender) direct.ep.removePeer(String(id)); });
  ipcMain.on('direct:send', (e, meta, data) => { if (direct?.wc === e.sender && direct.role === 'host' && meta && data) direct.ep.sendMedia(meta, data); });
  ipcMain.on('direct:keyframe', (e) => { if (direct?.wc === e.sender && direct.role === 'viewer') direct.ep.keyframe('host'); });
  ipcMain.on('direct:bitrate', (e, bps) => { if (direct?.wc === e.sender && direct.role === 'host') direct.ep.setBitrate(Number(bps)); });
  ipcMain.on('direct:stop', (e) => { if (direct?.wc === e.sender) stopDirect(); });
  ipcMain.handle('app:get-url', () => serverUrl);
  ipcMain.handle('app:set-url', (e, raw) => {
    if (!isLocalPage(e.senderFrame?.url)) return { ok: false };
    const url = normalizeUrl(raw);
    if (!url) return { ok: false, error: 'Érvénytelen cím.' };
    serverUrl = url;
    saveConfig({ serverUrl: url });
    if (mainWindow) loadServer();
    return { ok: true };
  });
}

let settingsWindow = null;
function openSettings() {
  if (settingsWindow) {
    settingsWindow.focus();
    return;
  }
  settingsWindow = new BrowserWindow({
    parent: mainWindow || undefined,
    modal: Boolean(mainWindow),
    width: 520,
    height: 300,
    resizable: false,
    backgroundColor: '#14141c',
    title: 'Szerver címe',
    autoHideMenuBar: true,
    webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true, nodeIntegration: false },
  });
  settingsWindow.on('closed', () => {
    settingsWindow = null;
  });
  settingsWindow.loadFile(path.join(__dirname, 'pages', 'settings.html'));
}

/* ------------------------------------------------ menu */

function buildMenu() {
  const template = [
    ...(IS_MAC ? [{ role: 'appMenu', label: 'Screen Share' }] : []),
    {
      label: 'Fájl',
      submenu: [
        { label: 'Szerver címe…', click: openSettings },
        { type: 'separator' },
        IS_MAC ? { role: 'close', label: 'Ablak bezárása' } : { role: 'quit', label: 'Kilépés' },
      ],
    },
    {
      label: 'Nézet',
      submenu: [
        { role: 'reload', label: 'Újratöltés' },
        { role: 'togglefullscreen', label: 'Teljes képernyő' },
        { type: 'separator' },
        { role: 'resetZoom', label: 'Eredeti méret' },
        { role: 'zoomIn', label: 'Nagyítás' },
        { role: 'zoomOut', label: 'Kicsinyítés' },
        { type: 'separator' },
        { role: 'toggleDevTools', label: 'Fejlesztői eszközök' },
      ],
    },
    { role: 'windowMenu', label: 'Ablak' },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

/* ------------------------------------------------ self-test (CI) */

async function runSmokeTest() {
  const log = (...a) => {
    const line = a.join(' ');
    console.log(line);
    const file = arg('smoke-log');
    if (file) fs.appendFileSync(file, `${line}\n`);
  };
  const timer = setTimeout(() => {
    log('SMOKE FAIL: timeout');
    app.exit(2);
  }, 90000);
  try {
    const wc = mainWindow.webContents;
    if (wc.isLoading()) await new Promise((r) => wc.once('did-finish-load', r));
    const page = await wc.executeJavaScript(`({
      title: document.title,
      url: location.href,
      app: Boolean(window.screenshareApp && window.screenshareApp.platform),
      shareButton: Boolean(document.querySelector('#btnShare')),
    })`);
    log('page', JSON.stringify(page));
    if (!page.app || !page.shareButton || !isServerUrl(page.url)) throw new Error('page or app bridge missing');
    log(`gpu: ${JSON.stringify(app.getGPUFeatureStatus()).slice(0, 400)}`);
    if (SMOKE_CAPTURE) {
      const cap = await wc.executeJavaScript(`(async () => {
        let stream, audioError = null;
        try {
          stream = await navigator.mediaDevices.getDisplayMedia({ video: true, audio: true });
        } catch (e) {
          audioError = e.name + ': ' + e.message;
          try {
            stream = await navigator.mediaDevices.getDisplayMedia({ video: true });
          } catch (e2) {
            return { video: 'failed', error: e2.name + ': ' + e2.message, audioError };
          }
        }
        const v = stream.getVideoTracks()[0];
        const s = v.getSettings();
        const result = { video: v.readyState, width: s.width, height: s.height, audioTracks: stream.getAudioTracks().length, audioError };
        stream.getTracks().forEach((t) => t.stop());
        return result;
      })()`, true);
      log('capture', JSON.stringify(cap));
      if (cap.video !== 'live' || !cap.width) throw new Error(`screen capture failed: ${cap.error || ''}`);
    }
    if (IS_WIN) {
      // the 5.1/7.1 helper is packaged and runs (a runner may have no audio device: that is reported, not fatal)
      if (!HAS_AUDIO_HELPER) throw new Error(`audio helper missing: ${AUDIO_HELPER}`);
      const probe = spawnSync(AUDIO_HELPER, ['--probe'], { encoding: 'utf8', timeout: 15000, windowsHide: true });
      log('audio helper probe', (probe.stdout || '').trim(), `exit ${probe.status}`);
      try {
        JSON.parse(probe.stdout);
      } catch {
        throw new Error(`audio helper did not run: ${probe.error?.message || probe.stderr || probe.status}`);
      }
      const started = await wc.executeJavaScript(`window.screenshareApp.systemAudio.start().then((r) => {
        window.screenshareApp.systemAudio.stop(r.id);
        return r;
      })`, true);
      log('audio helper start', JSON.stringify(started));
    }
    // direct mode: two endpoints in this process exchange a frame (UDP, X25519, AES-GCM)
    const loop = await directSelfTest();
    log('direct self-test', JSON.stringify(loop));
    if (!loop.ok) throw new Error(`direct mode transport failed: ${loop.error || ''}`);
    log('SMOKE OK');
    clearTimeout(timer);
    app.exit(0);
  } catch (err) {
    log(`SMOKE FAIL: ${err?.message || err}`);
    clearTimeout(timer);
    app.exit(1);
  }
}

async function directSelfTest() {
  const host = new DirectEndpoint({ role: 'host', serverHost: '127.0.0.1' });
  const viewer = new DirectEndpoint({ role: 'viewer', serverHost: '127.0.0.1' });
  try {
    await host.open();
    await viewer.open();
    const info = { session: 4242, secret: Buffer.alloc(16, 7).toString('base64'), port: 0 };
    const offer = await host.addPeer('v', info);
    const answer = await viewer.addPeer('host', info);
    // a computer without a network card still has the loopback address
    offer.cands.push({ address: '127.0.0.1', port: host.port });
    answer.cands.push({ address: '127.0.0.1', port: viewer.port });
    viewer.connectPeer('host', offer);
    host.connectPeer('v', answer);
    const frame = Buffer.alloc(50000, 3);
    const got = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('timeout')), 8000);
      viewer.on('media', (meta, data) => {
        clearTimeout(timer);
        resolve(data);
      });
      host.on('state', (id, s) => { if (s.state === 'connected') host.sendMedia({ stream: 0, key: true, ts: 0, capture: 0 }, frame); });
    });
    return { ok: got.equals(frame), bytes: got.length };
  } catch (err) {
    return { ok: false, error: err.message };
  } finally {
    host.close();
    viewer.close();
  }
}

/* ------------------------------------------------ start */

// own profile (e.g. a second copy of the app on the same computer, for testing)
if (arg('user-data-dir')) app.setPath('userData', path.resolve(arg('user-data-dir')));

if (!SMOKE && !app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (!mainWindow) return;
    if (mainWindow.isMinimized()) mainWindow.restore();
    mainWindow.focus();
  });

  app.whenReady().then(() => {
    serverUrl = normalizeUrl(arg('url') || process.env.SCREENSHARE_URL || loadConfig().serverUrl || DEFAULT_URL) || DEFAULT_URL;
    const ses = session.defaultSession;
    setupPermissions(ses);
    setupDisplayMedia(ses);
    setupIpc();
    buildMenu();
    createMainWindow();
    if (SMOKE) runSmokeTest();

    app.on('activate', () => {
      if (!mainWindow) createMainWindow();
    });
  });

  app.on('will-quit', () => {
    stopAudioHelper();
    stopDirect();
  });
  app.on('window-all-closed', () => {
    setSharing(false);
    if (!IS_MAC) app.quit();
  });
}
