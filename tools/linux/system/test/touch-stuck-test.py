#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Surface Duo, 2026-11-20: reproduce KWin's stuck touchscreen gesture on purpose, to test duo-wake's touch reset.

Run as root on the phone, with the screens on and nobody touching them. It makes a uinput touchscreen with 3 slots,
puts 3 fingers down close together (KWin's gesture filter takes them as a 3-finger gesture) and waits until the
screens are off (someone presses the power key). Then it lifts the fingers: KWin's DPMS filter eats those touch-ups,
so the gesture filter keeps the 3 points and from now on swallows all touch, as after closing the phone.
When the screens come on again (power key), duo-wake should log "touch reset (screens on)" and touch should work.
Without the reset, touch stays dead until a 3-finger touch (seat slots 0-2 lifted again) clears it.
Prints a timeline. Writes nothing; the device goes when the script ends.
Limit (2026-11-20): the power key locks the session at once, and a lock-state change while the test fingers are still
down makes KWin cancel them, which clears the gesture. So this power-key variant does NOT reproduce the stuck state;
the idle timeout with the session unlocked does (README "Dotyk przestaje działać").
"""
import fcntl, glob, os, struct, sys, time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556e
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x401c5504, 0x5501, 0x5502
EV_SYN, EV_KEY, EV_ABS, BTN_TOUCH = 0, 1, 3, 0x14a
ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0, 1, 0x2f, 0x35, 0x36, 0x39
W, H, RES = 1350, 1800, 20


def log(msg):
    print('%s %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


def screens_off():
    for p in glob.glob('/sys/class/drm/card*-DSI-*/dpms'):
        try:
            if open(p).read().strip() == 'Off':
                return True
        except OSError:
            pass
    return False


def wait(cond, limit, what):
    end = time.monotonic() + limit
    while not cond():
        if time.monotonic() > end:
            log('gave up waiting: %s' % what)
            return False
        time.sleep(0.1)
    return True


def main():
    if screens_off():
        log('the screens are off: turn them on first')
        return 1
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    for ev in (EV_KEY, EV_ABS):
        fcntl.ioctl(fd, UI_SET_EVBIT, ev)
    fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH)
    fcntl.ioctl(fd, UI_SET_PROPBIT, 1)  # INPUT_PROP_DIRECT
    for code, top, res in ((ABS_X, W - 1, RES), (ABS_Y, H - 1, RES), (ABS_MT_SLOT, 2, 0),
                           (ABS_MT_POSITION_X, W - 1, RES), (ABS_MT_POSITION_Y, H - 1, RES),
                           (ABS_MT_TRACKING_ID, 65535, 0)):
        fcntl.ioctl(fd, UI_SET_ABSBIT, code)
        fcntl.ioctl(fd, UI_ABS_SETUP, struct.pack('HHiiiiii', code, 0, 0, 0, top, 0, 0, res))
    fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack('HHHH80sI', 0x06, 0x045e, 0x0d02, 1, b'duo-touch-stuck-test', 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)

    def emit(events):
        os.write(fd, b''.join(struct.pack('llHHi', 0, 0, t, c, v) for t, c, v in events + [(EV_SYN, 0, 0)]))

    try:
        time.sleep(1.5)  # KWin picks the device up through udev
        x0, y = W // 2 - 60, H // 2
        down = []
        for i in range(3):  # 3 fingers 3 mm apart, one frame: KWin takes a 3-finger gesture
            down += [(EV_ABS, ABS_MT_SLOT, i), (EV_ABS, ABS_MT_TRACKING_ID, 100 + i),
                     (EV_ABS, ABS_MT_POSITION_X, x0 + 60 * i), (EV_ABS, ABS_MT_POSITION_Y, y)]
        emit(down + [(EV_KEY, BTN_TOUCH, 1), (EV_ABS, ABS_X, x0), (EV_ABS, ABS_Y, y)])
        log('3 fingers down: press the power key now (screens off)')
        if not wait(screens_off, 120, 'screens off'):
            emit([e for i in range(3) for e in ((EV_ABS, ABS_MT_SLOT, i), (EV_ABS, ABS_MT_TRACKING_ID, -1))]
                 + [(EV_KEY, BTN_TOUCH, 0)])
            return 1
        log('screens off')
        time.sleep(1.5)
        emit([e for i in range(3) for e in ((EV_ABS, ABS_MT_SLOT, i), (EV_ABS, ABS_MT_TRACKING_ID, -1))]
             + [(EV_KEY, BTN_TOUCH, 0)])
        log('fingers lifted while the screens are off: KWin should now be stuck; press the power key again')
        if wait(lambda: not screens_off(), 300, 'screens on'):
            log('screens on: duo-wake should reset touch within ~1.5 s; now try touching the screens')
        time.sleep(5)
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)
    return 0


if __name__ == '__main__':
    sys.exit(main())
