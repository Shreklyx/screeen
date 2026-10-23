'use strict';

// Bridge for the website and the app's own pages: only these few calls are exposed.
const { contextBridge, ipcRenderer } = require('electron');

const version = (process.argv.find((a) => a.startsWith('--ss-version=')) || '').split('=')[1] || '';

contextBridge.exposeInMainWorld('screenshareApp', {
  platform: process.platform,
  version,
  setSharing: (on) => ipcRenderer.send('app:sharing', Boolean(on)),
  retry: () => ipcRenderer.send('app:retry'),
  openSettings: () => ipcRenderer.send('app:settings'),
  getServerUrl: () => ipcRenderer.invoke('app:get-url'),
  setServerUrl: (url) => ipcRenderer.invoke('app:set-url', String(url)),
  // 5.1 / 7.1 system audio (Windows): float32 samples from the native helper
  systemAudio: process.argv.includes('--ss-surround')
    ? {
      start: () => ipcRenderer.invoke('audio:start'),
      stop: (id) => ipcRenderer.send('audio:stop', Number(id)),
      onFormat: (cb) => {
        ipcRenderer.removeAllListeners('audio:format');
        ipcRenderer.on('audio:format', (_e, format) => cb(format));
      },
      onData: (cb) => {
        ipcRenderer.removeAllListeners('audio:data');
        ipcRenderer.on('audio:data', (_e, bytes) => cb(bytes.buffer.byteLength === bytes.byteLength ? bytes.buffer : bytes.slice().buffer));
      },
    }
    : null,
});
