"""Type text into the guest via QEMU monitor sendkey (US layout). A trailing newline in the text presses Return.
Usage: python3 -I scripts/guest-type.py 'sysctl hw.ncpu\n'"""
import socket, sys, time

SOCK = "/tmp/hkvm-monitor.sock"
PLAIN = {" ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal", "[": "bracket_left",
         "]": "bracket_right", "\\": "backslash", ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
         ",": "comma", ".": "dot", "/": "slash"}
SHIFT = {"_": "minus", "+": "equal", "{": "bracket_left", "}": "bracket_right", "|": "backslash",
         ":": "semicolon", '"': "apostrophe", "~": "grave_accent", "<": "comma", ">": "dot", "?": "slash",
         "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0"}


def key(c):
    if c.isalpha() and c.isascii():
        return ("shift-" if c.isupper() else "") + c.lower()
    if c.isdigit():
        return c
    if c in PLAIN:
        return PLAIN[c]
    if c in SHIFT:
        return "shift-" + SHIFT[c]
    raise ValueError("no key for %r" % c)


text = sys.argv[1].encode().decode("unicode_escape")
s = socket.socket(socket.AF_UNIX)
s.connect(SOCK)
for c in text:
    s.sendall(("sendkey %s\n" % key(c)).encode())
    time.sleep(0.04)
time.sleep(0.2)
s.close()
