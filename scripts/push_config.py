import argparse
import errno
import fcntl
import glob
import os
import pathlib
import select
import struct
import sys
import termios
import time


MAX_SIZE = 4096


def detect_port():
    patterns = (
        "/dev/cu.usbmodem*",
        "/dev/cu.usbserial*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
    )
    ports = sorted({port for pattern in patterns for port in glob.glob(pattern)})
    if len(ports) != 1:
        found = ", ".join(ports) or "none"
        raise RuntimeError(f"Expected one serial port, found {found}. Use --port PORT.")
    return ports[0]


def configure(port):
    attributes = termios.tcgetattr(port)
    attributes[0] = 0
    attributes[1] = 0
    attributes[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
    attributes[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
    attributes[3] = 0
    attributes[4] = termios.B115200
    attributes[5] = termios.B115200
    attributes[6][termios.VMIN] = 0
    attributes[6][termios.VTIME] = 1
    termios.tcsetattr(port, termios.TCSANOW, attributes)
    bits = struct.pack("I", termios.TIOCM_DTR | termios.TIOCM_RTS)
    try:
        fcntl.ioctl(port, termios.TIOCMBIC, bits)
    except OSError as error:
        if error.errno not in (errno.ENOTTY, errno.EINVAL):
            raise


def read_line(port, deadline):
    line = bytearray()
    while time.monotonic() < deadline:
        timeout = max(0, min(0.1, deadline - time.monotonic()))
        if not select.select([port], [], [], timeout)[0]:
            continue
        value = os.read(port, 1)
        if value == b"\n":
            return line.decode("utf-8", errors="replace").strip()
        line.extend(value)
    return ""


def wait_for(port, expected, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = read_line(port, deadline)
        if line.startswith("CONFIG ERROR"):
            raise RuntimeError(line)
        if line == expected:
            return
    raise RuntimeError(f"Timed out waiting for {expected}")


def write_all(port, data):
    offset = 0
    while offset < len(data):
        offset += os.write(port, data[offset:])
    termios.tcdrain(port)


def push(name, path, reboot):
    data = path.read_bytes()
    data.decode("utf-8")
    if not data or len(data) > MAX_SIZE:
        raise ValueError(f"Configuration must be 1-{MAX_SIZE} bytes")
    port = os.open(name, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        os.set_blocking(port, True)
        configure(port)
        time.sleep(1)
        termios.tcflush(port, termios.TCIFLUSH)
        write_all(port, f"push-config {len(data)}\n".encode())
        wait_for(port, "CONFIG READY", 5)
        for offset in range(0, len(data), 64):
            write_all(port, data[offset:offset + 64])
            time.sleep(0.03)
        wait_for(port, "CONFIG OK", 10)
        if reboot:
            write_all(port, b"reboot\n")
    finally:
        os.close(port)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file", type=pathlib.Path)
    parser.add_argument("--port")
    parser.add_argument("--reboot", action="store_true")
    arguments = parser.parse_args()
    try:
        push(arguments.port or detect_port(), arguments.file, arguments.reboot)
    except (OSError, UnicodeError, ValueError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1
    print("Configuration saved." + (" Reboot requested." if arguments.reboot else " Reboot to apply network changes."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
