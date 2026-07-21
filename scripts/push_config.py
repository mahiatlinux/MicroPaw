import argparse
import pathlib
import sys
import time

import serial


MAX_SIZE = 4096


def wait_for(port, expected, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if line.startswith("CONFIG ERROR"):
            raise RuntimeError(line)
        if line == expected:
            return
    raise RuntimeError(f"Timed out waiting for {expected}")


def push(name, path, reboot):
    data = path.read_bytes()
    data.decode("utf-8")
    if not data or len(data) > MAX_SIZE:
        raise ValueError(f"Configuration must be 1-{MAX_SIZE} bytes")
    with serial.Serial(name, 115200, timeout=0.1, write_timeout=2) as port:
        port.dtr = False
        port.rts = False
        time.sleep(1)
        port.reset_input_buffer()
        port.write(f"push-config {len(data)}\n".encode())
        port.flush()
        wait_for(port, "CONFIG READY", 5)
        port.write(data)
        port.flush()
        wait_for(port, "CONFIG OK", 10)
        if reboot:
            port.write(b"reboot\n")
            port.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("file", type=pathlib.Path)
    parser.add_argument("--reboot", action="store_true")
    arguments = parser.parse_args()
    try:
        push(arguments.port, arguments.file, arguments.reboot)
    except (OSError, UnicodeError, ValueError, RuntimeError, serial.SerialException) as error:
        print(error, file=sys.stderr)
        return 1
    print("Configuration saved." + (" Reboot requested." if arguments.reboot else " Reboot to apply network changes."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
