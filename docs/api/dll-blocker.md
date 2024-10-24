# dllBlocker

> Block configured DLL filenames in the Windows main process.

Process: [Main](../glossary.md#main-process)

This module is only available on Windows. It installs a DLL loading hook before
the executable's entry point runs. Settings are stored under the current user's
registry and take effect the next time the application starts. The default is
disabled.

Blocking matches the DLL basename or its PE export name, with ASCII case ignored.
It does not remove modules that have already loaded, and it is not a general
security boundary against arbitrary code execution. Renderer and utility
processes do not install this hook.

## Configuration

Before distributing an application, configure its `registryPath` in the
executable's configuration wire. This path is relative to `HKEY_CURRENT_USER`
and must be an application-specific subkey of `Software`, for example
`Software\\ExampleCompany\\ExampleApp`. Missing or invalid configuration disables
the feature; write methods return `false`.

The wire retains the original DLL blocker's binary layout: the ASCII sentinel
`tt-fuses-5344BEC8-BF93-1B87-2DA3-785C278AA87A`, version byte `01`, length bytes
`00 00 30 00`, and a 12,288-byte buffer containing NUL-terminated UTF-8 JSON.
Only the `registryPath` field is used. Patch this buffer before signing the EXE.
This wire is separate from Electron's standard boolean fuses.

For example, a packaging script can configure the executable without an
internal package dependency:

```js
const fs = require('node:fs')

const executable = 'dist/ExampleApp.exe'
const image = fs.readFileSync(executable)
const sentinel = Buffer.from('tt-fuses-5344BEC8-BF93-1B87-2DA3-785C278AA87A')
const offset = image.indexOf(sentinel)
if (offset < 0 || image.indexOf(sentinel, offset + sentinel.length) !== -1) {
  throw new Error('Expected one DLL blocker configuration wire')
}
const header = offset + sentinel.length
if (image[header] !== 1 || !image.subarray(header + 1, header + 5).equals(Buffer.from([0, 0, 0x30, 0]))) {
  throw new Error('Unsupported configuration wire')
}
const start = header + 5
const payload = Buffer.from(JSON.stringify({ registryPath: 'Software\\ExampleCompany\\ExampleApp' }))
if (payload.length >= 12288 || start + 12288 > image.length) {
  throw new Error('Invalid configuration size')
}
image.fill(0, start, start + 12288)
payload.copy(image, start)
fs.writeFileSync(executable, image)
```

The registry path must be available before Electron or application JavaScript
initializes. Changing `app.name` or `app.setPath()` does not change this path.
Applications that share a registry path also share their saved blocking policy.

The list retains the original registry format: a JSON array stored in
`ThirdParty\\TTBlocklist` as `REG_SZ`. Existing saved lists remain readable.

## Methods

### `dllBlocker.enable()` _Windows_

Returns `boolean` - Whether the enabled state was saved successfully.

Enables blocking for the next application launch.

### `dllBlocker.disable()` _Windows_

Returns `boolean` - Whether the disabled state was saved successfully.

Disables blocking for the next application launch. It does not remove a hook
that is already installed in the current process.

### `dllBlocker.setState(state)` _Windows_

* `state` Integer - `BlockingState.DISABLED` or `BlockingState.ENABLED`.

Returns `boolean` - Whether the state was saved. Invalid states return `false`.

### `dllBlocker.getState()` _Windows_

Returns `Integer` - The saved blocking state, or `BlockingState.DISABLED` if it
cannot be read. This reports the persisted policy, not whether a hook is currently
active in this process.

### `dllBlocker.setBlocklist(blocklist)` _Windows_

* `blocklist` string[] - DLL filenames, without directory components.

Returns `boolean` - Whether the list was saved. Empty filenames, paths, embedded
NUL characters, and invalid UTF-8 are rejected.

Replaces the list for the next application launch. An empty list allows all DLLs
through this hook. Updating the list does not enable the blocker automatically.

### `dllBlocker.getCurrentBlocklist()` _Windows_

Returns `string[]` - The normalized list loaded when this process initialized its
blocker. Returns an empty array if the blocker did not initialize.

## Properties

### `dllBlocker.BlockingState` _Windows_

An `Object` containing the following properties:

* `DISABLED` Integer - `0`: blocking is disabled.
* `ENABLED` Integer - `1`: blocking is enabled for the next launch.
* `SETUP_RUNNING` Integer - `2`: early initialization has not yet been acknowledged.
* `SETUP_FAILED` Integer - `3`: repeated startup failures disabled initialization.

The early loader maintains the two setup states to avoid repeated startup
failures. Applications can only set `DISABLED` or `ENABLED`.

## Events

### Event: 'dll-blocked'

Returns:

* `event` Event
* `dllName` string - The blocked DLL name.

Emitted on the main thread when a configured DLL is blocked. Events collected
before `app` is ready are queued and delivered after the `ready` event. Register
listeners before or during `ready` to receive those events.
