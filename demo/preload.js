const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('api', {
  triggerCrash: (type) => {
    console.log('Preload forwarding crash to main:', type);
    ipcRenderer.send('trigger-crash', type);
  }
});
contextBridge.exposeInMainWorld('electronAPI', {
  crash: (type) => {
    console.log('Preload calling process.crash:', type);
    // 这里在预加载环境中调用你的新接口，确保真正触发
    process.crash(type);
  }
});