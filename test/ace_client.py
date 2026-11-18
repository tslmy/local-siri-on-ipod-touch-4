# Test client that talks ACE to acesrv the way an iOS 6 Siri client does.
#   python3 test_client.py HOST PORT ca.pem speech.spx   (Speex WB frames from speexenc)
#   python3 test_client.py HOST PORT ca.pem -t "typed question"
import plistlib
import socket
import ssl
import struct
import sys
import time
import uuid
import zlib

host, port, ca = sys.argv[1], int(sys.argv[2]), sys.argv[3]
ctx = ssl.create_default_context(cafile=ca)
s = ctx.wrap_socket(socket.create_connection((host, port)), server_hostname=host)
print("TLS", s.version(), s.cipher()[0])
s.sendall(
    b"ACE /ace HTTP/1.0\r\nHost: %s\r\nUser-Agent: Assistant(iPhone/iPhone4,1; iPhone OS/6.1.3/10B329) Ace/1.2\r\n"
    b"Content-Length: 2000000000\r\nX-Ace-Host: test\r\n\r\n" % host.encode()
)
s.sendall(b"\xaa\xcc\xee\x02")
comp = zlib.compressobj()


def obj(cls, group, props=None, refId=None):
    o = {"class": cls, "group": group, "aceId": str(uuid.uuid4()).upper(), "properties": props or {}}
    if refId:
        o["refId"] = refId
    data = plistlib.dumps(o, fmt=plistlib.FMT_BINARY)
    s.sendall(comp.compress(struct.pack(">BI", 2, len(data)) + data) + comp.flush(zlib.Z_SYNC_FLUSH))
    return o["aceId"]


obj("CreateAssistant", "com.apple.ace.system", {"language": "en-US"})
obj("LoadAssistant", "com.apple.ace.system", {"assistantId": "X", "language": "en-US"})
s.sendall(comp.compress(struct.pack(">BI", 3, 1)) + comp.flush(zlib.Z_SYNC_FLUSH))  # ping
t0 = time.time()
if sys.argv[4] == "-t":
    obj("StartRequest", "com.apple.ace.system", {"utterance": sys.argv[5], "handsFree": False})
else:
    sid = obj("StartSpeechRequest", "com.apple.ace.speech", {"codec": "Speex_WB_Quality8", "audioSource": "BuiltInMic"})
    raw = open(sys.argv[4], "rb").read()
    frames = []
    i = 0
    while i < len(raw):
        (n,) = struct.unpack("<H", raw[i : i + 2])
        frames.append(raw[i + 2 : i + 2 + n])
        i += 2 + n
    for k in range(0, len(frames), 10):
        obj("SpeechPacket", "com.apple.ace.speech", {"packets": frames[k : k + 10], "packetNumber": k // 10}, refId=sid)
    obj("FinishSpeech", "com.apple.ace.speech", {"packetCount": (len(frames) + 9) // 10}, refId=sid)
    t0 = time.time()

hdr = b""
while b"\r\n\r\n" not in hdr:
    hdr += s.recv(1)
print(hdr.split(b"\r\n")[0].decode())
buf = b""
while len(buf) < 4:
    buf += s.recv(4 - len(buf))
assert buf == b"\xaa\xcc\xee\x02", buf
d = zlib.decompressobj()
data = b""
done = False
while not done:
    chunk = s.recv(65536)
    if not chunk:
        break
    data += d.decompress(chunk)
    while len(data) >= 5:
        t, n = struct.unpack(">BI", data[:5])
        if t in (3, 4):
            print("pong", n)
            data = data[5:]
            continue
        if len(data) < 5 + n:
            break
        o = plistlib.loads(data[5 : 5 + n])
        data = data[5 + n :]
        cls = o["class"]
        p = o.get("properties", {})
        dt = time.time() - t0
        if cls == "SpeechRecognized":
            toks = p["recognition"]["properties"]["phrases"][0]["properties"]["interpretations"][0]["properties"][
                "tokens"
            ]
            print("[%5.2fs] SpeechRecognized: %s" % (dt, " ".join(t["properties"]["text"] for t in toks)))
        elif cls == "AddViews":
            print("[%5.2fs] AddViews: %s" % (dt, p["views"][0]["properties"]["text"]))
        else:
            print("[%5.2fs] %s" % (dt, cls))
        if cls == "RequestCompleted":
            done = True
s.close()
