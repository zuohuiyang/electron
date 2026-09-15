# Windows Runtime Distribution

On Windows, Electron loads its main runtime from `main.dll` next to the application
executable. Include both files from the same Electron distribution when packaging
or updating an application.

## Package and update the runtime

Use the complete Electron distribution as the starting point for a Windows package.
The runtime DLL is included in the Windows x64 and ARM64 distribution manifests.
Copying only `electron.exe` from a newer distribution is insufficient.

* Keep `main.dll` in the same directory as the executable. The loader resolves this
  directory from the executable location, independently of the working directory.
* Preserve the `main.dll` filename when renaming `electron.exe` to your application
  name. Export forwarders in the executable also refer to `main.dll`.
* Update file allowlists, installer payloads, integrity manifests and incremental
  update rules to include the DLL. Install the executable and DLL as a matching set;
  avoid launching an application while only one of these files has been replaced.
* Include the DLL in your application's code-signing and signature-verification
  workflow where that workflow covers shipped executable code.
* Retain the existing resource files, locale files and other DLLs. Moving the main
  runtime does not make the executable and `main.dll` a standalone distribution.

Custom packaging and update pipelines that enumerate Electron files must adapt to
this layout. Pipelines that copy the entire distribution should still verify their
final installed output. This is a distribution change even though JavaScript API
names are unchanged.

## Native modules and fuses

Continue to build native modules for the target Electron version and architecture.
The executable forwards the runtime's named exports to `main.dll`; applications
should not change their native modules to link directly against that DLL.

Keep applying Electron fuses to the application executable. The executable passes
its fuse configuration to the runtime. Apply fuse changes before the final signing
step, as with other changes to signed executable content.

The runtime stays loaded through process shutdown so that native module destructors
can continue to resolve runtime functions. The Windows xcache tool reads the Node
startup snapshot from `main.dll`.

## Validate a custom distribution

Test the final installed or updated package, including a renamed executable and the
native modules used by the application. Verify that its fuse settings still take
effect. Do not infer compatibility with every native module or packaging tool from
the presence of export forwarders alone.

The loader reports a Windows error if it cannot load the DLL or find its entry
point. A missing `main.dll` produces `ERROR_MOD_NOT_FOUND` (126), an invalid DLL
produces `ERROR_BAD_EXE_FORMAT` (193), and a valid DLL without `ElectronMain` produces
`ERROR_PROC_NOT_FOUND` (127). Check the deployed files and architecture when
investigating these errors.

If collecting symbols or diagnosing native crashes, retain symbols for both the
executable and `main.dll`. The main runtime's code now belongs to the DLL module.
