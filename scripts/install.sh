#!/bin/sh
set -eu

base_url=https://github.com/mahiatlinux/MicroPaw/releases/latest/download
python_cmd=
for candidate in python3 python python3.14 python3.13 python3.12 python3.11 python3.10; do
    if command -v "$candidate" >/dev/null 2>&1 &&
        "$candidate" -c 'import sys; raise SystemExit(sys.version_info < (3, 10))' 2>/dev/null; then
        python_cmd=$candidate
        break
    fi
done
if [ -z "$python_cmd" ]; then
    printf '%s\n' "Python 3.10 or newer is required." >&2
    exit 1
fi

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
image="$tmp_dir/micropaw-usb.hex"
checksums="$tmp_dir/SHA256SUMS"
curl -fL --retry 3 "$base_url/micropaw-usb.hex" -o "$image"
curl -fL --retry 3 "$base_url/SHA256SUMS" -o "$checksums"
expected=$(awk '$2 == "micropaw-usb.hex" {print $1}' "$checksums")
if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$image" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$image" | awk '{print $1}')
else
    printf '%s\n' "A SHA-256 command is required." >&2
    exit 1
fi
if [ -z "$expected" ] || [ "$actual" != "$expected" ]; then
    printf '%s\n' "Firmware checksum verification failed." >&2
    exit 1
fi

"$python_cmd" -m venv "$tmp_dir/venv"
python_cmd="$tmp_dir/venv/bin/python"
"$python_cmd" -m pip install --upgrade "esptool~=5.0"
printf '%s\n' "Connecting to the ESP32-S3. Hold BOOT now if automatic reset is unavailable."
if [ -n "${MICROPAW_PORT:-}" ]; then
    "$python_cmd" -m esptool --chip esp32s3 --port "$MICROPAW_PORT" --before default-reset --after hard-reset write-flash 0x0 "$image"
else
    "$python_cmd" -m esptool --chip esp32s3 --before default-reset --after hard-reset write-flash 0x0 "$image"
fi
printf '%s\n' "MicroPaw flash complete."
