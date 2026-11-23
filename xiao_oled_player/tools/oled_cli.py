#!/usr/bin/env python3
import argparse
import os
import struct
import time
import zlib
import serial

def resize_frame(frame, mode: str = "fill"):
    """Return a 128x128 frame using centered fit, fill, or stretch."""
    import cv2

    height, width = frame.shape[:2]
    if mode == "fill":
        side = min(width, height)
        left = (width - side) // 2
        top = (height - side) // 2
        frame = frame[top:top + side, left:left + side]
        target_width = target_height = 128
    elif mode == "fit":
        scale = 128 / max(width, height)
        target_width = max(1, min(128, round(width * scale)))
        target_height = max(1, min(128, round(height * scale)))
    elif mode == "stretch":
        target_width = target_height = 128
    else:
        raise ValueError(f"Unknown resize mode: {mode}")

    interpolation = (cv2.INTER_AREA if target_width < frame.shape[1]
                     or target_height < frame.shape[0] else cv2.INTER_LINEAR)
    resized = cv2.resize(frame, (target_width, target_height), interpolation=interpolation)
    if mode == "fit":
        left = (128 - target_width) // 2
        top = (128 - target_height) // 2
        resized = cv2.copyMakeBorder(
            resized, top, 128 - target_height - top,
            left, 128 - target_width - left,
            cv2.BORDER_CONSTANT, value=(0, 0, 0),
        )
    return resized


def encode_video(input_video: str, output_file: str, fps: int = 25,
                 resize_mode: str = "fill", codec: str = "lz4", quality: int = 85):
    import cv2

    if not 1 <= fps <= 60:
        raise ValueError("FPS must be between 1 and 60")
    if not 1 <= quality <= 100:
        raise ValueError("JPEG quality must be between 1 and 100")
    if codec not in ("lz4", "mjpeg"):
        raise ValueError(f"Unknown codec: {codec}")
    if codec == "lz4":
        import numpy as np
        import lz4.block

    cap = cv2.VideoCapture(input_video)
    if not cap.isOpened():
        raise RuntimeError(f"Could not open video: {input_video}")

    try:
        source_fps = cap.get(cv2.CAP_PROP_FPS)
        if not 0 < source_fps < float("inf"):
            raise ValueError("Cannot determine the source video frame rate")
        with open(output_file, "wb") as f:
            magic = b"MJPS" if codec == "mjpeg" else b"LZ4S"
            f.write(struct.pack("<4sHHHH", magic, 128, 128, fps, 0))
            count = 0
            source_index = -1
            while True:
                ret, frame = cap.read()
                if not ret:
                    break
                source_index += 1
                # Sample by source time; changing FPS preserves clip duration.
                if source_index < int(count * source_fps / fps):
                    continue
                frame = resize_frame(frame, resize_mode)
                if codec == "mjpeg":
                    ok, encoded = cv2.imencode(
                        ".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, quality,
                                        cv2.IMWRITE_JPEG_PROGRESSIVE, 0],
                    )
                    if not ok:
                        raise RuntimeError("JPEG encoding failed")
                    comp = encoded.tobytes()
                    if len(comp) > 33000:
                        raise ValueError("JPEG frame exceeds firmware buffer; lower --quality")
                else:
                    rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB).astype(np.uint16)
                    rgb565 = ((rgb[:, :, 0] & 0xF8) << 8) | ((rgb[:, :, 1] & 0xFC) << 3) | (rgb[:, :, 2] >> 3)
                    raw_bytes = rgb565.astype(">u2").tobytes()
                    comp = lz4.block.compress(raw_bytes, mode="high_compression", compression=12, store_size=False)
                    if len(comp) >= len(raw_bytes):
                        comp = raw_bytes
                # Duplicate frames when the requested FPS exceeds the source FPS.
                while source_index >= int(count * source_fps / fps):
                    f.write(struct.pack("<H", len(comp)))
                    f.write(comp)
                    count += 1
            if count == 0:
                raise RuntimeError("No video frames could be decoded")
            f.write(struct.pack("<H", 0))
    finally:
        cap.release()
    print(f"Encoded {count} frames at {fps} fps -> {output_file} ({os.path.getsize(output_file)} bytes)")


def encode_lz4s(input_video: str, output_lz4s: str, fps: int = 25,
                resize_mode: str = "fill"):
    encode_video(input_video, output_lz4s, fps, resize_mode, codec="lz4")

def cmd_list(port: str):
    with serial.Serial(port, 115200, timeout=3) as ser:
        ser.write(b"LIST\n")
        while True:
            line = ser.readline().decode(errors="ignore").strip()
            if not line:
                break
            print(line)
            if line == "END":
                break

