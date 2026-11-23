"""Pack a compatible ESP-IDF build for manual LAN firmware updates."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import tempfile


BOARD_LAYOUT = b"dual-ml307a-s3-ab1"
BOARD_VARIANT = b"8mb-octal-psram"
TARGET = "esp32s3"
PROJECT = "newsmsforward"
APP_BIN = f"{PROJECT}.bin"
SLOT_SIZE = 0x1E0000
UPDATE_NAME = "esp32-smsforward-s3-ml307a.smsfw"

WRAPPER = struct.Struct("<8s32sI32s4x")
BOARD_DESCRIPTOR = struct.pack("<8s32s32s", b"SMSFBRD1", BOARD_LAYOUT, BOARD_VARIANT)
# ESP image header, first segment header, then the 256-byte ESP app descriptor.
APP_DESCRIPTOR_OFFSET = 24 + 8
BOARD_DESCRIPTOR_OFFSET = APP_DESCRIPTOR_OFFSET + 256

PARTITION_ENTRY = struct.Struct("<HBBII16sI")
PARTITIONS = {
    "nvs": (1, 2, 0x9000, 0x6000, 0),
    "otadata": (1, 0, 0xF000, 0x2000, 0),
    "phy_init": (1, 1, 0x11000, 0x1000, 0),
    "ota_0": (0, 0x10, 0x20000, SLOT_SIZE, 0),
    "storage": (1, 0x81, 0x200000, 0x400000, 0),
    "ota_1": (0, 0x11, 0x600000, SLOT_SIZE, 0),
}


def check_partition_table(table):
    entries = {}
    if len(table) != 0xC00:
        raise ValueError("Expected a complete ESP-IDF partition table binary")
    for offset in range(0, len(table), PARTITION_ENTRY.size):
        entry = table[offset:offset + PARTITION_ENTRY.size]
        if entry[:16] == b"\xeb\xeb" + b"\xff" * 14:
            expected = hashlib.md5(table[:offset], usedforsecurity=False).digest()
            if entry[16:] != expected or any(byte != 0xFF for byte in table[offset + 32:]):
                raise ValueError("Partition table checksum or padding failed verification")
            if entries != PARTITIONS:
                raise ValueError("Partition table does not match the dual ML307A A/B layout")
            return
        magic, kind, subtype, address, size, label, flags = PARTITION_ENTRY.unpack(entry)
        if magic != 0x50AA:
            break
        name = label.split(b"\0", 1)[0].decode("ascii")
        if name in entries:
            raise ValueError(f"Duplicate partition {name}")
        entries[name] = (kind, subtype, address, size, flags)
    raise ValueError("Partition table lacks a valid MD5 record")


def build_update(build):
    build = build.resolve()
    metadata = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    if (
        metadata["project_name"] != PROJECT
        or metadata["target"] != TARGET
        or metadata["app_bin"] != APP_BIN
        or Path(metadata["build_dir"]).resolve() != build
    ):
        raise ValueError(f"Expected the original ESP32-S3 {PROJECT} build directory")
    version = metadata["project_version"]
    if (
        not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version)
        or tuple(map(int, version.split("."))) < (1, 3, 0)
    ):
        raise ValueError("Expected a release version of 1.3.0 or later")

    config = json.loads((build / "config/sdkconfig.json").read_text(encoding="utf-8"))
    if config["ESPTOOLPY_FLASHSIZE"] != "8MB" or config["SPIRAM_MODE_OCT"] is not True:
        raise ValueError("Expected 8 MB Flash and octal PSRAM build configuration")
    if config["PARTITION_TABLE_OFFSET"] != 0x8000 or config["BOOTLOADER_APP_ROLLBACK_ENABLE"] is not True:
        raise ValueError("Expected the dual ML307A partition table offset and bootloader rollback")
    check_partition_table((build / "partition_table/partition-table.bin").read_bytes())

    app = (build / APP_BIN).read_bytes()
    board_end = BOARD_DESCRIPTOR_OFFSET + len(BOARD_DESCRIPTOR)
    if (
        not board_end + 32 <= len(app) <= SLOT_SIZE
        or app[0] != 0xE9
        or app[3] >> 4 != 3  # ESP image flash size code for 8 MB.
        or struct.unpack_from("<H", app, 12)[0] != 9  # ESP32-S3 image chip ID.
        or app[23] != 1  # ESP image has an appended SHA-256 digest.
        or struct.unpack_from("<I", app, 28)[0] < board_end - APP_DESCRIPTOR_OFFSET
        or app[APP_DESCRIPTOR_OFFSET:APP_DESCRIPTOR_OFFSET + 4] != b"\x32\x54\xcd\xab"
        or app[48:80].split(b"\0", 1)[0] != version.encode("ascii")
        or app[80:112].split(b"\0", 1)[0] != PROJECT.encode("ascii")
        or app[BOARD_DESCRIPTOR_OFFSET:board_end] != BOARD_DESCRIPTOR
        or hashlib.sha256(app[:-32]).digest() != app[-32:]
    ):
        raise ValueError("Application image has an incompatible board, version, size, or digest")

    return WRAPPER.pack(b"SMSFUPD1", BOARD_LAYOUT, len(app), hashlib.sha256(app).digest()) + app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="original ESP-IDF build directory")
    parser.add_argument("output", type=Path, nargs="?", default=Path(UPDATE_NAME))
    args = parser.parse_args()
    if args.output.suffix != ".smsfw":
        parser.error("output path must end in .smsfw")
    try:
        update = build_update(args.build)
        temp_path = None
        try:
            with tempfile.NamedTemporaryFile(dir=args.output.parent, delete=False) as partial:
                temp_path = Path(partial.name)
                partial.write(update)
            temp_path.replace(args.output)
        finally:
            if temp_path is not None:
                temp_path.unlink(missing_ok=True)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    print(f"{args.output} ({len(update)} bytes)")


if __name__ == "__main__":
    main()
