#!/usr/bin/env python3
"""sys-botbase client for unattended Switch runs: screenshots, button presses and waiting for a known screen.

  switch_bot.py shot OUT.jpg                    screenshot
  switch_bot.py press A [B ...]                 click buttons (A B X Y L R ZL ZR PLUS MINUS DUP DDOWN DLEFT DRIGHT HOME)
  switch_bot.py hold BTN SECONDS                press and hold (the game polls the pad once per frame: at a few fps a
                                                click is missed, a hold is not)
  switch_bot.py stick LEFT|RIGHT X Y SECONDS    move a stick (-32767..32767), then centre it
  switch_bot.py cmd "raw command"               send a raw sys-botbase command and print the reply
  switch_bot.py run "wait 30; press A; shot a.jpg; stick LEFT 0 32000 5"   a sequence (wait = seconds)
  switch_bot.py state                           classify the screen: one of the names in screens.json, or "other"
  switch_bot.py until STATE TIMEOUT [BTN EVERY]  wait for a screen; meanwhile hold BTN every EVERY seconds.
                                                Also inside `run`: "until title 240; until menu 60 PLUS 4".
                                                Exit code 1 on timeout.

Configuration (environment variables, normally from console-test/credentials.env, see README.md):
  SWITCH_IP    address of the console (required)
  BOT_PORT     sys-botbase port (default 6000)
  REF_DIR      folder with the reference screenshots and screens.json (default console-test/ref, not distributed)

Screens are recognised by comparing a region of the screenshot with a reference image of the same name:
REF_DIR/<name>.jpg. REF_DIR/screens.json maps each name to [[x0, y0, x1, y1], limit]: the region of the 1280x720
capture and the largest mean pixel distance accepted. Make the references with `switch_bot.py shot`, for example
"home" (the HOME menu with your forwarder selected), "title" (press-START screen), "menu" (main menu with Resume
selected) and "game" (the HUD in play). Needs numpy and Pillow for `state` and `until`.
"""
import io
import json
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.environ.get("REF_DIR", os.path.join(HERE, "ref"))
DEFAULT_SCREENS = {"home": ((0, 600, 1280, 720), 6), "menu": ((800, 270, 1100, 400), 10),
                   "game": ((1050, 520, 1210, 680), 15), "title": ((400, 200, 880, 500), 40)}
_refs = {}


def load_screens():
    path = os.path.join(REF, "screens.json")
    if os.path.isfile(path):
        with open(path) as fh:
            return {name: (tuple(box), limit) for name, (box, limit) in json.load(fh).items()}
    return DEFAULT_SCREENS


def features(img, box):
    import numpy as np
    return np.asarray(img.convert("L").crop(box).resize((64, 36)), dtype=float)


def classify(jpeg):
    from PIL import Image
    img = Image.open(io.BytesIO(jpeg))
    best = ("other", 1e9)
    for name, (box, limit) in load_screens().items():
        reference = os.path.join(REF, name + ".jpg")
        if not os.path.isfile(reference):
            continue
        if name not in _refs:
            _refs[name] = features(Image.open(reference), box)
        d = float(abs(features(img, box) - _refs[name]).mean())
        if d <= limit and d < best[1]:
            best = (name, d)
    return best[0]


def console_ip():
    ip = os.environ.get("SWITCH_IP")
    if not ip:
        sys.exit("SWITCH_IP is not set (see tools/console-test/README.md)")
    return ip


class Bot:
    def __init__(self):
        self.s = socket.create_connection((console_ip(), int(os.environ.get("BOT_PORT", "6000"))), timeout=10)

    def send(self, command):
        self.s.sendall((command + "\r\n").encode())

    def reply(self, timeout=10):
        self.s.settimeout(timeout)
        data = b""
        while not data.endswith(b"\n"):
            chunk = self.s.recv(1 << 20)
            if not chunk:
                break
            data += chunk
        return data.strip()


def screenshot(b, tries=5):
    # sys-botbase sometimes resets the connection while the game loads: reconnect and retry.
    for i in range(tries):
        try:
            b.send("pixelPeek")
            return bytes.fromhex(b.reply(30).decode())
        except (OSError, ValueError):
            if i == tries - 1:
                raise
            time.sleep(3)
            b = Bot()


def until(w):
    want, timeout = w[1], float(w[2])
    button, every = (w[3], float(w[4])) if len(w) > 4 else (None, 0)
    start = time.time()
    last_press = 0
    state = "?"
    while time.time() - start < timeout:
        try:
            b = Bot()
            state = classify(screenshot(b))
        except OSError:
            time.sleep(3)
            continue
        if state == want:
            print(f"{want} after {time.time() - start:.0f} s", flush=True)
            return True
        if button and time.time() - last_press >= every:
            b.send(f"press {button}")
            time.sleep(0.7)
            b.send(f"release {button}")
            last_press = time.time()
        time.sleep(1.5)
    print(f"timeout waiting for {want} (last: {state})", flush=True)
    return False


def execute(b, a):
    if a[0] == "shot":
        with open(a[1], "wb") as fh:
            fh.write(screenshot(b))
    elif a[0] == "press":
        for button in a[1:]:
            b.send(f"click {button}")
            time.sleep(0.35)
    elif a[0] == "hold":
        b.send(f"press {a[1]}")
        time.sleep(float(a[2]))
        b.send(f"release {a[1]}")
    elif a[0] == "stick":
        b.send(f"setStick {a[1]} {a[2]} {a[3]}")
        time.sleep(float(a[4]))
        b.send(f"setStick {a[1]} 0 0")
    elif a[0] == "cmd":
        b.send(a[1])
        print(b.reply().decode(errors="replace"))
    else:
        sys.exit(f"unknown command: {a[0]}")
    time.sleep(0.2)


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == "run":
        for step in a[1].split(";"):
            w = step.split()
            if not w:
                continue
            if w[0] == "wait":
                time.sleep(float(w[1]))
            elif w[0] == "until":
                if not until(w):
                    sys.exit(1)
            else:
                execute(Bot(), w)
        return
    if a[0] == "until":
        sys.exit(0 if until(a) else 1)
    if a[0] == "state":
        print(classify(screenshot(Bot())))
        return
    execute(Bot(), a)


if __name__ == "__main__":
    main()
