# List the images in an (old, armv7) dyld_shared_cache.  python dsc_list.py cache [filter]
import struct
import sys

f = open(sys.argv[1], "rb")
d = f.read(0x1000)
magic = d[:16].rstrip(b"\0").decode()
(mapOff, mapCnt, imgOff, imgCnt) = struct.unpack("<IIII", d[16:32])
f.seek(imgOff)
imgs = f.read(imgCnt * 32)
flt = sys.argv[2].lower() if len(sys.argv) > 2 else ""
print(magic, imgCnt, "images")
for i in range(imgCnt):
    addr, mtime, inode, pathOff, pad = struct.unpack("<QQQII", imgs[i * 32 : (i + 1) * 32])
    f.seek(pathOff)
    p = f.read(256).split(b"\0")[0].decode()
    if flt in p.lower():
        print(hex(addr), p)
