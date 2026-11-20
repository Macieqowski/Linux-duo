#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Surface Duo auto-brightness: the ambient light sensor (SLPI via iio-sensor-proxy) sets BOTH panels' backlights.
Plasma's PowerDevil drives only one backlight (DSI-1), the Duo has two (DSI-1, DSI-2).

Curve = ANDROID'S OWN (2026-11-08, android-parity.md): the Duo's framework overlay (vendor/overlay/
FrameworksResSurfaceDuo-V1.apk) config_autoBrightnessLevels / config_autoBrightnessLcdBacklightValues, interpolated in
lux like Android's mapping, setting 0..255 -> panel fraction (v - 1) / 254 like Android's BrightnessSynchronizer.
Debounce like Android's: brighter light must hold 1.5 s, darker light 8 s (config_autoBrightness*LightDebounce), and
only a clear change moves the screens (> 8 % from what this service set last), then a 2 s / 3 s fade.
With PowerDevil (Plasma): the automatic level goes through PowerDevil's D-Bus API, so its slider shows it, and DSI-2
copies whatever DSI-1's backlight gets (automatic level, slider, dim after idle, undim at screen-on). A slider move
holds until the light changes a lot (~3x brighter or darker than when the slider moved).
Without PowerDevil (another shell, or /etc/duo/autobrightness-no-powerdevil): the service writes both backlights
itself, and a change it did not make (a slider) is copied to the other screen and holds the same way.
/etc/duo/autobrightness-off switches the AUTOMATIC part off (the copy to DSI-2 keeps working).
Runs as root (systemd duo-autobrightness.service).
"""
import glob, math, os, time
import gi
from gi.repository import Gio, GLib

OFF = "/etc/duo/autobrightness-off"
# Android's curve (the Duo's framework overlay; numbers only - read with tools/linux/display/android-res-dump.py)
LUX = [0, 1, 2, 5, 10, 21, 40, 70, 110, 165, 250, 400, 650, 1000, 1700, 3000, 5000, 10000, 20000]
SETTING = [11, 26, 32, 44, 55, 67, 79, 89, 97, 106, 113, 125, 141, 154, 172, 194, 213, 236, 255]   # 0..255
TABLE = [(l, (s - 1) / 254.0) for l, s in zip(LUX, SETTING)]
DEBOUNCE_UP, DEBOUNCE_DOWN = 1.5, 8.0      # seconds (config_autoBrightnessBrightening/DarkeningLightDebounce)
state = {"set": None, "fade": None, "written": {}, "pending": None}

def curve(lux):
    lux = max(lux, 0.0)
    for (x0, v0), (x1, v1) in zip(TABLE, TABLE[1:]):
        if lux <= x1:
            return v0 + (v1 - v0) * (lux - x0) / (x1 - x0)
    return TABLE[-1][1]

def backlights():
    return sorted(glob.glob("/sys/class/backlight/*"))

# BALANCE, for a phone whose two panels really differ at the same DCS value. On the test phone they do not
# (2026-11-20, side by side at 900, 400 and 150): the left one only looked brighter because KWin also dimmed the right
# one's image (duo-kwin-outputs). Per-panel factor, one line per backlight in /etc/duo/brightness-balance:
# "ae96000.dsi.0=0.95" (1.0 = as is; missing file = no balance), or a curve "ae96000.dsi.0=0.95@400,0.9@160" = factor
# at that backlight value (of 1023) of the unbalanced screen, linear in between, flat outside.
BALANCE_FILE = "/etc/duo/brightness-balance"
def gain(b, level):
    try:
        for line in open(BALANCE_FILE):
            k, _, f = line.strip().partition("=")
            if k != os.path.basename(b) or not f:
                continue
            pts = []
            for part in f.split(","):
                g, _, at = part.strip().partition("@")
                pts.append((float(at) / 1023.0 if at else 0.0, min(1.0, max(0.2, float(g)))))
            pts.sort()
            if level <= pts[0][0]:
                return pts[0][1]
            for (l0, g0), (l1, g1) in zip(pts, pts[1:]):
                if level <= l1:
                    return g0 + (g1 - g0) * (level - l0) / (l1 - l0)
            return pts[-1][1]
    except (OSError, ValueError):
        pass
    return 1.0

def unbalanced(b, v, mx):
    """the common (unbalanced) level that gives value v on screen b"""
    lv = v / mx
    for _ in range(4):
        lv = v / mx / gain(b, lv)
    return min(1.0, lv)

def put(path, v):
    """One backlight write; False if the panel refused it. With the screens off (idle, power key, phone closed) the
    DSI backlight answers EINVAL, but the backlight core has already stored the value and the panel driver sends it
    again when the screen comes back (duo_panel_enable), so the value counts as written either way. A bare
    open().write() raised that error only when the file was closed, outside any try: 421 lines of "Exception ignored
    ... OSError: [Errno 22]" in the log on 2026-11-20."""
    try:
        with open(path, "w") as f:
            f.write(str(v))
        return True
    except OSError:
        return False

