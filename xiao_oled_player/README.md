# XIAO ESP32C3 + Waveshare 14747 (SSD1351) Media Player

ESP-IDF v5.1 or newer project featuring:
- **No-OTA 4MB Partition Table** (~3 MB LittleFS storage partition)
- **20 MHz SPI DMA** driver for Waveshare 1.5" RGB OLED (128x128, SSD1351)
- **Built-in startup splash** from `resource/fuji.png`, embedded as a `const uint8_t` PNG array in `main/startup_png.h` and displayed for 4 seconds before media playback (no upload required)
- **Continuous directory playback** repeats all supported files in LittleFS indefinitely; the startup splash appears once per boot in the player task
- **Streaming `.lz4s` Video Playback** (12-byte header + length-prefixed raw LZ4 blocks)
- **Motion JPEG `.mjps` playback** with baseline JPEG frames decoded by Apache-2.0 licensed JPEGDEC 1.8.4; LZ4 playback remains supported
- **Static `.png` Playback** with automatic centering (if <128x128) and nearest-neighbor downscaling (if >128x128), followed by a 4-second static hold
- **USB Serial/JTAG CLI** (`LIST`, `UPLOAD` with CRC32 verification, `DOWNLOAD`, `DELETE`) that safely pauses display playback during flash writes

## Wiring (XIAO ESP32C3 -> Waveshare 14747)
- `3V3` -> `VCC`
- `GND` -> `GND`
- `D10` (GPIO10) -> `DIN` (MOSI)
- `D8` (GPIO8) -> `CLK` (SCK)
- `D1` (GPIO3) -> `CS`
- `D2` (GPIO4) -> `DC`
- `D3` (GPIO5) -> `RST`

## Build & Flash
```bash
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash
```

## Python Host Tool (`tools/oled_cli.py`)
```bash
pip install opencv-python numpy lz4 pyserial
python tools/oled_cli.py encode input.mp4 anim.lz4s --fps 25
python tools/oled_cli.py encode input.mp4 anim.lz4s --resize fit
python tools/oled_cli.py encode input.mp4 anim.lz4s --resize stretch
python tools/oled_cli.py encode input.mp4 anim.mjps --codec mjpeg --quality 85 --fps 25
python tools/oled_cli.py --port /dev/ttyACM0 upload anim.lz4s
python tools/oled_cli.py --port /dev/ttyACM0 upload logo.png
python tools/oled_cli.py --port /dev/ttyACM0 list
```

Video resizing defaults to `--resize fill`: a centered square crop that fills
128x128 without distortion. Use `fit` to preserve the entire image with centered
black bars, or `stretch` to resize directly to 128x128 with changed proportions.

`--fps` resamples the source video (dropping or duplicating frames) to preserve
clip duration. The supported range is 1–60 fps; achievable playback speed depends
on decode and display time on the device. `--quality` controls JPEG quality from
1–100 (default 85). Lower quality generally produces smaller files. Upload `.mjps`
files with the same `upload` command used for `.lz4s` files.

## Motion JPEG stream format

`.mjps` is this project's lightweight container for a sequence of independent
baseline JPEG frames. It is not an AVI file or a raw concatenated `.mjpg` file;
convert source videos with `--codec mjpeg` before uploading.

- 12-byte little-endian header: `MJPS`, uint16 width (128), uint16 height (128),
  uint16 fps (1–60), uint16 flags (0).
- Each frame: uint16 little-endian JPEG byte count, followed by that JPEG's bytes.
  JPEGs must be baseline, 128x128, and at most 33,000 bytes.
- A zero frame length ends the stream. Frames are decoded to big-endian RGB565
  using the existing framebuffer and compressed-input buffer.

JPEGDEC's Apache-2.0 license grants royalty-free use under its terms. Its source,
copyright notices, and license are included in `components/jpegdec/upstream/`.
