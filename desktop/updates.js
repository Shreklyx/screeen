'use strict';

// In-app updates. The installers and their update files (latest.yml, latest-mac.yml) are on the
// "desktop-latest" GitHub release, rebuilt by CI on every change.
//  - Windows: a new version downloads in the background; "Újraindítás és frissítés" (or quitting
//    the app) installs it.
//  - macOS: the app is not signed by Apple, so it cannot replace itself – a new version is
//    offered for download.

const { app, net, shell } = require('electron');

const FEED = 'https://github.com/Shreklyx/screeen/releases/download/desktop-latest';
const MAC_DOWNLOAD = `${FEED}/ScreenShare-mac.dmg`;
const CHECK_EVERY_MS = 3 * 3600 * 1000;

/** a > b for dotted version numbers */
function newer(a, b) {
  const pa = String(a).split('.').map((x) => parseInt(x, 10) || 0);
  const pb = String(b).split('.').map((x) => parseInt(x, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) > (pb[i] || 0);
  }
  return false;
}

/** onState({ status: idle|checking|latest|downloading|ready|available|error, version, progress, error }) */
function createUpdater({ onState }) {
  const state = { status: 'idle', version: null, progress: null, error: null, current: app.getVersion() };
  const emit = (patch) => {
    Object.assign(state, patch);
    onState({ ...state });
  };
  let check = async () => {};
  let install = () => {};

  if (process.platform === 'win32') {
    const { autoUpdater } = require('electron-updater');
    autoUpdater.autoDownload = true;
    autoUpdater.autoInstallOnAppQuit = true;
    autoUpdater.disableDifferentialDownload = true; // the installer's name never changes, no old blockmap to diff against
    autoUpdater.logger = null;
    autoUpdater.on('checking-for-update', () => {
      if (!['downloading', 'ready'].includes(state.status)) emit({ status: 'checking', error: null });
    });
    autoUpdater.on('update-not-available', () => {
      if (state.status === 'checking') emit({ status: 'latest' });
    });
    autoUpdater.on('update-available', (info) => emit({ status: 'downloading', version: info.version, progress: 0 }));
    autoUpdater.on('download-progress', (p) => emit({ status: 'downloading', progress: Math.round(p.percent) }));
    autoUpdater.on('update-downloaded', (info) => emit({ status: 'ready', version: info.version, progress: 100 }));
    autoUpdater.on('error', (err) => {
      if (state.status !== 'ready') emit({ status: 'error', error: String(err?.message || err).split('\n')[0].slice(0, 200) });
    });
    check = () => autoUpdater.checkForUpdates().catch(() => {});
    install = () => autoUpdater.quitAndInstall(true, true); // silent, and the app starts again
  } else if (process.platform === 'darwin') {
    check = async () => {
      if (state.status !== 'available') emit({ status: 'checking', error: null });
      try {
        const res = await net.fetch(`${FEED}/latest-mac.yml`, { cache: 'no-store' });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const version = /^version:\s*['"]?([\d.]+)/m.exec(await res.text())?.[1];
        if (version && newer(version, app.getVersion())) emit({ status: 'available', version });
        else if (state.status !== 'available') emit({ status: 'latest' });
      } catch (err) {
        if (state.status !== 'available') emit({ status: 'error', error: err.message });
      }
    };
    install = () => shell.openExternal(MAC_DOWNLOAD);
  }

  return {
    state,
    check: () => check(),
    install: () => install(),
    start() {
      setTimeout(() => check(), 5000);
      setInterval(() => check(), CHECK_EVERY_MS).unref?.();
    },
  };
}

module.exports = { createUpdater, newer, FEED };
