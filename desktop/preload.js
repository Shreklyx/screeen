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
});