def read_int(path):
    try:
        return int(open(path).read())
    except (OSError, ValueError):
        return None

# --- PowerDevil (Plasma) ---
# Plasma's brightness slider, its dimming after 30 s idle and its undim at every screen-on belong to PowerDevil, which
# drives ONE backlight (DSI-1; org.kde.ScreenBrightness display0, 0..10000). A level written to DSI-1's sysfs behind
# its back did not last: at every screen-on PowerDevil wrote its own value back (10000 = 1023 when nobody had set it),
# 30 s later its dim (30 % of it), and this service took both for slider moves and copied them to DSI-2: in practice
# both screens at 100 % after every wake, and the slider always at maximum (2026-11-20). With PowerDevil running the
# automatic level therefore goes through its D-Bus API (context "duo-autobrightness", no OSD), and DSI-2 follows
# whatever DSI-1's backlight gets (automatic, slider, dim, undim). A slider move comes as a BrightnessChanged signal
# with another context; dim and undim send no signal (checked with gdbus monitor).
import pwd, re, subprocess
PD_SVC, PD_ROOT, PD_IF = "org.kde.ScreenBrightness", "/org/kde/ScreenBrightness", "org.kde.ScreenBrightness"
PD_CTX = "duo-autobrightness"
PD_OFF = "/etc/duo/autobrightness-no-powerdevil"     # write DSI-1's sysfs directly, as before

def session_user():
    for d in sorted(os.listdir("/run/user")) if os.path.isdir("/run/user") else []:
        if d.isdigit() and int(d) >= 1000 and os.path.exists("/run/user/%s/bus" % d):
            try:
                return pwd.getpwuid(int(d))
            except KeyError:
                pass
    return None

