'use strict';

// Screen Share desktop app (Windows / macOS).
//
// The UI and the WebRTC code are the website itself (loaded from the server),
// so the app always matches the server. What the app adds on top of a browser:
//  - its own screen/window picker, and on Windows the system audio comes along
//    automatically (no checkbox to forget),
//  - laptops with two GPUs use the strong one (NVIDIA NVENC instead of the
//    integrated chip), H.265 hardware encoding is allowed,
//  - sharing is never throttled when the window is minimised or covered, and the
//    capture/encode processes get a higher priority while sharing (a game
//    running next to it cannot starve them),
//  - the computer does not go to sleep while sharing.

const { app, BrowserWindow, Menu, desktopCapturer, dialog, ipcMain, powerSaveBlocker, screen, session, shell, systemPreferences } = require('electron');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const DEFAULT_URL = 'https://mozizas.hu/';
const IS_MAC = process.platform === 'darwin';
const IS_WIN = process.platform === 'win32';

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
      additionalArguments: [`--ss-version=${app.getVersion()}`],
    },
  });

  mainWindow.once('ready-to-show', () => {
    if (!SMOKE) mainWindow.show();
  });
  mainWindow.on('close', () => saveConfig({ bounds: mainWindow.getNormalBounds() }));
  mainWindow.on('closed', () => {
    mainWindow = null;
  });

  const wc = mainWindow.webContents;
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
    // audio inputs (e.g. BlackHole on a Mac) yes, camera no
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
      return screen ? { id: screen.id, audio: audioRequested && IS_WIN } : null;
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
        additionalArguments: [`--ss-audio=${audioRequested && IS_WIN ? '1' : '0'}`, `--ss-platform=${process.platform}`],
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
      // Windows: the whole system sound ("loopback"); macOS has no loopback – use an audio input (BlackHole)
      if (IS_WIN && choice.audio && request.audioRequested) streams.audio = 'loopback';
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

/* ------------------------------------------------ IPC from the pages */

function setupIpc() {
  ipcMain.on('app:sharing', (e, on) => {
    if (isServerUrl(e.senderFrame?.url)) setSharing(Boolean(on));
  });
  ipcMain.on('app:retry', (e) => {
    if (isLocalPage(e.senderFrame?.url) && mainWindow) loadServer();
  });
  ipcMain.on('app:settings', () => openSettings());
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
    log('SMOKE OK');
    clearTimeout(timer);
    app.exit(0);
  } catch (err) {
    log(`SMOKE FAIL: ${err?.message || err}`);
    clearTimeout(timer);
    app.exit(1);
  }
}

/* ------------------------------------------------ start */

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

  app.on('window-all-closed', () => {
    setSharing(false);
    if (!IS_MAC) app.quit();
  });
}
