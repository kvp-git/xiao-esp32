JPEGDEC 1.8.4 from https://github.com/bitbank2/JPEGDEC/tree/1.8.4.

The upstream source and Apache-2.0 license are preserved in `upstream/`.
Apache-2.0 grants royalty-free use under its terms. Retain the license and
copyright notices when distributing this component.

The ESP-IDF wrapper compiles the portable C++ decoder, without Arduino or
architecture-specific assembly. The player only accepts baseline 128x128 JPEG
frames and requests big-endian RGB565 output for the SSD1351 display.
