# Make a dylib decached from an old (iOS 6) armv7 dyld_shared_cache loadable outside that cache,
# at any address, against another device's cache.
#
# When the cache was built, three kinds of fixup were baked in and then dropped from the image:
#  1. Rebases. The cache slides as one block using its v1 slide table, so per-image rebase info
#     was stripped. We rebuild it: every word the slide table marks, in the image's writable
#     segments, that points back into the image becomes REBASE_TYPE_POINTER.
#  2. Binds that dyld resolved for good. Examples are non-lazy pointers, CFString isa,
#     objc_empty_cache and block isa. Their opcodes are gone, and the pointers hold addresses in
#     the *source* cache. We look up the symbol at each such address: first in the image's
#     indirect symbol table, otherwise among the target image's exported symbols in the source
#     cache. Then we emit a bind for it.
#  3. Lazy pointers that were pre-resolved to source-cache addresses. Their lazy-bind opcodes are
#     still present, so we copy them into the regular bind stream, and dyld binds them at load.
# It also moves __LINKEDIT next to the last segment (so dyld reserves a smaller span) and clears
# the "dylib in cache" header flag.
#
#   python3 fixdylib.py <source dyld_shared_cache_armv7> <decached dylib> <output>
import struct
import sys

cache_path, in_path, out_path = sys.argv[1:4]
C = open(cache_path, "rb").read()
mo, mc, io, ic = struct.unpack_from("<4I", C, 16)
maps = [struct.unpack_from("<QQQII", C, mo + 32 * i) for i in range(mc)]


def c_off(a):
    for m in maps:
        if m[0] <= a < m[0] + m[1]:
            return a - m[0] + m[2]


def cstr(buf, o):
    return buf[o : buf.index(b"\0", o)].decode()


def load_commands(buf, base):
    n = struct.unpack_from("<I", buf, base + 16)[0]
    off = base + 28
    for _ in range(n):
        cmd, size = struct.unpack_from("<II", buf, off)
        yield cmd, off
        off += size


# --- source cache: image ranges and exported symbols -------------------------------------------
images = []  # (lo, hi, path, header file offset)
for i in range(ic):
    addr, _, _, po, _ = struct.unpack_from("<QQQII", C, io + 32 * i)
    path, ho = cstr(C, po), c_off(addr)
    for cmd, off in load_commands(C, ho):
        if cmd == 0x1:
            vm, vs = struct.unpack_from("<II", C, off + 24)
            images.append((vm, vm + vs, path, ho))
_exports = {}


def image_at(v):
    return next(((p, ho) for lo, hi, p, ho in images if lo <= v < hi), (None, None))


def exported_name(v):
    path, ho = image_at(v)
    if not path:
        return None, None
    if path not in _exports:
        tab = {}
        for cmd, off in load_commands(C, ho):
            if cmd == 0x2:  # LC_SYMTAB (offsets are into the cache file)
                symoff, nsyms, stroff, _ = struct.unpack_from("<4I", C, off + 8)
                for k in range(nsyms):
                    strx, ntype, sect, desc, val = struct.unpack_from("<IBBhI", C, symoff + 12 * k)
                    if ntype & 0x0E == 0x0E and ntype & 0x01:  # N_SECT | N_EXT
                        tab.setdefault(val, cstr(C, stroff + strx))
        _exports[path] = tab
    tab = _exports[path]
    return path, tab.get(v) or tab.get(v & ~1)


# --- slide table ---------------------------------------------------------------------------------
slide_off, slide_size = struct.unpack_from("<QQ", C, 0x38)
SI = C[slide_off : slide_off + slide_size]
ver, toc_off, toc_cnt, ent_off, _, ent_size = struct.unpack_from("<6I", SI, 0)
assert ver == 1 and ent_size == 128, (ver, ent_size)
toc = struct.unpack_from("<%dH" % toc_cnt, SI, toc_off)
data_map = maps[1]


