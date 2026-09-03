#!/bin/sh
# Turn mx5gauge/web/boot.mp4 into the clip the firmware plays.
#
# boot.mp4 is already square and already trimmed to BOOT_MS -- the crop out of
# the portrait source and the trim are baked into that file, so this script
# only ever rescales, thins and packs. Re-cut boot.mp4 if either needs to
# change, and keep FPS below the ceiling in the next paragraph.
#
# 15 fps, not the source's 24. The panel plus the decode cost 66-69 ms a frame
# on the real board (read 8 + inflate 31 + blit 28), so about 15 frames a
# second is the most that can ever be *shown*; storing more would only waste
# flash. Playback is time-paced, so the splash lasts BOOT_MS whatever this is.
#
# ---- why the frames are deflated, and stored big-endian --------------------
#
# Benched on the board 2026-09-03, per 466x466 frame:
#
#   raw 434 KB from flash ......... 46.6 ms
#   67 KB deflated from flash ...... 7.5 ms
#   ROM tinfl inflate to PSRAM ..... 30.9 ms   (14.1 MB/s out)
#
# So compressing is *both* 8x smaller and 8 ms a frame faster than reading the
# raw bytes. The decompressor is `tinfl_decompress_mem_to_mem`, which lives in
# the ESP32-S3's ROM (esp32s3.rom.ld), so it costs no flash and no code.
#
# Raw deflate, one stream per frame, no zlib header -- the firmware seeks to a
# single frame by index, so a whole-file stream would be useless.
#
# Two things that were measured and rejected: RLE came out at 83% of raw and
# *expands* the tail frames, because the dark gradient is full of RGB565 dither
# noise that breaks every run; LZ4 was 12.3% against deflate's 12.2% and would
# need vendored source.
#
# Pixels are written big-endian because that is the order the panel wants.
# The firmware's normal blit path byte-swaps every frame in place (12 ms), for
# LVGL's sake; a clip that arrives already swapped skips that stage entirely.
set -e
cd "$(dirname "$0")/.."
W=466; H=466; FPS=15
mkdir -p build-assets
# rgb565be, not rgb565le: see the note above.
ffmpeg -v error -y -i mx5gauge/web/boot.mp4 \
    -vf "scale=${W}:${H}:flags=lanczos,fps=${FPS}" \
    -pix_fmt rgb565be -f rawvideo build-assets/boot_rgb565.bin
python3 - "$W" "$H" "$FPS" <<'PY'
import struct, sys, zlib

w, h, fps = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
raw = open('build-assets/boot_rgb565.bin', 'rb').read()
frame = w * h * 2
assert len(raw) % frame == 0, "frame size mismatch"
n = len(raw) // frame

FLAG_BE      = 1   # pixels are big-endian, so the firmware must not swap them
FLAG_DEFLATE = 2   # every frame is its own raw deflate stream
HDR = 24           # magic w h frames fps flags max_comp reserved

blobs = []
for k in range(n):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)   # -15 = raw, no zlib header
    blobs.append(c.compress(raw[k * frame:(k + 1) * frame]) + c.flush())

# frames+1 offsets, absolute from the start of the file, so frame k is
# [off[k], off[k+1]) and the last entry closes the final frame.
table = HDR + 4 * (n + 1)
offs, at = [], table
for b in blobs:
    offs.append(at)
    at += len(b)
offs.append(at)

hdr = struct.pack('<4sHHHHIII', b'MX5C', w, h, n, fps,
                  FLAG_BE | FLAG_DEFLATE, max(len(b) for b in blobs), 0)
assert len(hdr) == HDR
with open('build-assets/boot_asset.bin', 'wb') as f:
    f.write(hdr)
    f.write(struct.pack('<%dI' % (n + 1), *offs))
    for b in blobs:
        f.write(b)

print("boot_asset.bin: %d frames %dx%d @%d fps, %.2f MB "
      "(%.1f%% of the %.2f MB raw), biggest frame %d B"
      % (n, w, h, fps, at / 1024 / 1024, 100.0 * at / len(raw),
         len(raw) / 1024 / 1024, max(len(b) for b in blobs)))
print("worst case %d frames x ~69 ms = %.2f s of showing time"
      % (n, n * 0.069))
PY
echo "flash with:  esptool --port <port> write-flash 0x410000 build-assets/boot_asset.bin"
