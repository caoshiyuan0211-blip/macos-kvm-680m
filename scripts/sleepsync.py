#!/usr/bin/env python3
"""Host follows the macOS guest into S3 and brings it back.

Polls the QEMU monitor; when the guest has entered ACPI S3 (VM status "suspended"), suspends the
host (`systemctl suspend`, allowed for the active local session). After the host resumes
(CLOCK_BOOTTIME jumps ahead of CLOCK_MONOTONIC), sends `system_wakeup` so the guest resumes too.
Exits when QEMU goes away. Started by pt-reboot.sh when S3=1. Log: logs/sleepsync.log
"""
import glob, os, socket, subprocess, sys, threading, time

SOCK = "/tmp/hkvm-monitor.sock"
NOHOST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", ".nohostsleep")  # guest-only S3 test
SPURIOUS_S = 3  # host asleep less than this = woke right away at entry, suspend again
MIN_AWAKE = 30  # s after a wake before the host may be suspended again (guards a sleep loop)
MUTE_PORT = 7779  # guest sleepmute daemon: "mute" at kIOMessageSystemWillSleep, "unmute" at wake/abort
MUTE_MS = 90000   # any HID event during the ~40 s guest sleep entry aborts the sleep


def log(msg):
    print(time.strftime("%F %T"), msg, flush=True)


def hmp(cmd):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(5)
    s.connect(SOCK)
    out = b""
    try:
        while not out.endswith(b"(qemu) "):
            out += s.recv(4096)
        s.sendall(cmd.encode() + b"\n")
        out = b""
        while out.count(b"(qemu) ") < 1:
            chunk = s.recv(4096)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        pass
    finally:
        s.close()
    return out.decode(errors="replace")


def status():
    out = hmp("info status")
    if "VM status:" not in out:
        return None
    st = out.split("VM status:")[1].split("\n")[0]
    return "suspended" if "suspended" in st else st.split()[0]  # S3 shows as "paused (suspended)"


def asleep_s():
    return time.clock_gettime(time.CLOCK_BOOTTIME) - time.clock_gettime(time.CLOCK_MONOTONIC)


def suspend_count():
    try:
        return int(open("/sys/power/suspend_stats/success").read())
    except OSError:
        return -1


def wake_irq():
    try:
        return open("/sys/power/pm_wakeup_irq").read().strip()
    except OSError:
        return "?"


def pwrbtn_events():
    """Power-button wakeup-source events (PNP0C0C/LNXPWRBN); bumps when the user's press woke the host."""
    n = 0
    for d in glob.glob("/sys/class/wakeup/*"):
        try:
            if open(d + "/name").read().startswith(("PNP0C0C", "LNXPWRBN")):
                n += int(open(d + "/event_count").read())
        except (OSError, ValueError):
            pass
    return n


def set_mute(ms):
    for oid in ("evkbd0", "evmouse0"):
        out = hmp(f"qom-set /objects/{oid} mute-ms {ms}")
        if "rror" not in out:
            return oid
    return None


def mute_listener():
    """Guest -> 10.0.2.2:7779 (slirp) -> here: mute QEMU input while the guest enters sleep."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("127.0.0.1", MUTE_PORT))
    while True:
        word = u.recv(64).decode(errors="replace").strip()
        ms = MUTE_MS if word == "mute" else 0
        try:
            oid = set_mute(ms)
        except OSError:
            oid = None
        log(f"guest says {word} -> input mute-ms {ms} via {oid}")


def qemu_alive():
    return subprocess.run(["pgrep", "-x", "qemu-system-x86"], capture_output=True).returncode == 0


def main():
    log("sleepsync started")
    threading.Thread(target=mute_listener, daemon=True).start()
    last_wake = 0.0
    while qemu_alive():
        try:
            st = status()
        except OSError:
            st = None
        if st == "suspended" and time.monotonic() - last_wake > MIN_AWAKE:
            time.sleep(2)  # let QEMU finish the S3 entry
            if status() != "suspended":
                continue
            if os.path.exists(NOHOST):
                log(f"guest is in S3, {NOHOST} set -> NOT suspending host, waking guest in 20 s")
                time.sleep(20)
            else:
                for attempt in range(1, 4):
                    log(f"guest is in S3 -> suspending host (try {attempt})")
                    n0 = suspend_count()
                    b0 = pwrbtn_events()
                    t_before = asleep_s()
                    subprocess.run(["systemctl", "suspend"])
                    # systemctl returns before the host sleeps; a completed cycle bumps suspend_stats/success.
                    t0 = time.monotonic()
                    while suspend_count() == n0 and time.monotonic() - t0 < 60:
                        time.sleep(0.5)
                    slept = asleep_s() - t_before
                    if suspend_count() == n0:
                        log("host did not suspend (inhibited?) -> waking guest")
                        break
                    if slept >= SPURIOUS_S:
                        log(f"host resumed after {slept:.0f} s asleep")
                        break
                    if pwrbtn_events() != b0:
                        log(f"host resumed after {slept:.1f} s asleep (power button)")
                        break
                    # GPIO wake (pinctrl_amd IRQ: touch/touchpad/power button) right at entry -> go again
                    log(f"host woke right away ({slept:.1f} s, wake irq {wake_irq()}) -> retry")
                    time.sleep(3)
            time.sleep(2)  # let vfio/PCI settle
            try:
                hmp("system_wakeup")
            except OSError:
                log("QEMU gone before system_wakeup")
                break
            log("sent system_wakeup")
            try:
                set_mute(0)
            except OSError:
                pass
            for _ in range(20):
                if status() == "running":
                    break
                time.sleep(0.5)
            log(f"guest status: {status()}")
            last_wake = time.monotonic()
        time.sleep(2)
    log("QEMU gone, exiting")


if __name__ == "__main__":
    sys.exit(main())
