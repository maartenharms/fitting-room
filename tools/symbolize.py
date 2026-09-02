"""Turn a crash-log address into a Fitting Room function name.

    python tools/symbolize.py 0x7823C 0x1111EF ...
    python tools/symbolize.py FittingRoom.dll+007823C

⚠ WHY THIS EXISTS. A trainwreck report names offsets, not functions:

    [0] 0x7FFFAE7C823C  FittingRoom.dll+007823C
        mov rax,[rsi]

Without symbols that is unactionable, which is exactly where a crash on
2026-08-05 stopped. The build now emits a PDB beside the DLL, and this maps
each offset back to the function that contains it.

⚠ THE PDB MUST BE THE ONE FROM THE BUILD THAT CRASHED. Offsets move between
builds, so a PDB from a later build will confidently name the WRONG function.
Check the DLL's timestamp against the crash before believing the answer. If the
crashing build had no PDB, rebuild that exact commit with only the CMake symbol
change applied: /Zi and /DEBUG do not alter code generation, and /OPT:REF plus
/OPT:ICF keep the layout, so the offsets line up.

Read-only. Loads the image for symbol purposes only; nothing is executed.
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import sys

dbghelp = ctypes.WinDLL("dbghelp")
kernel32 = ctypes.WinDLL("kernel32")

SYMOPT_UNDNAME = 0x2
SYMOPT_LOAD_LINES = 0x10
MAX_SYM_NAME = 2000

DEFAULT_DLL = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "build", "release", "FittingRoom.dll")


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.ULONG),
        ("TypeIndex", wt.ULONG),
        ("Reserved", ctypes.c_ulonglong * 2),
        ("Index", wt.ULONG),
        ("Size", wt.ULONG),
        ("ModBase", ctypes.c_ulonglong),
        ("Flags", wt.ULONG),
        ("Value", ctypes.c_ulonglong),
        ("Address", ctypes.c_ulonglong),
        ("Register", wt.ULONG),
        ("Scope", wt.ULONG),
        ("Tag", wt.ULONG),
        ("NameLen", wt.ULONG),
        ("MaxNameLen", wt.ULONG),
        ("Name", ctypes.c_char * (MAX_SYM_NAME + 1)),
    ]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.DWORD),
        ("Key", ctypes.c_void_p),
        ("LineNumber", wt.DWORD),
        ("FileName", ctypes.c_char_p),
        ("Address", ctypes.c_ulonglong),
    ]


def parse_addr(text):
    """Accept 0x7823C, 7823C, or FittingRoom.dll+007823C."""
    m = re.search(r"\+([0-9A-Fa-f]+)\s*$", text)
    if m:
        return int(m.group(1), 16)
    return int(text, 16)


def main():
    args = [a for a in sys.argv[1:]]
    dll = DEFAULT_DLL
    if args and args[0].lower().endswith(".dll"):
        dll = args.pop(0)
    dll = os.path.abspath(dll)
    if not args:
        print(__doc__)
        return 2
    if not os.path.exists(os.path.splitext(dll)[0] + ".pdb"):
        print(f"⚠ no PDB beside {dll} - build it first, or offsets cannot be named.")
        return 1

    hproc = wt.HANDLE(kernel32.GetCurrentProcess())
    dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES)
    dbghelp.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
    dbghelp.SymLoadModuleExW.restype = ctypes.c_ulonglong
    dbghelp.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR,
                                         ctypes.c_ulonglong, wt.DWORD, ctypes.c_void_p,
                                         wt.DWORD]
    if not dbghelp.SymInitializeW(hproc, os.path.dirname(dll), False):
        print("SymInitialize failed")
        return 1
    base = dbghelp.SymLoadModuleExW(hproc, None, dll, None, 0, 0, None, 0)
    if not base:
        print("SymLoadModuleEx failed")
        return 1

    print(f"module: {dll}")
    sym = ctypes.cast(ctypes.create_string_buffer(ctypes.sizeof(SYMBOL_INFO)),
                      ctypes.POINTER(SYMBOL_INFO))
    for text in args:
        rva = parse_addr(text)
        sym.contents.SizeOfStruct = 88
        sym.contents.MaxNameLen = MAX_SYM_NAME
        disp = ctypes.c_ulonglong(0)
        ok = dbghelp.SymFromAddr(hproc, ctypes.c_ulonglong(base + rva),
                                 ctypes.byref(disp), sym)
        if not ok:
            print(f"  +0x{rva:07X}  <no symbol covers this address>")
            continue
        name = sym.contents.Name.decode("ascii", "replace")
        line = IMAGEHLP_LINE64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        ldisp = wt.DWORD(0)
        where = ""
        if dbghelp.SymGetLineFromAddr64(hproc, ctypes.c_ulonglong(base + rva),
                                        ctypes.byref(ldisp), ctypes.byref(line)):
            fname = (line.FileName or b"").decode("ascii", "replace")
            where = f"   [{os.path.basename(fname)}:{line.LineNumber}]"
        print(f"  +0x{rva:07X}  {name}+0x{disp.value:X}{where}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