def slid(a):
    page, word = divmod(a - data_map[0], 4096)
    if not 0 <= page < toc_cnt:
        return False
    bit = word // 4
    return bool(SI[ent_off + toc[page] * ent_size + bit // 8] >> (bit % 8) & 1)


# --- the image -----------------------------------------------------------------------------------
b = bytearray(open(in_path, "rb").read())


def align_segments():
    """dyld mmaps each segment, so its file offset must be page aligned. The decacher packs them
    back to back: insert padding before each misaligned segment and shift every file offset."""
    global b
    while True:
        seglist = [
            (off, struct.unpack_from("<I", b, off + 32)[0])
            for cmd, off in load_commands(b, 0)
            if cmd == 0x1 and struct.unpack_from("<I", b, off + 36)[0]
        ]
        bad = [fo for _, fo in seglist if fo % 4096]
        if not bad:
            return
        at = min(bad)
        delta = 4096 - at % 4096

        def shift(o):
            v = struct.unpack_from("<I", b, o)[0]
            if v >= at:
                struct.pack_into("<I", b, o, v + delta)

        for cmd, off in load_commands(b, 0):
            if cmd == 0x1:
                shift(off + 32)
                for k in range(struct.unpack_from("<I", b, off + 48)[0]):
                    so = off + 56 + 68 * k
                    if struct.unpack_from("<I", b, so + 40)[0]:
                        shift(so + 40)  # section offset
                    if struct.unpack_from("<I", b, so + 48)[0]:
                        shift(so + 48)  # reloff
            elif cmd == 0x2:
                shift(off + 8)
                shift(off + 16)  # symoff, stroff
            elif cmd == 0xB:
                for f in (32, 40, 48, 56, 64, 72):  # toc, modtab, extrefsym, indirectsym, extrel, locrel
                    if struct.unpack_from("<I", b, off + f)[0]:
                        shift(off + f)
            elif cmd in (0x22, 0x80000022):
                for f in (8, 16, 24, 32, 40):
                    if struct.unpack_from("<I", b, off + f)[0]:
                        shift(off + f)
            elif cmd in (0x1D, 0x1E, 0x26, 0x29):  # code sig, split info, function starts, data in code
                if struct.unpack_from("<I", b, off + 8)[0]:
                    shift(off + 8)
        b = b[:at] + bytearray(delta) + b[at:]


align_segments()
flags = struct.unpack_from("<I", b, 24)[0]
segs, sects, dylibs = [], [], []
for cmd, off in load_commands(b, 0):
    if cmd == 0x1:
        vm, vs, fo, fs = struct.unpack_from("<4I", b, off + 24)
        segs.append(dict(name=cstr(b, off + 8)[:16], cmd_off=off, vm=vm, vs=vs, fo=fo, fs=fs))
        for k in range(struct.unpack_from("<I", b, off + 48)[0]):
            so = off + 56 + 68 * k
            sa, ss, sfo = struct.unpack_from("<III", b, so + 32)
            sects.append(
                dict(
                    name=b[so : so + 16].rstrip(b"\0").decode(),
                    vm=sa,
                    size=ss,
                    flags=struct.unpack_from("<I", b, so + 56)[0],
                    r1=struct.unpack_from("<I", b, so + 60)[0],
                    seg=len(segs) - 1,
                )
            )
    elif cmd in (0xC, 0x18 | 0x80000000, 0x1F | 0x80000000, 0x23 | 0x80000000):  # LOAD, WEAK, REEXPORT, UPWARD
        dylibs.append(cstr(b, off + struct.unpack_from("<I", b, off + 8)[0]))
    elif cmd in (0x22, 0x80000022):
        dyld_info = off
    elif cmd == 0x2:
        symtab = struct.unpack_from("<4I", b, off + 8)
    elif cmd == 0xB:
        dysym = struct.unpack_from("<18I", b, off + 8)
linkedit = next(s for s in segs if s["name"] == "__LINKEDIT")
body = [s for s in segs if s is not linkedit]
seg_index = {id(s): i for i, s in enumerate(segs)}


def seg_of(a):
    for i, s in enumerate(segs):
        if s is not linkedit and s["vm"] <= a < s["vm"] + s["vs"]:
            return i, s


def word(a):
    i, s = seg_of(a)
    return struct.unpack_from("<I", b, s["fo"] + a - s["vm"])[0]


def inside(v):
    return any(s["vm"] <= v < s["vm"] + s["vs"] for s in body)


def uleb(n):
    out = bytearray()
    while True:
        byte, n = n & 0x7F, n >> 7
        out.append(byte | (0x80 if n else 0))
        if not n:
            return out


def read_uleb(buf, o):
    r = sh = 0
    while True:
        x = buf[o]
        o += 1
        r |= (x & 0x7F) << sh
        sh += 7
        if x < 0x80:
            return r, o


# Addresses already covered by the surviving bind / lazy-bind streams.
reb_off, reb_size, bind_off, bind_size, wb_off, wb_size, lz_off, lz_size, ex_off, ex_size = struct.unpack_from(
    "<10I", b, dyld_info + 8
)


def walk_binds(off, size, lazy):
    """Yield (address, opcode bytes of that entry) for a bind stream."""
    o, end, addr, start = off, off + size, 0, off
    while o < end:
        x = b[o]
        op, imm = x & 0xF0, x & 0x0F
        o += 1
        if op == 0x00:
            if lazy:
                start = o
            continue
        if op in (0x20, 0x80):
            v, o = read_uleb(b, o)
            if op == 0x80:
                addr += v
        elif op == 0x40:
            o = b.index(b"\0", o) + 1
        elif op == 0x60:
            _, o = read_uleb(b, o)
        elif op == 0x70:
            v, o = read_uleb(b, o)
            addr = segs[imm]["vm"] + v
        elif op == 0x90:
            yield addr, b[start:o]
            addr += 4
        elif op == 0xA0:
            v, o = read_uleb(b, o)
            yield addr, b[start:o]
            addr += 4 + v
        elif op == 0xB0:
            yield addr, b[start:o]
            addr += 4 + imm * 4
        elif op == 0xC0:
            n, o = read_uleb(b, o)
            skip, o = read_uleb(b, o)
            for _ in range(n):
                yield addr, b[start:o]
                addr += 4 + skip


bound = {a for a, _ in walk_binds(bind_off, bind_size, False)} | {a for a, _ in walk_binds(wb_off, wb_size, False)}
lazy_entries = list(walk_binds(lz_off, lz_size, True))
lazy_bound = {a for a, _ in lazy_entries}

# Indirect symbol table: name for each __nl_symbol_ptr / __la_symbol_ptr slot.
symoff, nsyms, stroff, _ = symtab
indirectsymoff, nindirect = dysym[12], dysym[13]
slot_sym = {}
for s in sects:
    if s["flags"] & 0xFF in (0x6, 0x7):  # S_NON_LAZY / S_LAZY_SYMBOL_POINTERS
        for k in range(s["size"] // 4):
            idx = struct.unpack_from("<I", b, indirectsymoff + 4 * (s["r1"] + k))[0]
            if idx & 0xC0000000:
                continue
            strx, _, _, desc, _ = struct.unpack_from("<IBBhI", b, symoff + 12 * idx)
            slot_sym[s["vm"] + 4 * k] = (cstr(b, stroff + strx), (desc >> 8) & 0xFF)

libsystem = next(i + 1 for i, d in enumerate(dylibs) if d.endswith("libSystem.B.dylib"))


def ordinal_for(path):
    for i, d in enumerate(dylibs):
        if d == path:
            return i + 1
    if path.startswith("/usr/lib/system/"):
        return libsystem
    return None


rebases, new_binds, unresolved = [], [], []
for i, s in enumerate(segs):
    if s is linkedit or not (data_map[0] <= s["vm"] < data_map[0] + data_map[1]):
        continue
    for a in range(s["vm"], s["vm"] + s["fs"], 4):
        if not slid(a) or a in bound:
            continue
        v = word(a)
        if inside(v):
            if a not in lazy_bound:
                rebases.append((i, a - s["vm"]))
        elif v and a not in lazy_bound:
            if a in slot_sym:
                name, ordn = slot_sym[a]
            else:
                path, name = exported_name(v)
                ordn = ordinal_for(path) if path else None
            if name and ordn:
                new_binds.append((ordn, name, i, a - s["vm"]))
            else:
                unresolved.append((hex(a), hex(v), image_at(v)[0]))

print(
    "rebases %d, recovered binds %d, lazy pointers made eager %d, unresolved %d"
    % (len(rebases), len(new_binds), len(lazy_entries), len(unresolved))
)
for u in unresolved[:20]:
    print("  unresolved", *u)


# --- stubs: the cache builder pointed some __picsymbolstub4 stubs straight at another image's lazy
# pointer (e.g. libdispatch's), which is meaningless once we're out of that cache. Point every stub
# back at our own lazy pointer for the same symbol.
def put(a, v):
    i, s = seg_of(a)
    struct.pack_into("<I", b, s["fo"] + a - s["vm"], v)


def indirect_name(i):
    idx = struct.unpack_from("<I", b, indirectsymoff + 4 * i)[0]
    if idx & 0xC0000000:
        return None
    return cstr(b, stroff + struct.unpack_from("<I", b, symoff + 12 * idx)[0])


la_by_name = {}
for s in sects:
    if s["flags"] & 0xFF == 0x7:
        for k in range(s["size"] // 4):
            la_by_name.setdefault(indirect_name(s["r1"] + k), s["vm"] + 4 * k)
restubbed = 0
for s in sects:
    if s["flags"] & 0xFF == 0x8 and s["name"] == "__picsymbolstub4":
        ssize = 16
        for k in range(s["size"] // ssize):
            st = s["vm"] + ssize * k
            if word(st) != 0xE59FC004 or word(st + 4) != 0xE08FC00C:  # ldr ip,[pc,#4]; add ip,pc,ip
                continue
            target = (st + 12 + word(st + 12)) & 0xFFFFFFFF
            want = la_by_name.get(indirect_name(s["r1"] + k))
            if want and target != want:
                put(st + 12, (want - st - 12) & 0xFFFFFFFF)
                restubbed += 1
print("stubs redirected back to own lazy pointers: %d" % restubbed)


# --- objc: the cache marks method lists "fixed up" (selectors already uniqued in the source cache).
# Outside the cache their names point at private string copies, so clear the flag and libobjc
# re-registers them at load.
def ptr_at(a):
    return word(a) & ~3 if a and seg_of(a) else 0


method_lists = set()
for s in sects:
    if s["name"] == "__objc_classlist":
        for k in range(s["size"] // 4):
            cls = ptr_at(s["vm"] + 4 * k)
            for c in (cls, ptr_at(cls)):  # class and its metaclass (isa)
                ro = ptr_at(c + 16) if c and seg_of(c) else 0
                if ro and seg_of(ro):
                    method_lists.add(ptr_at(ro + 20))
    elif s["name"] == "__objc_catlist":
        for k in range(s["size"] // 4):
            cat = ptr_at(s["vm"] + 4 * k)
            method_lists.update(ptr_at(cat + o) for o in (8, 12))
    elif s["name"] == "__objc_protolist":
        for k in range(s["size"] // 4):
            p = ptr_at(s["vm"] + 4 * k)
            if p and seg_of(p):
                method_lists.update(ptr_at(p + o) for o in (12, 16, 20, 24))
fixed = 0
for ml in method_lists:
    if ml and seg_of(ml):
        e = word(ml)
        if e & 3:
            put(ml, e & ~3)
            fixed += 1
print("objc method lists un-marked as fixed up: %d of %d" % (fixed, len([m for m in method_lists if m])))

# --- emit ----------------------------------------------------------------------------------------
reb = bytearray([0x11])  # SET_TYPE_IMM pointer
for seg, o in rebases:
    reb += bytes([0x20 | seg]) + uleb(o) + bytes([0x51])
reb.append(0)

bind = bytearray([0x51])  # SET_TYPE_IMM pointer
for ordn, name, seg, o in new_binds:
    bind += bytes([0x10 | ordn]) if ordn < 16 else bytes([0x20]) + uleb(ordn)
    bind += bytes([0x40]) + name.encode() + b"\0"
    bind += bytes([0x70 | seg]) + uleb(o) + bytes([0x90])
for _, entry in lazy_entries:  # lazy entries: no DONE, regular stream
    bind += entry
bind.append(0x51)
bind += b[bind_off : bind_off + bind_size]  # original binds; they end with DONE

new = bytearray()
for blob in (reb, bind):
    while (len(b) + len(new)) % 4:
        new.append(0)
    blob_off = len(b) + len(new)
    new += blob
    if blob is reb:
        reb_new = (blob_off, len(blob))
    else:
        bind_new = (blob_off, len(blob))
while len(new) % 4:
    new.append(0)
assert linkedit["fo"] + linkedit["fs"] >= len(b) - 16, "LINKEDIT must end the file"
b += new
struct.pack_into("<4I", b, dyld_info + 8, reb_new[0], reb_new[1], bind_new[0], bind_new[1])
last_end = max(s["vm"] + s["vs"] for s in body)
new_vm = (last_end + 0xFFF) & ~0xFFF
new_fs = len(b) - linkedit["fo"]
struct.pack_into("<4I", b, linkedit["cmd_off"] + 24, new_vm, (new_fs + 0xFFF) & ~0xFFF, linkedit["fo"], new_fs)
struct.pack_into("<I", b, 24, flags & ~0x80000000)  # MH_DYLIB_IN_CACHE
open(out_path, "wb").write(b)
print(
    "__LINKEDIT %#x -> %#x; image spans %#x..%#x"
    % (linkedit["vm"], new_vm, min(s["vm"] for s in body), new_vm + new_fs)
)
