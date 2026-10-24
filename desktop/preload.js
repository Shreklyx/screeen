'use strict';

// Bridge for the website and the app's own pages: only these few calls are exposed.
const { contextBridge, ipcRenderer } = require('electron');

const version = (process.argv.find((a) => a.startsWith('--ss-version=')) || '').split('=')[1] || '';

contextBridge.exposeInMainWorld('screenshareApp', {
  platform: process.platform,
  version,
  // the server for signaling (the pages are built into the app)
  serverUrl: ipcRenderer.sendSync('app:server-url') || '',
  // in-app updates (installed app on Windows / macOS)
  updates: process.argv.includes('--ss-updates')
    ? {
      getState: () => ipcRenderer.invoke('update:state'),
      check: () => ipcRenderer.send('update:check'),
      install: () => ipcRenderer.send('update:install'),
      onState: (cb) => {
        ipcRenderer.removeAllListeners('update:state');
        ipcRenderer.on('update:state', (_e, s) => cb(s));
      },
    }
    : null,
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
  // direct mode (app ↔ app without WebRTC): the app's own UDP transport, see direct.js
  direct: {
    add: (role, id, info) => ipcRenderer.invoke('direct:add', String(role), String(id), info),
    connect: (id, answer) => ipcRenderer.send('direct:connect', String(id), answer),
    remove: (id) => ipcRenderer.send('direct:remove', String(id)),
    send: (meta, data) => ipcRenderer.send('direct:send', meta, data),
    keyframe: () => ipcRenderer.send('direct:keyframe'),
    bitrate: (bps) => ipcRenderer.send('direct:bitrate', Number(bps)),
    stop: () => ipcRenderer.send('direct:stop'),
    onEvent: (cb) => {
      ipcRenderer.removeAllListeners('direct:event');
      ipcRenderer.on('direct:event', (_e, name, ...args) => cb(name, ...args));
    },
    onMedia: (cb) => {
      ipcRenderer.removeAllListeners('direct:media');
      ipcRenderer.on('direct:media', (_e, meta, data) => cb(meta, data));
    },
  },
  // direct mode's picture from native code (Windows + NVIDIA: DXGI capture, NVENC), sent by the app itself
  nativeCapture: process.argv.includes('--ss-native-capture')
    ? {
      start: (opts) => ipcRenderer.invoke('native:start', opts),
      stop: (id) => ipcRenderer.send('native:stop', Number(id)),
      onEvent: (cb) => {
        ipcRenderer.removeAllListeners('native:event');
        ipcRenderer.on('native:event', (_e, name, id, info) => cb(name, id, info));
      },
    }
    : null,
});