def user_env(u):
    return {"XDG_RUNTIME_DIR": "/run/user/%d" % u.pw_uid, "PATH": "/usr/bin:/bin", "HOME": u.pw_dir,
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/%d/bus" % u.pw_uid}

def as_user(args, timeout=5):
    u = session_user()
    if not u:
        return None
    try:
        r = subprocess.run(args, user=u.pw_uid, group=u.pw_gid, env=user_env(u), capture_output=True, text=True,
                           timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return r.stdout if r.returncode == 0 else None

class PowerDevil:
    SIGNAL = re.compile(r"BrightnessChanged \('([^']*)', (-?\d+), '([^']*)', '([^']*)'\)")

    def __init__(self):
        self.path = self.name = self.max = self.mon = None
        self.buf = b""
        self.next_try = 0.0

    def ready(self):
        """the internal display's object, looked up again every 10 s until PowerDevil answers"""
        if self.path:
            return True
        if os.path.exists(PD_OFF) or time.monotonic() < self.next_try:
            return False
        self.next_try = time.monotonic() + 10.0
        out = as_user(["busctl", "--user", "get-property", PD_SVC, PD_ROOT, PD_IF, "DisplaysDBusNames"]) or ""
        for name in re.findall(r'"([^"]+)"', out):
            o = (as_user(["busctl", "--user", "get-property", PD_SVC, PD_ROOT + "/" + name, PD_IF + ".Display",
                          "IsInternal", "MaxBrightness"]) or "").split()
            if len(o) == 4 and o[1] == "true" and o[3].isdigit() and int(o[3]) > 0:
                self.path, self.name, self.max = PD_ROOT + "/" + name, name, int(o[3])
                break
        if not self.path:
            return False
        self.watch()
        # PowerDevil sets its own value when the session starts (the old mirror below may have taken that for a
        # slider move): apply the automatic level again through PowerDevil, without debounce
        state["set"] = state["manual_ll"] = None
        print("PowerDevil %s (0..%d): automatic level through PowerDevil, DSI-2 follows DSI-1" % (self.name, self.max),
              flush=True)
        return True

    def lost(self):
        if self.path:
            print("PowerDevil gone: writing the backlights directly", flush=True)
        self.path = None
        state["written"] = {}                     # the old mirror starts again from what the backlights hold now
        if self.mon:
            try:
                self.mon.kill(); self.mon.wait()
            except OSError:
                pass
            self.mon = None

    def watch(self):
        u = session_user()
        try:
            self.mon = subprocess.Popen(["gdbus", "monitor", "--session", "--dest", PD_SVC, "--object-path", PD_ROOT],
                                        user=u.pw_uid, group=u.pw_gid, env=user_env(u), stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, bufsize=0)
        except (OSError, AttributeError):
            self.mon = None
            return
        self.buf = b""
        GLib.io_add_watch(self.mon.stdout.fileno(), GLib.PRIORITY_DEFAULT, GLib.IOCondition.IN | GLib.IOCondition.HUP,
                          self.on_data)

    def on_data(self, fd, cond):
        data = os.read(fd, 4096) if cond & GLib.IOCondition.IN else b""
        if not data:
            self.lost()
            return False
        self.buf += data
        *lines, self.buf = self.buf.split(b"\n")
        for line in lines:
            m = self.SIGNAL.search(line.decode(errors="replace"))
            if m and m.group(1) == self.name and PD_CTX not in (m.group(3), m.group(4)):
                on_slider(int(m.group(2)) / self.max)
        return True

    def set(self, level):
        v = max(1, min(self.max, round(level * self.max)))
        if as_user(["busctl", "--user", "call", PD_SVC, self.path, PD_IF + ".Display", "SetBrightnessWithContext",
                    "ius", str(v), "1", PD_CTX]) is None:       # flags 1 = no on-screen indicator
            self.lost()
            return False
        return True

pd = PowerDevil()

def on_slider(level):
    """the user moved Plasma's slider: keep it until the light changes a lot"""
    if state["fade"]:
        GLib.source_remove(state["fade"]); state["fade"] = None
    state["set"] = level
    state["manual_ll"] = state.get("ll")
    print("slider -> %d %% (automatic paused until the light changes a lot)" % round(100 * level), flush=True)

def write(level):
    if pd.path and pd.set(level):
        return                                   # DSI-1 gets it from PowerDevil, DSI-2 from mirror()
    for b in backlights():
        mx = read_int(b + "/max_brightness")
        cur = read_int(b + "/brightness")
        if mx is None or cur is None:
            continue
        v = max(1, round(level * gain(b, level) * mx))
        if v != cur:                             # each write is a command to the panel: only real changes
            put(b + "/brightness", v)
        state["written"][b] = v

def follow():
    """PowerDevil mode: DSI-2 (and any other backlight) copies DSI-1's, whatever set it. Compared every time, so a
    write from someone else (systemd-backlight at boot) is put right too; a refused write (screens off) still sets
    the stored value, so it is not repeated."""
    bls = backlights()
    if len(bls) < 2:
        return
    lead = bls[0]
    v, mx = read_int(lead + "/brightness"), read_int(lead + "/max_brightness")
    if v is None or not mx:
        return
    lv = unbalanced(lead, v, mx)
    for o in bls[1:]:
        omx, cur = read_int(o + "/max_brightness"), read_int(o + "/brightness")
        if not omx or cur is None:
            continue
        ov = max(1, min(omx, round(lv * gain(o, lv) * omx)))
        if ov != cur:
            put(o + "/brightness", ov)
        state["written"][o] = ov

def mirror():
    """every 300 ms. PowerDevil mode: follow(). Otherwise (Phosh, no PowerDevil): a backlight changed by someone else
    (the brightness slider) is copied to the other screen(s), and the automatic part pauses."""
    if pd.ready():
        follow()
        return True
    for b in backlights():
        v, mx = read_int(b + "/brightness"), read_int(b + "/max_brightness")
        if v is None or not mx:
            continue
        last = state["written"].get(b)
        if last is not None and v != last:
            if state["fade"]:
                GLib.source_remove(state["fade"]); state["fade"] = None
            state["written"][b] = v              # the slider's screen: leave it alone (a second writer flickers)
            for o in backlights():
                if o != b:
                    omx = read_int(o + "/max_brightness")
                    if not omx:
                        continue
                    lv = unbalanced(b, v, mx)
                    ov = max(1, min(omx, round(lv * gain(o, lv) * omx)))
                    put(o + "/brightness", ov)
                    state["written"][o] = ov
            state["set"] = unbalanced(b, v, mx)
            state["manual_ll"] = state.get("ll")   # automatic pauses until the light changes a lot
            print("slider %s -> %d %% on both screens" % (os.path.basename(b), round(100 * v / mx)), flush=True)
            break
        state["written"][b] = v
    return True

def fade_to(target):
    start = state["set"] if state["set"] is not None else target
    if state["fade"]:
        GLib.source_remove(state["fade"])
    if abs(target - start) < 0.005:         # nothing to fade (e.g. the first reading): one write at most
        write(target); state["set"] = target; state["fade"] = None
        return
    n = 6                                   # 6 steps (each a panel command): 2 s brighter, 3 s darker
    period = (2000 if target > start else 3000) // n
    steps = {"i": 0}
    def tick():
        steps["i"] += 1
        lv = start + (target - start) * steps["i"] / n
        write(lv); state["set"] = lv
        if steps["i"] >= n:
            state["fade"] = None; return False
        return True
    state["fade"] = GLib.timeout_add(period, tick)

def on_level(lux):
    if os.path.exists(OFF):
        return
    # smooth in log space (a hand passing, a flicker or a reflection should not move the screens), then act only on
    # a clear change of the target level
    ll = math.log10(max(lux, 0.0) + 1.0)
    state["ll"] = ll if state.get("ll") is None else state["ll"] + 0.35 * (ll - state["ll"])
    decide()

def decide():
    """the light only produces events when it changes: also called every 0.5 s so a debounce can complete"""
    if state.get("ll") is None or os.path.exists(OFF):
        return True
    lux = 10 ** state["ll"] - 1.0
    m = state.get("manual_ll")
    if m is not None:
        if abs(state["ll"] - m) < 0.5:            # the slider was used: keep it until the light is ~3x off
            return True
        state["manual_ll"] = None
    target = curve(lux)
    if state["set"] is None:                       # first reading: no debounce
        fade_to(target); return True
    if abs(target - state["set"]) <= 0.08:
        state["pending"] = None; return True
    # Android-style debounce: the new direction must hold 1.5 s (brighter) / 8 s (darker) before the screens move
    up = target > state["set"]; now = time.monotonic(); p = state["pending"]
    if p is None or p[0] != up:
        state["pending"] = (up, now); return True
    if now - p[1] < (DEBOUNCE_UP if up else DEBOUNCE_DOWN):
        return True
    state["pending"] = None
    print("light %.1f lux -> %d %% (Android curve)" % (lux, round(target * 100)), flush=True)
    fade_to(target)
    return True

bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)
proxy = Gio.DBusProxy.new_sync(bus, 0, None, "net.hadess.SensorProxy", "/net/hadess/SensorProxy", "net.hadess.SensorProxy")
# The light sensor (TXC PA22A on the SLPI) reports on change only, and iio-sensor-proxy shows LightLevel 0 until the
# first report. After two boots on 2026-11-20 none came: once the proxy started while the sensor hub was settling
# ("driver_set_polling: assertion 'sensor_device' failed") and stayed at 0 while ssccli read 35 lux; once the sensor
# sent nothing at all in a steady, dim room (also not to ssccli). This service then set the curve's minimum (screens at
# 4 %). So a 0 read right after the claim is never applied: the screens keep PowerDevil's level until a real reading
# arrives (a PropertiesChanged signal, or a value above 0). Meanwhile the claim is released and taken again after
# 6, 12, 24, 48, 96 and 192 s (the sensor is switched off and on, and a stuck proxy driver sets up its polling again).
light = {"seen": False, "tries": 0, "timer": 0}

def later(sec, fn):
    """one pending light timer at a time (a new one replaces it); fn returns True to run again"""
    if light["timer"]:
        GLib.source_remove(light["timer"])
    me = [0]
    def run():
        if light["timer"] == me[0]:
            light["timer"] = 0              # running now: a later() inside fn must not remove it
        again = fn()
        if again and not light["timer"]:
            light["timer"] = me[0]
            return True
        return False
    me[0] = light["timer"] = GLib.timeout_add_seconds(sec, run)

def take(v):
    if v is not None and v.unpack() > 0:
        light["seen"] = True
        on_level(v.unpack())

def claim():
    """True = iio-sensor-proxy not there yet, try again"""
    try:
        proxy.call_sync("ClaimLight", None, 0, 5000, None)
    except GLib.Error as e:
        print("waiting for iio-sensor-proxy:", e.message, flush=True)
        return True
    take(proxy.get_cached_property("LightLevel"))
    if not light["seen"] and light["tries"] < 6:
        later(6 << light["tries"], reclaim)
    return False

def reclaim():
    if light["seen"]:
        return False
    light["tries"] += 1
    print("no light reading from iio-sensor-proxy: claiming the sensor again (%d)" % light["tries"], flush=True)
    try:
        proxy.call_sync("ReleaseLight", None, 0, 5000, None)
    except GLib.Error:
        pass
    later(1, claim)
    return False

def changed(p, props, invalidated):
    d = props.unpack()
    if "LightLevel" in d:
        light["seen"] = True
        on_level(d["LightLevel"])
proxy.connect("g-properties-changed", changed)

def owner_changed(p, pspec):
    """iio-sensor-proxy restarted: its claims are gone, claim again"""
    if p.get_name_owner():
        print("iio-sensor-proxy (re)started: claiming the light sensor", flush=True)
        light["seen"], light["tries"] = False, 0
        later(1, claim)
proxy.connect("notify::g-name-owner", owner_changed)
GLib.timeout_add(300, mirror)
GLib.timeout_add(500, decide)
if claim():
    later(3, claim)
GLib.MainLoop().run()
