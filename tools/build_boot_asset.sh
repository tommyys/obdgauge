#!/bin/sh
# Turn mx5gauge/web/boot.mp4 into the raw RGB565 clip the firmware plays.
#
# 12 fps, not the source's 24: the measured panel floor is ~52ms per full-screen
# refresh, so frames beyond ~19fps can never be shown and storing them would
# only waste flash. Playback is time-paced, so the splash still lasts 2.5s.
#
# Raw RGB565 rather than JPEG because the ESP32-S3 has no hardware JPEG decoder,
# and software decode would eat the frame budget the panel already spends.
set -e
cd "$(dirname "$0")/.."
W=466; H=466; FPS=12
mkdir -p build-assets
ffmpeg -v error -y -i mx5gauge/web/boot.mp4 \
    -vf "scale=${W}:${H}:flags=lanczos,fps=${FPS}" \
    -pix_fmt rgb565le -f rawvideo build-assets/boot_rgb565.bin
python3 - "$W" "$H" "$FPS" <<'PY'
import struct, sys
w, h, fps = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
raw = open('build-assets/boot_rgb565.bin','rb').read()
frame = w * h * 2
n = len(raw) // frame
assert len(raw) % frame == 0, "frame size mismatch"
hdr = struct.pack('<4sHHHHI', b'MX5B', w, h, n, fps, 0)
open('build-assets/boot_asset.bin','wb').write(hdr + raw)
print("boot_asset.bin: %d frames %dx%d @%d fps, %.2f MB" %
      (n, w, h, fps, (len(hdr)+len(raw))/1024/1024))
PY
echo "flash with:  esptool --port <port> write-flash 0x410000 build-assets/boot_asset.bin"
