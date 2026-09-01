# SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import asyncio
import io
import os
import struct
import sys
import time

try:
    from bleak import BleakClient, BleakScanner
except ImportError as exc:
    raise SystemExit("Missing dependency 'bleak'. Install it with: pip install bleak pillow") from exc

SERIAL_TRAN_DATA_UUID = "00000000-0000-0200-6473-5f696c666973"
DEVICE_NAME = "SIFLI_SCREENCAST"
CATE_ID = 0x20
SC_FRAME_MAGIC = 0x53C1
SC_FRAME_TYPE_JPEG = 0
MAX_FRAME_SIZE = 0xFFFF
MIN_ATT_MTU = 7


def load_pillow():
    try:
        from PIL import Image
    except ImportError as exc:
        raise SystemExit("Missing dependency 'pillow'. Install it with: pip install bleak pillow") from exc
    return Image


def build_frame(jpeg_bytes: bytes) -> bytes:
    """Prepend the screen-cast header to a JPEG payload."""
    header = struct.pack("<HBB", SC_FRAME_MAGIC, SC_FRAME_TYPE_JPEG, 0)
    frame = header + jpeg_bytes
    if len(frame) > MAX_FRAME_SIZE:
        raise ValueError(
            f"frame is {len(frame)} bytes, exceeding the {MAX_FRAME_SIZE}-byte transport limit; "
            "reduce --quality or --width"
        )
    return frame


def fragment(frame: bytes, mtu: int):
    """Yield serial-transmission packets for one frame."""
    if mtu < MIN_ATT_MTU:
        raise ValueError(f"ATT MTU must be at least {MIN_ATT_MTU}, got {mtu}")
    if not frame or len(frame) > MAX_FRAME_SIZE:
        raise ValueError(f"frame size must be between 1 and {MAX_FRAME_SIZE} bytes")

    max_payload = mtu - 3
    total = len(frame)

    if total <= max_payload - 4:
        yield bytes([CATE_ID, 0]) + struct.pack("<H", total) + frame
        return

    first_chunk = max_payload - 4
    yield bytes([CATE_ID, 1]) + struct.pack("<H", total) + frame[:first_chunk]
    offset = first_chunk

    continuation_chunk = max_payload - 2
    while offset < total:
        remaining = total - offset
        if remaining <= continuation_chunk:
            yield bytes([CATE_ID, 3]) + frame[offset:]
            offset = total
        else:
            yield bytes([CATE_ID, 2]) + frame[offset:offset + continuation_chunk]
            offset += continuation_chunk


def encode_jpeg(image, width, quality):
    if width:
        height = int(image.height * width / image.width)
        image = image.resize((width, height))
    buffer = io.BytesIO()
    image.save(buffer, format="JPEG", quality=quality)
    return buffer.getvalue()


def iter_folder_images(path, width, quality):
    Image = load_pillow()
    try:
        files = sorted(
            name for name in os.listdir(path)
            if name.lower().endswith((".jpg", ".jpeg", ".png", ".bmp"))
        )
    except OSError as exc:
        raise SystemExit(f"cannot read image folder '{path}': {exc}") from exc

    if not files:
        raise SystemExit(f"no images found in {path}")

    while True:
        for name in files:
            with Image.open(os.path.join(path, name)) as source:
                image = source.convert("RGB")
                yield encode_jpeg(image, width, quality)


def list_gpus():
    """Print available dxcam GPUs and outputs."""
    try:
        import dxcam
    except ImportError as exc:
        raise SystemExit(
            "Missing screen-capture dependencies. Install them with: "
            "pip install bleak pillow dxcam numpy mss"
        ) from exc

    print("GPUs (device_idx):")
    print(dxcam.device_info())
    print("\nOutputs (output_idx, per GPU):")
    print(dxcam.output_info())
    print("\nIf accelerated windows are black, try another --gpu index.")


def create_dxcam(device_idx, output_idx):
    import dxcam

    options = {"output_color": "RGB"}
    if device_idx is not None:
        options["device_idx"] = device_idx
    if output_idx is not None:
        options["output_idx"] = output_idx
    camera = dxcam.create(**options)
    if camera is None:
        raise RuntimeError("dxcam.create returned None; check --gpu and --output")
    return camera


