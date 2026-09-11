import { flipFuses, FuseV1Options, FuseVersion } from '@electron/fuses';

import { expect } from 'chai';

import { spawnSync } from 'node:child_process';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';

import { copyApp } from './lib/fs-helpers';
import { ifdescribe } from './lib/spec-helpers';

ifdescribe(process.platform === 'win32')('Windows runtime DLL', function () {
  this.timeout(120000);

  let tempDir: string;
  let executable: string;

  before(async () => {
    tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'electron-runtime-'));
    const originalExecutable = await copyApp(tempDir);
    executable = path.join(tempDir, 'renamed electron.exe');
    fs.renameSync(originalExecutable, executable);
  });

  after(() => {
    if (tempDir) fs.rmSync(tempDir, { recursive: true, force: true, maxRetries: 5 });
  });

  it('resolves native addon exports after renaming the executable', () => {
    const addon = require.resolve('@electron-ci/echo');
    const script = `const echo = require(${JSON.stringify(addon)});
      if (echo('sync') !== 'sync') process.exit(1);
      echo.async('async', value => console.log(value));`;
    const result = spawnSync(executable, ['-e', script], {
      env: { ...process.env, ELECTRON_RUN_AS_NODE: '1', NODE_OPTIONS: '' },
      cwd: os.tmpdir(),
      encoding: 'utf8',
      timeout: 30000
    });
    expect(result.error).to.equal(undefined);
    expect(result.status, result.stderr).to.equal(0);
    expect(result.stdout.trim()).to.equal('async');
  });

  it('reads the fuse patched in the executable from the runtime DLL', async () => {
    const fuseExecutable = path.join(tempDir, 'fuses.exe');
    fs.copyFileSync(executable, fuseExecutable);
    const options = {
      env: { ...process.env, ELECTRON_RUN_AS_NODE: '1', NODE_OPTIONS: '-e 0' },
      encoding: 'utf8' as const,
      timeout: 30000
    };
    const before = spawnSync(fuseExecutable, ['-e', 'console.log("ok")'], options);
    expect(before.error).to.equal(undefined);
    expect(before.status, before.stderr).to.equal(9);
    await flipFuses(fuseExecutable, {
      version: FuseVersion.V1,
      [FuseV1Options.EnableNodeOptionsEnvironmentVariable]: false
    });
    const after = spawnSync(fuseExecutable, ['-e', 'console.log("ok")'], options);
    expect(after.error).to.equal(undefined);
    expect(after.status, after.stderr).to.equal(0);
    expect(after.stdout.trim()).to.equal('ok');
  });

  it('reports a missing runtime DLL', () => {
    const directory = path.join(tempDir, 'missing-runtime');
    fs.mkdirSync(directory);
    const missingExecutable = path.join(directory, 'electron.exe');
    fs.copyFileSync(executable, missingExecutable);
    const result = spawnSync(missingExecutable, ['-v'], { encoding: 'utf8', timeout: 30000 });
    expect(result.error).to.equal(undefined);
    expect(result.status).to.equal(126); // ERROR_MOD_NOT_FOUND
    expect(result.stderr).to.include('Unable to load Electron runtime');
  });

  it('reports an invalid runtime DLL', () => {
    const directory = path.join(tempDir, 'invalid-runtime');
    fs.mkdirSync(directory);
    const invalidExecutable = path.join(directory, 'electron.exe');
    fs.copyFileSync(executable, invalidExecutable);
    fs.writeFileSync(path.join(directory, 'main.dll'), 'invalid PE image');
    const result = spawnSync(invalidExecutable, ['-v'], { encoding: 'utf8', timeout: 30000 });
    expect(result.error).to.equal(undefined);
    expect(result.status).to.equal(193); // ERROR_BAD_EXE_FORMAT
    expect(result.stderr).to.include('Unable to load Electron runtime');
  });
});
