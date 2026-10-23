'use strict';

const { contextBridge, ipcRenderer } = require('electron');

const argValue = (name) => (process.argv.find((a) => a.startsWith(`--${name}=`)) || '').split('=')[1] || '';

contextBridge.exposeInMainWorld('picker', {
  audioAvailable: argValue('ss-audio') === '1',
  platform: argValue('ss-platform'),
  sources: () => ipcRenderer.invoke('picker:sources'),
  choose: (id, audio) => ipcRenderer.send('picker:choose', { id, audio }),
  cancel: () => ipcRenderer.send('picker:choose', null),
});
