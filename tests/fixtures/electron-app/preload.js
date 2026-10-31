// Bridge between the sandboxed renderer and the main process.
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('cortexDemo', {
  getSetting: (key) => ipcRenderer.invoke('settings:get', key),
  setSetting: (key, value) => ipcRenderer.invoke('settings:set', key, value),
  log: (message) => ipcRenderer.send('log:write', message),
  onReady: (callback) => ipcRenderer.on('app:ready', (_event, info) => callback(info)),
});
