// Electron main process of the Cortex fixture.
'use strict';
const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('node:path');
const store = require('./lib/store.cjs');

function createWindow() {
  const win = new BrowserWindow({
    width: 800,
    height: 600,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });
  win.loadFile(path.join(__dirname, 'renderer', 'index.html'));
  win.webContents.on('did-finish-load', () => {
    win.webContents.send('app:ready', { version: app.getVersion() });
  });
  return win;
}

ipcMain.handle('settings:get', async (_event, key) => store.get(key));
ipcMain.handle('settings:set', async (_event, key, value) => store.set(key, value));
ipcMain.on('log:write', (_event, message) => console.log(`[renderer] ${message}`));

app.whenReady().then(async () => {
  createWindow();
  const { describe } = await import('./lib/describe.mjs');
  console.log(describe(app.getName()));
});
