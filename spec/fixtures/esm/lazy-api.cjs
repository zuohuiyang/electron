const assert = require('node:assert/strict');
const electron = require('electron/main');

const linkedBinding = process._linkedBinding;
let nativeThemeLoads = 0;
process._linkedBinding = function (name) {
  if (name === 'electron_browser_native_theme') nativeThemeLoads++;
  return linkedBinding.call(this, name);
};

(async () => {
  try {
    assert.equal(electron.app.isReady(), false);
    // The API module must first load here, rather than in the default launcher.
    const nativeTheme = electron.nativeTheme;
    assert.equal(nativeThemeLoads, 1);
    assert.equal(electron.nativeTheme, nativeTheme);
    assert.equal(nativeThemeLoads, 1);
    assert.equal(typeof nativeTheme.shouldUseDarkColors, 'boolean');
    const imported = await import('electron/main');
    assert.equal(imported.nativeTheme, nativeTheme);
    assert.equal(imported.app, electron.app);
    assert.equal(nativeThemeLoads, 1);
    console.log('Lazy API first access and import / require parity');
    process.exit(0);
  } catch (error) {
    console.error(error);
    process.exit(1);
  } finally {
    process._linkedBinding = linkedBinding;
  }
})();
