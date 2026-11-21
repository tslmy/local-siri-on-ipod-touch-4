#!/usr/bin/env python3
"""Build build/local-siri_<version>_iphoneos-arm.deb (pure Python: no dpkg needed).

Contains only our binaries and the two models; nothing from Apple.
Usage: scripts/mkdeb.py <version>      (after `just build` and `just models`)
"""

import gzip
import io
import os
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
B, O, M, D, S = (ROOT / "build", ROOT / "build/ipod", ROOT / "build/models", ROOT / "deb", ROOT / "src")
EPOCH = int(os.environ.get("SOURCE_DATE_EPOCH", "1793000000"))
version = sys.argv[1]

# (path on the iPod, source file, mode)
FILES = [
    ("/var/root/siri/localsirid", O / "localsirid", 0o755),
    ("/var/root/siri/sirictl", O / "sirictl", 0o755),
    ("/var/root/siri/run.sh", S / "localsirid/run.sh", 0o755),
    ("/var/root/siri/activate.sh", D / "activate.sh", 0o755),
    ("/var/root/siri/deactivate.sh", D / "deactivate.sh", 0o755),
    ("/var/root/siri/whistle.cact", M / "whistle.cact", 0o644),
    ("/var/root/siri/smollm2-135m-cq4.bin", M / "smollm2-135m-cq4.bin", 0o644),
    ("/var/root/siri/smollm2-135m.tok", M / "smollm2-135m.tok", 0o644),
    ("/var/root/siri/ace/acesrv", O / "acesrv", 0o755),
    ("/var/root/siri/ace/run.sh", S / "acesrv/run.sh", 0o755),
    ("/var/root/siri/ace/plistset", O / "plistset", 0o755),
    ("/var/root/siri/ace/sirinotify", O / "sirinotify", 0o755),
    ("/Library/MobileSubstrate/DynamicLibraries/SiriCapability.dylib", O / "SiriCapability.dylib", 0o755),
    ("/Library/MobileSubstrate/DynamicLibraries/SiriCapability.plist", S / "tweak/SiriCapability.plist", 0o644),
    ("/Library/LaunchDaemons/com.lmy.localsirid.plist", S / "localsirid/com.lmy.localsirid.plist", 0o644),
    ("/Library/LaunchDaemons/com.lmy.acesrv.plist", S / "acesrv/com.lmy.acesrv.plist", 0o644),
    ("/usr/share/doc/com.lmy.local-siri/NOTICE", D / "NOTICE", 0o644),
    ("/usr/share/doc/com.lmy.local-siri/copyright", ROOT / "LICENSE", 0o644),
    ("/usr/share/doc/com.lmy.local-siri/Apache-2.0.txt", D / "licenses/Apache-2.0.txt", 0o644),
    ("/usr/share/doc/com.lmy.local-siri/Speex-BSD.txt", D / "licenses/Speex-BSD.txt", 0o644),
]


def tar_gz(entries):
    """entries: (arcname, source Path or None for a directory, mode)."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as tf:
        for arc, src, mode in entries:
            ti = tarfile.TarInfo(arc)
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            ti.mtime = EPOCH
            ti.mode = mode
            if src is None:
                ti.type = tarfile.DIRTYPE
                tf.addfile(ti)
            else:
                ti.size = src.stat().st_size
                with open(src, "rb") as f:
                    tf.addfile(ti, f)
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=0, compresslevel=9) as g:
        g.write(raw.getvalue())
    return out.getvalue()


def ar_member(name, data):
    hdr = f"{name:<16}{EPOCH:<12}{0:<6}{0:<6}{'100644':<8}{len(data):<10}`\n".encode()
    return hdr + data + (b"\n" if len(data) % 2 else b"")


for _, src, _ in FILES:
    if not src.is_file():
        sys.exit(f"missing {src}: run 'just build' and 'just models' first")

dirs, entries = set(), []
for dst, src, mode in FILES:
    parts = dst.strip("/").split("/")
    for i in range(1, len(parts)):
        dirs.add("/".join(parts[:i]))
data_entries = [("./", None, 0o755)]
data_entries += [(f"./{d}/", None, 0o755) for d in sorted(dirs)]
data_entries += [(f".{dst}", src, mode) for dst, src, mode in FILES]
size_kb = sum(s.stat().st_size for _, s, _ in FILES) // 1024

control = (D / "DEBIAN/control").read_text().replace("@VERSION@", version).rstrip("\n")
control += f"\nInstalled-Size: {size_kb}\n"
ctrl = io.BytesIO()
(B / ".control").write_text(control)
ctrl_entries = [("./", None, 0o755), ("./control", B / ".control", 0o644)]
for n in ("postinst", "prerm", "postrm"):
    ctrl_entries.append((f"./{n}", D / "DEBIAN" / n, 0o755))

deb = (
    b"!<arch>\n"
    + ar_member("debian-binary", b"2.0\n")
    + ar_member("control.tar.gz", tar_gz(ctrl_entries))
    + ar_member("data.tar.gz", tar_gz(data_entries))
)
(B / ".control").unlink()
out = B / f"com.lmy.local-siri_{version}_iphoneos-arm.deb"
out.write_bytes(deb)
print(out, f"{len(deb) / 1e6:.1f} MB")
