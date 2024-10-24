#!/usr/bin/env python3
# Copyright (c) Electron contributors.
# Use of this source code is governed by the MIT license that can be
# found in the LICENSE file.

"""Make electron_elf.dll the first import, before signing the executable."""

import pathlib
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] /
                       'third_party' / 'pefile_py3'))
import pefile


def main():
  source, source_pdb, destination, destination_pdb = sys.argv[1:]
  pe = pefile.PE(source, fast_load=True)
  try:
    pe.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    imports = getattr(pe, 'DIRECTORY_ENTRY_IMPORT', [])
    elf = [entry for entry in imports
           if entry.dll.lower() == b'electron_elf.dll']
    if len(elf) != 1:
      raise ValueError('Expected exactly one electron_elf.dll import')
    first, early = imports[0].struct, elf[0].struct
    for field in ('OriginalFirstThunk', 'TimeDateStamp', 'ForwarderChain',
                  'Name', 'FirstThunk'):
      original, replacement = getattr(first, field), getattr(early, field)
      setattr(first, field, replacement)
      setattr(early, field, original)
    pe.write(filename=destination)
  finally:
    pe.close()
  shutil.copyfile(source_pdb, destination_pdb)


if __name__ == '__main__':
  main()
