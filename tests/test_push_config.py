import os
import pathlib
import pty
import select
import subprocess
import sys
import tempfile
import threading
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read_until(port, marker, seconds):
    deadline = time.monotonic() + seconds
    data = bytearray()
    while marker not in data and time.monotonic() < deadline:
        if select.select([port], [], [], 0.1)[0]:
            data.extend(os.read(port, 256))
    if marker not in data:
        raise TimeoutError(marker)
    return bytes(data)


def read_exact(port, length, seconds):
    deadline = time.monotonic() + seconds
    data = bytearray()
    while len(data) < length and time.monotonic() < deadline:
        if select.select([port], [], [], 0.1)[0]:
            data.extend(os.read(port, length - len(data)))
    if len(data) != length:
        raise TimeoutError(length)
    return bytes(data)


class PushConfigTest(unittest.TestCase):
    def test_upload_and_reboot(self):
        payload = b'wifi_ssid = "test"\ntimezone = "UTC0"\n'
        master, slave = pty.openpty()
        port = os.ttyname(slave)
        received = {}

        def device():
            command = read_until(master, b"\n", 5)
            length = int(command.decode().strip().split()[1])
            os.write(master, b"CONFIG READY\r\n")
            received["payload"] = read_exact(master, length, 5)
            os.write(master, b"CONFIG OK\r\n")
            received["reboot"] = read_until(master, b"\n", 5)

        with tempfile.TemporaryDirectory() as directory:
            config = pathlib.Path(directory) / "credentials.toml"
            config.write_bytes(payload)
            thread = threading.Thread(target=device)
            thread.start()
            result = subprocess.run(
                [sys.executable, ROOT / "scripts/push_config.py", config, "--port", port, "--reboot"],
                capture_output=True,
                text=True,
                timeout=15,
            )
            thread.join(5)

        os.close(master)
        os.close(slave)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(received["payload"], payload)
        self.assertEqual(received["reboot"], b"reboot\n")


if __name__ == "__main__":
    unittest.main()
