import argparse
import sys
import time

import serial


def reset_board(port):
    port.dtr = False
    port.rts = True
    time.sleep(0.15)
    port.rts = False


def collect(name, seconds):
    output = bytearray()
    sent = False
    with serial.Serial(name, 115200, timeout=0.1, write_timeout=1) as port:
        reset_board(port)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            block = port.read(2048)
            if block:
                output.extend(block)
                if not sent and b"MicroPaw ready" in output:
                    time.sleep(0.25)
                    port.write(b"metrics\n")
                    port.flush()
                    sent = True
            time.sleep(0.02)
    text = output.decode("utf-8", errors="replace")
    print(text, end="")
    return sent and text.count("heap_free=") >= 2


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("--seconds", type=float, default=8)
    arguments = parser.parse_args()
    return 0 if collect(arguments.port, arguments.seconds) else 1


if __name__ == "__main__":
    sys.exit(main())
