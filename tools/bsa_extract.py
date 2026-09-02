"""Pull named files out of Skyrim SE BSAs without unpacking the archive.

Written 2026-08-20 to answer one question about four head meshes, when
unpacking `Skyrim - Meshes0.bsa` to reach them would have cost 1.1 GB. It reads
the v105 header, walks the folder and file records, and decompresses only the
entries whose full path contains the substring asked for.

⚠ SEARCH BY SUBSTRING, AND THE PATH IS FOLDED TO LOWER CASE. Passing a leaf
name alone is usually right; pass a folder fragment too when a leaf is common.

⚠ IT SEARCHES EVERY BSA UNDER THE ROOTS GIVEN, WHICH IS HOW YOU FIND OUT WHO
WINS. A loose file beats every archive and this tool cannot see loose files, so
check for those separately before concluding vanilla's copy is the one the game
loads.

Needs lz4 (installed). BSArch is also on this machine, under the Nolvus
instance's TOOLS folder, when a full unpack really is wanted.

Usage: python bsa_extract.py <substring> <outdir> <root> [<root> ...]
"""
import struct, sys, os, lz4.frame

def read_bsa(path, want_substr):
    hits = []
    with open(path, 'rb') as f:
        hdr = f.read(36)
        if hdr[:4] != b'BSA\x00':
            return hits
        (ver, off, aflags, nfold, nfile, lfoldname, lfilename, fflags) = struct.unpack('<8I', hdr[4:36])
        if ver != 105:
            return hits
        embed = bool(aflags & 0x100)
        defcomp = bool(aflags & 0x4)
        hasdirnames = bool(aflags & 0x1)
        f.seek(off)
        folders = []
        for _ in range(nfold):
            h, cnt, pad, foff = struct.unpack('<QIIQ', f.read(24))
            folders.append((cnt, foff))
        # folder blocks
        entries = []   # (foldername, filehash, size, offset)
        for cnt, foff in folders:
            f.seek(foff - lfilename)
            name = ''
            if hasdirnames:
                ln = f.read(1)[0]
                name = f.read(ln).rstrip(b'\x00').decode('cp1252', 'replace')
            for _ in range(cnt):
                fh, fsz, fof = struct.unpack('<QII', f.read(16))
                entries.append([name, fh, fsz, fof, None])
        # file name block
        blob = f.read(lfilename)
        names = blob.split(b'\x00')
        for i, e in enumerate(entries):
            if i < len(names):
                e[4] = names[i].decode('cp1252', 'replace')
        for name, fh, fsz, fof, fn in entries:
            full = (name + chr(92) + (fn or '')).lower()
            if want_substr.lower() in full:
                comp = defcomp
                real = fsz & 0x3FFFFFFF
                if fsz & 0x40000000:
                    comp = not comp
                f.seek(fof)
                raw = f.read(real)
                p = 0
                if embed:
                    ln = raw[0]; p = 1 + ln
                if comp:
                    origsz = struct.unpack('<I', raw[p:p+4])[0]
                    data = lz4.frame.decompress(raw[p+4:])
                else:
                    data = raw[p:]
                hits.append((os.path.basename(path), full, len(data), data))
    return hits

if __name__ == '__main__':
    want = sys.argv[1]
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    for root in sys.argv[3:]:
        for dirpath, _, files in os.walk(root):
            for fn in files:
                if not fn.lower().endswith('.bsa'):
                    continue
                p = os.path.join(dirpath, fn)
                try:
                    for arc, full, n, data in read_bsa(p, want):
                        leaf = full.split(chr(92))[-1]
                        out = os.path.join(outdir, arc.replace(' ', '_') + '__' + leaf)
                        with open(out, 'wb') as g:
                            g.write(data)
                        print('%-34s %-62s %8d -> %s' % (arc, full, n, os.path.basename(out)))
                except Exception as ex:
                    print('ERR %s: %s' % (fn, ex), file=sys.stderr)
