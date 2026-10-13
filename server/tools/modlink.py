#!/usr/bin/env python3
"""RDPServer: makes the relocatable module file from the ELF linked at 0.

    modlink.py module.elf RDPServer,ffa

The ELF must be linked with --emit-relocs (ld -q) and the script in the
Makefile (one output section, .text: the header first, then code,
read-only data, .data between __data_start and __data_end, and .bss
between __bss_start and __bss_end, as zeros in the image).

The module can be loaded anywhere in the RMA, so the file gets, after the
image: a copy of .data as linked (for RMReInit) and the list of the
offsets of every word that holds an absolute address (R_ARM_ABS32). The
header's modinfo words say where they are (header.s), and init_veneer
adds the module's address to those words before any C runs.

Any other kind of absolute relocation (MOVW/MOVT, a GOT, ...) is refused:
the code would hold an address the list can't fix. Relative ones (calls,
branches, PC-relative words) need nothing.

Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).
"""
import struct
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

R_ARM_NONE, R_ARM_PC24, R_ARM_ABS32, R_ARM_REL32 = 0, 1, 2, 3
R_ARM_CALL, R_ARM_JUMP24, R_ARM_V4BX, R_ARM_PREL31 = 28, 29, 40, 42
RELATIVE = {R_ARM_NONE, R_ARM_PC24, R_ARM_REL32, R_ARM_CALL, R_ARM_JUMP24,
            R_ARM_V4BX, R_ARM_PREL31}
SHN_ABS = 'SHN_ABS'


def fail(msg):
    sys.exit("modlink: " + msg)


def main():
    if len(sys.argv) != 3:
        fail("usage: modlink.py module.elf out,ffa")
    with open(sys.argv[1], 'rb') as f:
        elf = ELFFile(f)
        text = elf.get_section_by_name('.text')
        if text is None or text['sh_addr'] != 0:
            fail(".text must be at address 0")
        alloc = [s.name for s in elf.iter_sections()
                 if s['sh_flags'] & 2 and s.name != '.text' and s['sh_size'] > 0]
        if alloc:
            fail("sections outside .text: " + ", ".join(alloc))
        image = bytearray(text.data())
        symtab = elf.get_section_by_name('.symtab')
        syms = {}
        for s in symtab.iter_symbols():
            if s.name:
                syms[s.name] = s['st_value']
        for need in ('module_base', 'modinfo', '__data_start', '__data_end',
                     '__bss_start', '__bss_end'):
            if need not in syms:
                fail("no symbol " + need)
        if syms['module_base'] != 0:
            fail("module_base isn't at 0")
        offsets = []
        for sec in elf.iter_sections():
            if not isinstance(sec, RelocationSection):
                continue
            target = elf.get_section(sec['sh_info'])
            if target.name != '.text':
                continue
            if sec.is_RELA():
                fail("RELA relocations: not expected")
            rsym = elf.get_section(sec['sh_link'])
            for r in sec.iter_relocations():
                t = r['r_info_type']
                if t in RELATIVE:
                    continue
                if t != R_ARM_ABS32:
                    sym = rsym.get_symbol(r['r_info_sym'])
                    fail("relocation type %d at &%X (%s): not position independent"
                         % (t, r['r_offset'], sym.name))
                sym = rsym.get_symbol(r['r_info_sym'])
                if sym['st_shndx'] == SHN_ABS:
                    continue          # a constant, not an address
                off = r['r_offset']
                if off % 4:
                    fail("unaligned absolute word at &%X" % off)
                offsets.append(off)
    offsets = sorted(set(offsets))
    ds, de = syms['__data_start'], syms['__data_end']
    bs, be = syms['__bss_start'], syms['__bss_end']
    if be != len(image) and be > len(image):
        fail(".bss isn't in the image")
    if any(bs <= o < be for o in offsets):
        fail("an address in .bss")
    while len(image) % 4:
        image.append(0)
    data_copy = len(image)
    image += image[ds:de]
    reloc_table = len(image)
    for o in offsets:
        image += struct.pack('<I', o)
    mi = syms['modinfo']
    # reloc_base, reloc_table, reloc_count, data_start, data_end, data_copy,
    # bss_start, bss_end
    struct.pack_into('<8I', image, mi, 0, reloc_table, len(offsets), ds, de,
                     data_copy, bs, be)
    with open(sys.argv[2], 'wb') as f:
        f.write(image)
    print("modlink: %s: %d bytes (code and data %d, .bss %d, %d addresses)"
          % (sys.argv[2], len(image), bs, be - bs, len(offsets)))


main()
