# Print the LC_LOAD_DYLIB dependencies of one image inside an old armv7 dyld_shared_cache (read-only).
#   python dsc_deps.py cache /System/Library/PrivateFrameworks/X.framework/X
import struct
import sys

f = open(sys.argv[1], "rb")
d = f.read(0x1000)
(mapOff, mapCnt, imgOff, imgCnt) = struct.unpack("<IIII", d[16:32])
f.seek(mapOff)
maps = [struct.unpack("<QQQII", f.read(32)) for _ in range(mapCnt)]


def v2o(a):
    for addr, size, off, _, _ in maps:
        if addr <= a < addr + size:
            return a - addr + off


f.seek(imgOff)
imgs = f.read(imgCnt * 32)
for i in range(imgCnt):
    addr, _, _, pathOff, _ = struct.unpack("<QQQII", imgs[i * 32 : (i + 1) * 32])
    f.seek(pathOff)
    p = f.read(256).split(b"\0")[0].decode()
    if p != sys.argv[2]:
        continue
    o = v2o(addr)
    f.seek(o)
    hdr = f.read(28)
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags = struct.unpack("<7I", hdr)
    cmds = f.read(sizeofcmds)
    q = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack("<II", cmds[q : q + 8])
        if cmd in (0xC, 0x80000018, 0x1F, 0xD):  # LOAD_DYLIB, LOAD_WEAK, REEXPORT, ID_DYLIB
            (noff,) = struct.unpack("<I", cmds[q + 8 : q + 12])
            print(
                {0xC: "load", 0x80000018: "weak", 0x1F: "reexport", 0xD: "id"}[cmd],
                cmds[q + noff : q + size].split(b"\0")[0].decode(),
            )
        q += size
