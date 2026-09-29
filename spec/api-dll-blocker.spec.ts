import { expect } from 'chai';

import * as cp from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { once } from 'node:events';
import * as fs from 'node:fs';
import * as net from 'node:net';
import * as os from 'node:os';
import * as path from 'node:path';

import { ifdescribe } from './lib/spec-helpers.ts';

type Message = { id?: number; result?: any; error?: string; ready?: boolean; blocked?: string; earlyLoaded?: boolean };

// These are standalone native executables and DLLs, not Node.js addons, so
// ELECTRON_SKIP_NATIVE_MODULE_TESTS does not apply to this suite.
ifdescribe(process.platform === 'win32')('dllBlocker', function () {
  // A persistence test starts up to three Electron processes; the native
  // helper alone has a 30-second deadline, equal to the default spec timeout.
  this.timeout(120000);
  const fixture = path.join(import.meta.dirname, 'fixtures', 'dll-block', 'main.cjs');
  const libraryName = 'dll_blocker_test_library.dll';
  const sentinel = Buffer.from('tt-fuses-5344BEC8-BF93-1B87-2DA3-785C278AA87A');
  let directory: string;
  let executable: string;
  let registryPath: string;
  let wireOffset: number;
  let nextId = 0;
  const children = new Map<cp.ChildProcess, Promise<number | null>>();

  function configureWire(json: string) {
    const payload = Buffer.alloc(0x3000);
    expect(Buffer.byteLength(json)).to.be.lessThan(payload.length);
    payload.write(json);
    const fd = fs.openSync(executable, 'r+');
    try {
      fs.writeSync(fd, payload, 0, payload.length, wireOffset);
    } finally {
      fs.closeSync(fd);
    }
  }

  before(() => {
    directory = fs.mkdtempSync(path.join(os.tmpdir(), 'electron-dll-blocker-'));
    const source = path.dirname(process.execPath);
    // Build output directories also contain object files and symbols. Copy only
    // runtime files, as well as the helper, into a disposable distribution.
    for (const entry of fs.readdirSync(source, { withFileTypes: true })) {
      if (
        (entry.isFile() &&
          (/\.(dll|pak|bin|dat)$/.test(entry.name) ||
            [path.basename(process.execPath), 'dll_blocker_test_helper.exe'].includes(entry.name))) ||
        (entry.isDirectory() && ['locales', 'resources'].includes(entry.name))
      ) {
        fs.cpSync(path.join(source, entry.name), path.join(directory, entry.name), { recursive: true });
      }
    }
    executable = path.join(directory, path.basename(process.execPath));
    for (const name of ['dll_blocker_test_helper.exe', libraryName, 'electron_elf.dll']) {
      expect(fs.existsSync(path.join(directory, name)), `Build fixture or runtime is missing: ${name}`).to.equal(true);
    }
    const image = fs.readFileSync(executable);
    const offset = image.indexOf(sentinel);
    expect(offset, 'DLL blocker configuration wire').to.be.at.least(0);
    expect(image.indexOf(sentinel, offset + sentinel.length), 'configuration wire must be unique').to.equal(-1);
    expect(image[offset + sentinel.length]).to.equal(1);
    expect(image.subarray(offset + sentinel.length + 1, offset + sentinel.length + 5)).to.deep.equal(
      Buffer.from([0, 0, 0x30, 0])
    );
    wireOffset = offset + sentinel.length + 5;
  });

  beforeEach(() => {
    expect(children.size, 'previous fixture processes must be fully closed').to.equal(0);
    registryPath = `Software\\Electron\\DllBlockerTests\\${randomUUID()}`;
    configureWire(JSON.stringify({ registryPath }));
  });

  afterEach(async () => {
    for (const [child, exited] of children) {
      if (child.pid && child.exitCode === null && child.signalCode === null) {
        // The early helper owns a kill-on-close job; regular children may also
        // have renderer processes, which taskkill terminates together.
        cp.spawnSync('taskkill.exe', ['/pid', String(child.pid), '/t', '/f'], { windowsHide: true, timeout: 10000 });
      }
      // Wait for process and stdio closure before the next test rewrites the
      // copied EXE. taskkill returning alone is not the ChildProcess lifecycle.
      let timer: NodeJS.Timeout | undefined;
      try {
        await Promise.race([
          exited,
          new Promise((_resolve, reject) => {
            timer = setTimeout(() => reject(new Error(`Fixture did not exit: ${child.pid}`)), 10000);
          })
        ]);
      } finally {
        clearTimeout(timer);
      }
    }
    children.clear();
    if (registryPath) {
      const result = cp.spawnSync('reg.exe', ['delete', `HKCU\\${registryPath}`, '/f'], { windowsHide: true });
      if (result.status !== 0) {
        const query = cp.spawnSync('reg.exe', ['query', `HKCU\\${registryPath}`], { windowsHide: true });
        expect(query.status, 'temporary registry key must be removed').to.equal(1);
      }
    }
  });

  after(() => {
    if (directory) fs.rmSync(directory, { recursive: true, force: true, maxRetries: 5 });
  });

  async function start(early = false) {
    const pipe = `\\\\.\\pipe\\electron-dll-blocker-${randomUUID()}`;
    const binary = early ? path.join(directory, 'dll_blocker_test_helper.exe') : executable;
    const args = early ? ['--early', executable, fixture, path.join(directory, libraryName), pipe] : [fixture, pipe];
    const env = { ...process.env };
    delete env.ELECTRON_RUN_AS_NODE;
    const child = cp.spawn(binary, args, { env, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    let failure: Error | undefined;
    const messages: Message[] = [];
    const waiters = new Set<() => void>();
    child.stderr.on('data', (chunk) => {
      stderr += chunk;
    });
    child.stdout.on('data', (chunk) => {
      stdout += chunk;
      let end: number;
      while ((end = stdout.indexOf('\n')) !== -1) {
        const line = stdout.slice(0, end);
        stdout = stdout.slice(end + 1);
        if (line.startsWith('@@DLLBLOCKER@@')) messages.push(JSON.parse(line.slice(14)));
      }
      for (const notify of waiters) notify();
    });
    const exited = new Promise<number | null>((resolve) => {
      child.on('error', (error) => {
        failure = error;
        for (const notify of waiters) notify();
      });
      child.on('close', (code) => {
        failure = new Error(`Fixture exited (${code}): ${stderr}`);
        for (const notify of waiters) notify();
        resolve(code);
      });
    });
    children.set(child, exited);
    function waitFor(predicate: (message: Message) => boolean): Promise<Message> {
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          waiters.delete(check);
          reject(new Error(`Timed out waiting for fixture: ${stderr}`));
        }, 20000);
        function check() {
          const match = messages.find(predicate);
          if (!match && !failure) return;
          clearTimeout(timer);
          waiters.delete(check);
          if (match) resolve(match);
          else reject(failure);
        }
        waiters.add(check);
        check();
      });
    }
    async function request(action: string, fields = {}) {
      const id = ++nextId;
      socket.write(`${JSON.stringify({ id, action, ...fields })}\n`);
      const response = await waitFor((message) => message.id === id);
      if (response.error) throw new Error(response.error);
      return response.result;
    }
    await waitFor((message) => !!message.ready);
    const socket = net.createConnection(pipe);
    child.once('close', () => socket.destroy());
    socket.on('error', (error) => {
      failure = error;
      for (const notify of waiters) notify();
    });
    await once(socket, 'connect');
    return {
      request,
      waitFor,
      async close() {
        socket.end();
        expect(await exited, stderr).to.equal(0);
        children.delete(child);
      }
    };
  }

  async function persist(enabled: boolean, list: string[]) {
    const child = await start();
    const result = await child.request('configure', { enabled, list });
    expect(result.listSaved).to.equal(true);
    expect(result.stateSaved).to.equal(true);
    expect(result.state).to.equal(enabled ? 1 : 0);
    await child.close();
  }

  for (const early of [false, true]) {
    for (const scenario of [
      { title: 'disabled', enabled: false, list: [libraryName], loaded: true },
      { title: 'enabled with an empty list', enabled: true, list: [], loaded: true },
      { title: 'enabled with an unrelated DLL', enabled: true, list: ['unrelated.dll'], loaded: true },
      { title: 'enabled with a matching DLL', enabled: true, list: [libraryName.toUpperCase()], loaded: false }
    ]) {
      it(`${scenario.title}: ${early ? 'before the EXE entry point' : 'after ready'}`, async () => {
        await persist(scenario.enabled, scenario.list);
        const child = await start(early);
        const loaded = early
          ? (await child.waitFor((message) => message.earlyLoaded !== undefined)).earlyLoaded
          : await child.request('load', { dll: path.join(directory, libraryName) });
        expect(loaded).to.equal(scenario.loaded);
        if (!scenario.loaded) {
          const message = await child.waitFor((message) => message.blocked?.toLowerCase() === libraryName);
          expect(message.blocked?.toLowerCase()).to.equal(libraryName);
        }
        await child.close();
      });
    }
  }

  it('applies a changed list on the next launch', async () => {
    await persist(true, []);
    let child = await start();
    const changed = await child.request('configure', { enabled: true, list: [libraryName] });
    expect(changed.listSaved).to.equal(true);
    expect(changed.list).to.deep.equal([]);
    expect(await child.request('load', { dll: path.join(directory, libraryName) })).to.equal(true);
    await child.close();
    child = await start();
    expect((await child.request('state')).list).to.deep.equal([libraryName]);
    expect(await child.request('load', { dll: path.join(directory, libraryName) })).to.equal(false);
    await child.close();
  });

  it('loads a blocklist saved in the original registry format', async () => {
    await persist(true, []);
    const saved = cp.spawnSync(
      'reg.exe',
      [
        'add',
        `HKCU\\${registryPath}\\ThirdParty`,
        '/v',
        'TTBlocklist',
        '/t',
        'REG_SZ',
        '/d',
        JSON.stringify([libraryName]),
        '/f'
      ],
      { encoding: 'utf8', windowsHide: true, timeout: 10000 }
    );
    expect(saved.error).to.equal(undefined);
    expect(saved.status, saved.stderr).to.equal(0);
    const child = await start();
    expect((await child.request('state')).list).to.deep.equal([libraryName]);
    expect(await child.request('load', { dll: path.join(directory, libraryName) })).to.equal(false);
    await child.waitFor((message) => message.blocked?.toLowerCase() === libraryName);
    await child.close();
  });

  it('keeps the current protection active after disabling it until the next launch', async () => {
    await persist(true, [libraryName]);
    let child = await start();
    expect(await child.request('set-state', { state: 0 })).to.equal(true);
    expect((await child.request('state')).state).to.equal(0);
    expect(await child.request('load', { dll: path.join(directory, libraryName) })).to.equal(false);
    await child.close();
    child = await start();
    expect(await child.request('load', { dll: path.join(directory, libraryName) })).to.equal(true);
    await child.close();
  });

  for (const json of ['', '{', '{}', '{"registryPath":42}']) {
    it(`starts safely without a valid registry configuration: ${JSON.stringify(json)}`, async () => {
      configureWire(json);
      const child = await start();
      const result = await child.request('configure', { enabled: true, list: [libraryName] });
      expect(result).to.deep.equal({ listSaved: false, stateSaved: false, state: 0, list: [] });
      expect(await child.request('load', { dll: path.join(directory, libraryName) })).to.equal(true);
      await child.close();
    });
  }

  it('rejects invalid blocking states without changing the saved state', async () => {
    await persist(true, []);
    const child = await start();
    for (const state of [-1, 2, 3, 4, 100]) expect(await child.request('set-state', { state })).to.equal(false);
    const state = await child.request('state');
    expect(state.state).to.equal(1);
    expect(state.constants).to.deep.equal({ DISABLED: 0, ENABLED: 1, SETUP_RUNNING: 2, SETUP_FAILED: 3 });
    await child.close();
  });

  it('starts renderer and utility processes while protection is enabled', async () => {
    await persist(true, [libraryName]);
    const child = await start();
    expect(await child.request('children')).to.deep.equal({
      title: 'dll blocker renderer',
      utility: 'utility started',
      code: 0
    });
    await child.close();
  });

  it('preserves ELECTRON_RUN_AS_NODE startup', async () => {
    await persist(true, [libraryName]);
    const result = cp.spawnSync(executable, ['-e', 'console.log("node started")'], {
      env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' },
      encoding: 'utf8',
      timeout: 30000,
      windowsHide: true
    });
    expect(result.error).to.equal(undefined);
    expect(result.status, result.stderr).to.equal(0);
    expect(result.stdout.trim()).to.equal('node started');
  });
});
