#!/usr/bin/env python3
# Copyright (c) Electron contributors.
# Use of this source code is governed by the MIT license that can be
# found in the LICENSE file.

"""Run with python3 build/reorder-electron-imports_test.py.

Builds PE import fixtures directly; no compiler, prebuilt executable, or
dependency beyond the production script's Chromium pefile module is required.
"""

import importlib.util
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name('reorder-electron-imports.py')
SPEC = importlib.util.spec_from_file_location('reorder_imports', SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
PEFILE = MODULE.pefile

ARCHITECTURES = {'x86': (0x14c, False), 'x64': (0x8664, True),
                 'arm64': (0xaa64, True)}
IMPORT_OFFSET = 0x200


def make_pe(architecture, libraries):
  """Create a PE with one section and named plus ordinal imports for each DLL."""
  machine, is_64 = ARCHITECTURES[architecture]
  image = bytearray(0x1200)
  image[:2] = b'MZ'
  struct.pack_into('<I', image, 0x3c, 0x80)
  image[0x80:0x84] = b'PE\0\0'
  optional_size = 0xf0 if is_64 else 0xe0
  struct.pack_into('<HHIIIHH', image, 0x84, machine, 1, 0, 0, 0,
                   optional_size, 0x22 if is_64 else 0x102)
  optional = 0x98
  struct.pack_into('<H', image, optional, 0x20b if is_64 else 0x10b)
  struct.pack_into('<I', image, optional + 8, 0x1000)
  struct.pack_into('<I', image, optional + 20, 0x1000)
  if is_64:
    struct.pack_into('<Q', image, optional + 24, 0x140000000)
  else:
    struct.pack_into('<II', image, optional + 24, 0x1000, 0x400000)
  struct.pack_into('<II', image, optional + 32, 0x1000, 0x200)
  struct.pack_into('<H', image, optional + 40, 6)
  struct.pack_into('<H', image, optional + 48, 6)
  struct.pack_into('<II', image, optional + 56, 0x2000, 0x200)
  struct.pack_into('<H', image, optional + 68, 3)
  struct.pack_into('<QQQQ' if is_64 else '<IIII', image, optional + 72,
                   0x100000, 0x1000, 0x100000, 0x1000)
  directories = optional + (112 if is_64 else 96)
  struct.pack_into('<I', image, directories - 4, 16)
  struct.pack_into('<II', image, directories + 8,
                   0x1000, 20 * (len(libraries) + 1))
  section = optional + optional_size
  struct.pack_into('<8sIIIIIIHHI', image, section, b'.idata\0\0',
                   0x1000, 0x1000, 0x1000, IMPORT_OFFSET, 0, 0, 0, 0,
                   0xc0000040)

  cursor = IMPORT_OFFSET + 20 * (len(libraries) + 1)

  def allocate(data):
    nonlocal cursor
    cursor = (cursor + 7) & ~7
    offset = cursor
    image[offset:offset + len(data)] = data
    cursor += len(data)
    return offset - IMPORT_OFFSET + 0x1000

  for index, library in enumerate(libraries):
    name = allocate(library + b'\0')
    symbol = allocate(struct.pack('<H', index) +
                      f'Function{index}'.encode('ascii') + b'\0')
    ordinal = (1 << (63 if is_64 else 31)) | (index + 1)
    thunks = struct.pack('<QQQ' if is_64 else '<III', symbol, ordinal, 0)
    lookup = allocate(thunks)
    address = allocate(thunks)
    struct.pack_into('<IIIII', image, IMPORT_OFFSET + index * 20,
                     lookup, 0, 0xffffffff, name, address)
  return bytes(image)


def imports(image):
  pe = PEFILE.PE(data=image)
  try:
    return [(entry.dll, entry.struct.OriginalFirstThunk,
             entry.struct.FirstThunk,
             [(symbol.name, symbol.ordinal, symbol.address)
              for symbol in entry.imports])
            for entry in pe.DIRECTORY_ENTRY_IMPORT]
  finally:
    pe.close()


class ReorderImportsTest(unittest.TestCase):
  def run_script(self, directory, image):
    source = directory / 'initial.exe'
    pdb = directory / 'initial.pdb'
    destination = directory / 'electron.exe'
    destination_pdb = directory / 'electron.pdb'
    source.write_bytes(image)
    pdb_bytes = b'PDB copy fixture\0\xff\x80\r\n'
    pdb.write_bytes(pdb_bytes)
    result = subprocess.run([sys.executable, str(SCRIPT), str(source), str(pdb),
                             str(destination), str(destination_pdb)],
                            capture_output=True, text=True, timeout=30)
    self.assertEqual(source.read_bytes(), image, 'source EXE was modified')
    self.assertEqual(pdb.read_bytes(), pdb_bytes, 'source PDB was modified')
    return result, destination, destination_pdb, pdb_bytes

  def test_moves_elf_first_without_changing_thunks(self):
    for architecture in ARCHITECTURES:
      for elf_index in (1, 2):
        with self.subTest(architecture=architecture, elf_index=elf_index):
          libraries = [b'kernel32.dll', b'user32.dll']
          libraries.insert(elf_index, b'ElEcTrOn_ElF.DlL')
          original = make_pe(architecture, libraries)
          before = imports(original)
          self.assertEqual([entry[0] for entry in before], libraries)
          with tempfile.TemporaryDirectory() as temporary:
            result, output, pdb, pdb_bytes = self.run_script(
                pathlib.Path(temporary), original)
            self.assertEqual(result.returncode, 0, result.stderr)
            reordered = output.read_bytes()
            expected = bytearray(original)
            first = IMPORT_OFFSET
            early = IMPORT_OFFSET + elf_index * 20
            expected[first:first + 20], expected[early:early + 20] = (
                expected[early:early + 20], expected[first:first + 20])
            # This also checks that every IAT, lookup table, DLL string, import
            # name/ordinal, PE header, and terminator remains byte-identical.
            self.assertEqual(reordered, bytes(expected))
            after = imports(reordered)
            before[0], before[elf_index] = before[elf_index], before[0]
            self.assertEqual(after, before)
            self.assertEqual(pdb.read_bytes(), pdb_bytes)

  def test_first_import_is_idempotent(self):
    for architecture in ARCHITECTURES:
      with self.subTest(architecture=architecture):
        original = make_pe(architecture, [b'electron_elf.dll', b'kernel32.dll'])
        with tempfile.TemporaryDirectory() as temporary:
          for _ in range(2):
            result, output, pdb, pdb_bytes = self.run_script(
                pathlib.Path(temporary), original)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.read_bytes(), original)
            self.assertEqual(pdb.read_bytes(), pdb_bytes)

  def test_missing_or_duplicate_elf_is_rejected(self):
    for architecture in ARCHITECTURES:
      for libraries in ([], [b'kernel32.dll', b'user32.dll'],
                        [b'electron_elf.dll', b'ELECTRON_ELF.DLL']):
        with self.subTest(architecture=architecture, libraries=libraries):
          with tempfile.TemporaryDirectory() as temporary:
            result, output, pdb, _ = self.run_script(
                pathlib.Path(temporary), make_pe(architecture, libraries))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Expected exactly one electron_elf.dll import',
                          result.stderr)
            self.assertFalse(output.exists())
            self.assertFalse(pdb.exists())


if __name__ == '__main__':
  unittest.main()
