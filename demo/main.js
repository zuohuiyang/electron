const { app, BrowserWindow, ipcMain } = require('electron');

function createWindow() {
  const win = new BrowserWindow({
    webPreferences: {
      preload: __dirname + '\\preload.js',
      nodeIntegration: false,
      contextIsolation: true,
      sandbox: true
    }
  });
  win.loadFile(__dirname + '\\index.html');
}

// 监听渲染端触发，主进程执行 process.crash
ipcMain.on('trigger-crash', (_event, type) => {
  console.log('Main received crash request:', type);
  process.crash(type);
});

app.whenReady().then(createWindow);
app.on('window-all-closed', () => app.quit());