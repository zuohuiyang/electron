const { dllBlocker } = process._linkedBinding('electron_browser_dll_blocker');

Object.assign(dllBlocker, {
  BlockingState: {
    DISABLED: 0,
    ENABLED: 1,
    SETUP_RUNNING: 2,
    SETUP_FAILED: 3
  },
  enable: () => dllBlocker.setState(1),
  disable: () => dllBlocker.setState(0)
});

export default dllBlocker;