def cmd_upload(port: str, local_file: str, remote_name: str = None):
    if not remote_name:
        remote_name = os.path.basename(local_file)
    with open(local_file, "rb") as f:
        data = f.read()
    crc = zlib.crc32(data) & 0xFFFFFFFF

    with serial.Serial(port, 115200, timeout=5) as ser:
        ser.write(f"UPLOAD {remote_name} {len(data)} {crc:08X}\n".encode())
        resp = ser.readline().decode(errors="ignore").strip()
        if resp != "READY":
            raise RuntimeError(f"Device rejected upload: {resp}")
        # The firmware writes each 1 KB chunk to flash with a 4 KB RX buffer.
        # Pace the sender so a large upload does not overflow that buffer.
        for offset in range(0, len(data), 1024):
            chunk = data[offset:offset + 1024]
            if ser.write(chunk) != len(chunk):
                raise RuntimeError("Incomplete serial write during upload")
            ser.flush()
            time.sleep(0.02)
        result = ser.readline().decode(errors="ignore").strip()
        if not result.startswith("OK UPLOAD_DONE"):
            raise RuntimeError(f"Upload failed: {result or 'no response'}")
        print(result)

def cmd_download(port: str, remote_name: str, local_file: str = None):
    if not local_file:
        local_file = remote_name
    with serial.Serial(port, 115200, timeout=5) as ser:
        ser.write(f"DOWNLOAD {remote_name}\n".encode())
        hdr = ser.readline().decode(errors="ignore").strip()
        if not hdr.startswith("OK DATA "):
            raise RuntimeError(f"Download failed: {hdr}")
        parts = hdr.split()
        size = int(parts[2])
        expected_crc = int(parts[3], 16) if len(parts) > 3 else None

        data = bytearray()
        while len(data) < size:
            chunk = ser.read(size - len(data))
            if not chunk:
                raise RuntimeError("Timeout reading file data")
            data.extend(chunk)

        actual_crc = zlib.crc32(data) & 0xFFFFFFFF
        if expected_crc is not None and actual_crc != expected_crc:
            raise RuntimeError(f"CRC32 mismatch! Expected {expected_crc:08X}, got {actual_crc:08X}")

        with open(local_file, "wb") as f:
            f.write(data)
        print(f"Downloaded {len(data)} bytes -> {local_file} (CRC32={actual_crc:08X} verified)")

def cmd_delete(port: str, remote_name: str):
    with serial.Serial(port, 115200, timeout=3) as ser:
        ser.write(f"DELETE {remote_name}\n".encode())
        print(ser.readline().decode(errors="ignore").strip())

def main():
    parser = argparse.ArgumentParser(description="XIAO ESP32C3 OLED Media Player CLI")
    parser.add_argument("--port", default="/dev/ttyACM0", help="Serial port (e.g. /dev/ttyACM0 or COM3)")
    sub = parser.add_subparsers(dest="command", required=True)

    enc = sub.add_parser("encode", help="Encode video to .lz4s or .mjps format")
    enc.add_argument("input", help="Input video file (.mp4, .gif, etc.)")
    enc.add_argument("output", help="Output .lz4s or .mjps file")
    enc.add_argument("--fps", type=int, choices=range(1, 61), default=25,
                     metavar="1-60", help="Resample to target FPS (default: 25)")
    enc.add_argument("--codec", choices=("lz4", "mjpeg"), default="lz4",
                     help="Video compression (default: lz4); use .mjps for mjpeg")
    enc.add_argument("--quality", type=int, choices=range(1, 101), default=85,
                     metavar="1-100", help="JPEG quality for --codec mjpeg (default: 85)")
    enc.add_argument(
        "--resize", choices=("fit", "fill", "stretch"), default="fill",
        help="Resize to 128x128: fit adds black bars, fill crops the center "
             "(default), stretch changes proportions",
    )

    sub.add_parser("list", help="List files on LittleFS")

    up = sub.add_parser("upload", help="Upload file with CRC32 verification")
    up.add_argument("local_file", help="Path to local file")
    up.add_argument("--name", help="Remote filename (defaults to basename)")

    dl = sub.add_parser("download", help="Download file with CRC32 verification")
    dl.add_argument("remote_name", help="Remote filename on LittleFS")
    dl.add_argument("--out", help="Local output path")

    rm = sub.add_parser("delete", help="Delete file on LittleFS")
    rm.add_argument("remote_name", help="Remote filename on LittleFS")

    args = parser.parse_args()
    if args.command == "encode":
        encode_video(args.input, args.output, args.fps, args.resize, args.codec, args.quality)
    elif args.command == "list":
        cmd_list(args.port)
    elif args.command == "upload":
        cmd_upload(args.port, args.local_file, args.name)
    elif args.command == "download":
        cmd_download(args.port, args.remote_name, args.out)
    elif args.command == "delete":
        cmd_delete(args.port, args.remote_name)

if __name__ == "__main__":
    main()
