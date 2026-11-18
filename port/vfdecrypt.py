# Minimal vfdecrypt (FileVault "encrcdsa" v2) for iOS <= 9 root filesystem DMGs.
#   python vfdecrypt.py in.dmg out.dmg <72-hex-char key from The Apple Wiki>
# Key = 16-byte AES-128 key + 20-byte HMAC-SHA1 key. Each 4096-byte chunk is
# AES-CBC with IV = HMAC-SHA1(hmac_key, be32(chunk_no))[:16].
import sys
import struct
import hmac
import hashlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

src, dst, keyhex = sys.argv[1:4]
key = bytes.fromhex(keyhex)
aes_key, hmac_key = key[:16], key[16:36]
f = open(src, "rb")
hdr = f.read(0x100)
assert hdr[:8] == b"encrcdsa", "not an encrcdsa v2 image"
# v2 header (big endian): sig[8] version blocksize? ... fields per vfdecrypt.c
(version, enc_iv_size, _u1, _u2, _u3, _u4, _u5, uuid, blocksize, datasize, dataoffset) = struct.unpack(
    ">IIIIIII16sIQQ", hdr[8 : 8 + 4 * 7 + 16 + 4 + 16]
)
blocksize = blocksize or 4096
f.seek(dataoffset)
out = open(dst, "wb")
n = 0
remaining = datasize
while remaining > 0:
    blk = f.read(blocksize)
    if not blk:
        break
    iv = hmac.new(hmac_key, struct.pack(">I", n), hashlib.sha1).digest()[:16]
    d = Cipher(algorithms.AES(aes_key), modes.CBC(iv)).decryptor()
    pt = d.update(blk[: len(blk) // 16 * 16]) + d.finalize()
    out.write(pt[:remaining] if remaining < len(pt) else pt)
    remaining -= len(pt)
    n += 1
out.close()
print("decrypted", n, "chunks, datasize", datasize)
