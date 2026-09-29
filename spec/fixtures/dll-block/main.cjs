const { app, BrowserWindow, dllBlocker, utilityProcess } = require('electron/main');
const { spawnSync } = require('node:child_process');
const { once } = require('node:events');
const net = require('node:net');
const path = require('node:path');
const readline = require('node:readline');

app.setPath('userData', path.join(path.dirname(process.execPath), 'user-data'));

const reply = (message) => process.stdout.write(`@@DLLBLOCKER@@${JSON.stringify(message)}\n`);
dllBlocker.on('dll-blocked', (_event, name) => reply({ blocked: name }));

async function command(message) {
  switch (message.action) {
    case 'configure':
      return {
        listSaved: dllBlocker.setBlocklist(message.list),
        stateSaved: message.enabled ? dllBlocker.enable() : dllBlocker.disable(),
        state: dllBlocker.getState(),
        list: dllBlocker.getCurrentBlocklist()
      };
    case 'state':
      return {
        state: dllBlocker.getState(),
        list: dllBlocker.getCurrentBlocklist(),
        constants: dllBlocker.BlockingState
      };
    case 'set-state':
      return dllBlocker.setState(message.state);
    case 'load': {
      const helper = path.join(path.dirname(process.execPath), 'dll_blocker_test_helper.exe');
      const result = spawnSync(helper, ['--inject', String(process.pid), message.dll], {
        encoding: 'utf8',
        timeout: 30000,
        windowsHide: true
      });
      if (result.error) throw result.error;
      if (result.status !== 0) throw new Error(`Injection helper failed: ${result.stderr}`);
      return JSON.parse(result.stdout);
    }
    case 'children': {
      const window = new BrowserWindow({ show: false, webPreferences: { sandbox: true } });
      try {
        await window.loadURL('data:text/html,<title>dll blocker renderer</title>');
        const title = await window.webContents.executeJavaScript('document.title');
        const child = utilityProcess.fork(path.join(__dirname, 'utility.cjs'), [], { stdio: 'pipe' });
        const exited = once(child, 'exit');
        try {
          const [message] = await once(child, 'message');
          child.postMessage('quit');
          const [code] = await exited;
          return { title, utility: message, code };
        } finally {
          child.kill();
        }
      } finally {
        window.destroy();
      }
    }
    case 'quit':
      app.exit(0);
      return;
    default:
      throw new Error(`Unknown command: ${message.action}`);
  }
}

app.whenReady().then(() => {
  const server = net.createServer((socket) => {
    server.close();
    const input = readline.createInterface({ input: socket });
    input.on('line', async (line) => {
      const message = JSON.parse(line);
      try {
        reply({ id: message.id, result: await command(message) });
      } catch (error) {
        reply({ id: message.id, error: error.stack });
      }
    });
    input.on('close', () => app.exit(0));
  });
  server.listen(process.argv[2], () => reply({ ready: true }));
});
