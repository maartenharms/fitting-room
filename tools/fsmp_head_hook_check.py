# -*- coding: utf-8 -*-
# Verify one FSMP 4.x hdtsmp64.dll against the kSkinAllEntry contract an
# FsmpBridge row asserts, from the build's OWN shipped PDB. Used 2026-09-03 on
# all four 4.0.1 CPU builds (docs/re/fsmp-cooperative-hair.md). Run it again
# before rowing 3.5.0 or any later line.
#
#   python Tools/re/pdb_syms.py <dir>/hdtsmp64.dll --enum "*" > <dir>/syms.txt
#   python tools/fsmp_head_hook_check.py <dir> <dir>/syms.txt "4.0.1 avx2"
#
import sys, re, os
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP

d, syms_path, label = sys.argv[1], sys.argv[2], sys.argv[3]
dll = os.path.join(d, "hdtsmp64.dll")
pe = pefile.PE(dll)
base = pe.OPTIONAL_HEADER.ImageBase

funcs, data_syms = [], {}
rx = re.compile(r"RVA 0x([0-9a-f]+)\s+size\s+(\d+)\s+tag\s+(\d+)\s+(.*)$")
for line in open(syms_path, encoding="utf-8", errors="ignore"):
    m = rx.search(line.strip())
    if not m:
        continue
    # ⚠ pdb_syms writes each name NUL terminated, and str.strip() does not
    # count NUL as whitespace, so a name-keyed lookup silently misses without
    # this. Cost one confused run.
    rva, size, tag, name = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), m.group(4).strip().strip("\x00").strip()
    if tag == 5 and size > 0:
        funcs.append((rva, size, name))
    elif tag == 7:
        data_syms[name] = rva
funcs.sort()

WANT = ["Hooks::BSFaceGenNiNodeHooks::_SkinAllGeometry_Orig",
        "Hooks::BSFaceGenNiNodeHooks::_SkinSingleGeometry"]
slots = {data_syms[n]: n.rsplit("::", 1)[-1] for n in WANT if n in data_syms}
print("=== %s ===" % label)
print("  TimeDateStamp 0x%08X  SizeOfImage 0x%06X  functions %d" %
      (pe.FILE_HEADER.TimeDateStamp, pe.OPTIONAL_HEADER.SizeOfImage, len(funcs)))
if len(slots) != 2:
    print("  !! one of the stored-original slots is MISSING from this PDB: %s" % slots)
    sys.exit(2)
for rva, n in slots.items():
    print("  slot %-24s rva 0x%06X" % (n, rva))

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
hits = []
for rva, size, name in funcs:
    try:
        code = pe.get_data(rva, size)
    except Exception:
        continue
    for insn in md.disasm(code, base + rva):
        for op in insn.operands:
            if op.type != X86_OP_MEM or op.mem.base != X86_REG_RIP:
                continue
            target = insn.address + insn.size + op.mem.disp - base
            if target in slots:
                hits.append((slots[target], name, insn.address - base, insn.mnemonic, insn.op_str))

by_slot = {}
for slot, fn, off, mn, ops in hits:
    by_slot.setdefault(slot, []).append((fn, off, mn, ops))
verdict_ok = True
for slot in sorted(slots.values()):
    rows = by_slot.get(slot, [])
    print("  %s: %d reference(s)" % (slot, len(rows)))
    for fn, off, mn, ops in rows:
        call = "  <-- CALL THROUGH THE SLOT" if mn.startswith("call") or mn.startswith("jmp") else ""
        short = fn.replace("Hooks::BSFaceGenNiNodeHooks::", "")
        print("     0x%06X %-6s %-34s in %s%s" % (off, mn, ops, short, call))
        if call:
            verdict_ok = False
        if "Hook" != short and not short.startswith("Hook"):
            # a reference from anything but the installer is what the contract forbids
            if mn.startswith("call") or mn.startswith("jmp") or mn.startswith("mov"):
                pass
print("  VERDICT: %s" % ("no call through either stored original" if verdict_ok
                         else "A CALL THROUGH A STORED ORIGINAL EXISTS, the row must NOT be added"))

# The ids the installer embeds, so the row is against the right engine entries.
ids = set()
for rva, size, name in funcs:
    if not name.startswith("Hooks::BSFaceGenNiNodeHooks::Hook"):
        continue
    try:
        code = pe.get_data(rva, size)
    except Exception:
        continue
    for insn in md.disasm(code, base + rva):
        for op in insn.operands:
            if op.type == X86_OP_IMM and 20000 <= op.imm <= 300000:
                ids.add(op.imm)
print("  ids embedded in the installer: %s" % sorted(ids))
