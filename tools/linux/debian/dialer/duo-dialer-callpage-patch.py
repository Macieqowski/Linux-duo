#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Plasma Dialer 6.3.x: make the call page's Speaker and Mute buttons show their state (Surface Duo, 2026-11-20).

The QML plugin org.kde.telephony declares DialerUtils' NOTIFY signals muteChanged() / speakerModeChanged() without an
argument, while CallPage.qml reads one (`function onMuteChanged(muted) { muteButton.toggledOn = muted }`): QML gets
undefined ("Cannot assign [undefined] to bool"), the buttons are never highlighted, the Mute icon never changes and
every press sends true. The fix reads the properties instead (DialerUtils.mute / DialerUtils.speakerMode; the plugin's
READ functions ask kde-telephony-daemon) and syncs both buttons when a call becomes active.

CallPage.qml is compiled into the plasma-dialer binary by rcc (qrc:/call/CallPage.qml). Rebuilding the dialer needs
its whole KF6 build environment, so this tool patches that one resource in a copy of the binary instead: it finds the
compressed blob (zstd or zlib, as rcc stores it: 4-byte big-endian length, then the data), applies the text changes,
compresses again and writes it back in place. The new blob must not be longer than the old one (rcc's tree points at
the old offset and size; a shorter blob with its new length is fine, the rest is unused). Nothing else in the binary
changes. Run as: duo-dialer-callpage-patch.py /usr/bin/plasma-dialer /tmp/plasma-dialer.new
Exit 0 = patched (prints the sha256), 2 = already patched, 1 = not found / does not fit (nothing written).
Or: duo-dialer-callpage-patch.py --install  (the image build and the apt hook 99-duo-dialer-patch: patches
/usr/bin/plasma-dialer in place, the unpatched binary kept as /var/lib/duo/plasma-dialer.orig; exit 0 when patched now
or already; a running dialer keeps the old page until it is started again).
Needs python3 and, for a zstd resource, the zstd command.
"""
import hashlib, os, shutil, struct, subprocess, sys, tempfile, zlib

MARK = "id: callPage"
REPS = [
    ("function onMuteChanged(muted) {\n            muteButton.toggledOn = muted;\n        }",
     "function onMuteChanged() {\n            muteButton.toggledOn = DialerUtils.mute;\n        }"),
    ("function onSpeakerModeChanged(enabled) {\n            speakerButton.toggledOn = enabled;\n        }",
     "function onSpeakerModeChanged() {\n            speakerButton.toggledOn = DialerUtils.speakerMode;\n        }"),
    ("    title: i18n(\"Active call list\")\n",
     "    title: i18n(\"Active call list\")\n"
     "    // duo: the buttons start from the daemon's state for every call\n"
     "    onCallActiveChanged: {\n"
     "        muteButton.toggledOn = DialerUtils.mute;\n"
     "        speakerButton.toggledOn = DialerUtils.speakerMode;\n"
     "    }\n"),
]
DONE = "muteButton.toggledOn = DialerUtils.mute;"


def zstd(args, data):
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "in")
        with open(src, "wb") as f:
            f.write(data)
        # from a file, so the frame header carries the content size (Qt's resource reader needs it)
        r = subprocess.run(["zstd", "-q", "-c"] + args + [src], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        return r.stdout if r.returncode == 0 else None


def blobs(data):
    """(kind, start of the length field, length, decompressed text) for every rcc blob that holds the call page"""
    have_zstd = shutil.which("zstd") is not None
    p = data.find(b"\x28\xb5\x2f\xfd")
    while p >= 4:
        n = struct.unpack(">I", data[p - 4:p])[0]
        if 16 < n < (1 << 20) and p + n <= len(data) and have_zstd:
            out = zstd(["-d"], data[p:p + n])
            if out and MARK.encode() in out:
                yield "zstd", p - 4, n, out.decode()
        p = data.find(b"\x28\xb5\x2f\xfd", p + 1)
    p = data.find(b"\x78")
    while p >= 8:
        if data[p + 1] in (0x01, 0x5e, 0x9c, 0xda):
            n, size = struct.unpack(">II", data[p - 8:p])
            if 8 < n < (1 << 20) and 0 < size < (1 << 22) and p - 4 + n <= len(data):
                try:
                    out = zlib.decompress(data[p:p - 4 + n])
                except zlib.error:
                    out = b""
                if len(out) == size and MARK.encode() in out:
                    yield "zlib", p - 8, n, out.decode()
        p = data.find(b"\x78", p + 1)


def pack(kind, text):
    raw = text.encode()
    if kind == "zlib":
        return min((struct.pack(">I", len(raw)) + zlib.compress(raw, lvl) for lvl in (9, 8, 7, 6)), key=len)
    best = None
    for args in (["-19"], ["--ultra", "-22"], ["--ultra", "-22", "--long"]):
        c = zstd(args, raw)
        if c and zstd(["-d"], c) == raw and (best is None or len(c) < len(best)):
            best = c
    return best


def install(binary="/usr/bin/plasma-dialer"):
    if not os.path.exists(binary):
        return 0
    tmp = binary + ".duo-new"
    sys.argv = [sys.argv[0], binary, tmp]
    rc = main()
    if rc != 0:
        return 0 if rc == 2 else rc
    os.makedirs("/var/lib/duo", exist_ok=True)
    shutil.copy2(binary, "/var/lib/duo/plasma-dialer.orig")
    shutil.copymode(binary, tmp)
    os.replace(tmp, binary)
    return 0


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: %s /usr/bin/plasma-dialer OUTPUT | --install" % sys.argv[0])
    data = bytearray(open(sys.argv[1], "rb").read())
    found = list(blobs(bytes(data)))
    if len(found) != 1:
        print("call page resource found %d times: stop" % len(found))
        return 1
    kind, at, n, text = found[0]
    print("CallPage.qml: %s blob at 0x%x, %d bytes (%d bytes of text)" % (kind, at, n, len(text)))
    if DONE in text:
        print("already patched")
        return 2
    new = text
    for old, rep in REPS:
        if new.count(old) != 1:
            print("text to change not found exactly once (another dialer version?): %r" % old[:40])
            return 1
        new = new.replace(old, rep)
    blob = pack(kind, new)
    if blob is None or len(blob) > n:
        # the licence header is the only text that can go without changing the page
        if new.startswith("/*") and "*/\n" in new:
            lean = new[new.index("*/\n") + 3:].lstrip("\n")
            lean = "// SPDX-License-Identifier: LGPL-2.0-or-later (Plasma Dialer, patched for the Surface Duo)\n" + lean
            blob = pack(kind, lean)
            new = lean
    if blob is None or len(blob) > n:
        print("patched page does not fit: %s bytes for %d" % (blob and len(blob), n))
        return 1
    data[at:at + 4] = struct.pack(">I", len(blob))
    data[at + 4:at + 4 + len(blob)] = blob
    data[at + 4 + len(blob):at + 4 + n] = bytes(n - len(blob))
    check = [b for b in blobs(bytes(data)) if b[1] == at]
    if len(check) != 1 or check[0][3] != new or check[0][2] != len(blob):
        print("verification of the patched blob failed: nothing written")
        return 1
    with open(sys.argv[2], "wb") as f:
        f.write(data)
    print("patched: blob %d -> %d bytes; %s %s" % (n, len(blob), hashlib.sha256(data).hexdigest(), sys.argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(install() if sys.argv[1:] == ["--install"] else main())