def iter_screen(width, quality, device_idx=None, output_idx=None):
    Image = load_pillow()
    try:
        camera = create_dxcam(device_idx, output_idx)
        print(f"screen capture: dxcam device_idx={device_idx} output_idx={output_idx}")
    except ImportError as exc:
        raise SystemExit(
            "Missing screen-capture dependencies. Install them with: "
            "pip install bleak pillow dxcam numpy mss"
        ) from exc
    except Exception as exc:
        print(
            f"dxcam init failed ({exc}), falling back to mss. "
            "Accelerated windows may appear black."
        )
        camera = None

    if camera is not None:
        last_frame = None
        while True:
            try:
                frame = camera.grab()
            except Exception as exc:
                print(f"dxcam grab error ({exc}), retrying")
                frame = None
            if frame is None:
                if last_frame is None:
                    time.sleep(0.01)
                    continue
                frame = last_frame
            last_frame = frame
            yield encode_jpeg(Image.fromarray(frame), width, quality)

    try:
        import mss
    except ImportError as exc:
        raise SystemExit(
            "Missing dependency 'mss'. Install screen-capture dependencies with: "
            "pip install bleak pillow dxcam numpy mss"
        ) from exc

    with mss.mss() as capture:
        monitor = capture.monitors[1]
        while True:
            shot = capture.grab(monitor)
            image = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
            yield encode_jpeg(image, width, quality)


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", choices=["folder", "screen"], default="folder")
    parser.add_argument("--path", default="./imgs", help="image folder for --source folder")
    parser.add_argument("--fps", type=float, default=5.0)
    parser.add_argument("--width", type=int, default=800, help="resize width (0 = keep)")
    parser.add_argument("--quality", type=int, default=60, help="JPEG quality 1..95")
    parser.add_argument("--mtu", type=int, default=247, help="fallback ATT MTU")
    parser.add_argument("--address", help="device BLE address (skip scan)")
    parser.add_argument("--gpu", type=int, help="dxcam GPU device index")
    parser.add_argument("--output", type=int, help="dxcam output index")
    parser.add_argument("--list-gpu", action="store_true", help="list dxcam GPUs and outputs")
    args = parser.parse_args()

    if args.fps < 0:
        parser.error("--fps must be non-negative")
    if args.width < 0:
        parser.error("--width must be non-negative")
    if not 1 <= args.quality <= 95:
        parser.error("--quality must be between 1 and 95")
    if args.mtu < MIN_ATT_MTU:
        parser.error(f"--mtu must be at least {MIN_ATT_MTU}")
    return args


async def run(args):
    if args.list_gpu:
        list_gpus()
        return

    if args.address:
        address = args.address
    else:
        print(f"scanning for {DEVICE_NAME} ...")
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
        if not device:
            raise SystemExit("device not found")
        address = device.address
        print(f"found {address}")

    if args.source == "folder":
        frames = iter_folder_images(args.path, args.width, args.quality)
    else:
        frames = iter_screen(args.width, args.quality, args.gpu, args.output)

    period = 1.0 / args.fps if args.fps > 0 else 0
    async with BleakClient(address) as client:
        print("connected")
        mtu = getattr(client, "mtu_size", 0) or args.mtu
        if mtu < MIN_ATT_MTU:
            raise RuntimeError(f"negotiated ATT MTU is too small: {mtu}")
        print(f"using MTU {mtu}")

        for jpeg in frames:
            start = time.time()
            frame = build_frame(jpeg)
            packets = list(fragment(frame, mtu))
            last_packet = len(packets) - 1
            for index, packet in enumerate(packets):
                await client.write_gatt_char(
                    SERIAL_TRAN_DATA_UUID,
                    packet,
                    response=(index == last_packet),
                )
            elapsed = max(time.time() - start, 1e-6)
            print(
                f"sent frame {len(jpeg)} B in {elapsed * 1000:.0f} ms "
                f"({len(jpeg) * 8 / elapsed / 1000:.0f} kbps)"
            )
            if period > elapsed:
                await asyncio.sleep(period - elapsed)


def main():
    args = parse_arguments()
    asyncio.run(run(args))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped")
    except (ValueError, RuntimeError) as exc:
        raise SystemExit(f"error: {exc}") from exc
