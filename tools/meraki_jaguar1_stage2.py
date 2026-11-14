#!/usr/bin/env python3
"""Pack and verify redundant Meraki Jaguar1 stage2 NOR slots."""

import argparse
import struct
import sys
import zlib
from pathlib import Path


MAGIC = 0x5332534D
VERSION = 2
HEADER_SIZE = 0x1000
SLOT_SIZE = 0xC0000
PAYLOAD_MAX = SLOT_SIZE - HEADER_SIZE
LOAD_ADDRESS = 0x81000000
ENTRY_ADDRESS = 0x81000000
BOARDS = {
    "ms42p": 0x00000001,
    "ms220-48": 0x00000002,
    "ms320-24": 0x00000003,
    "ms320-48": 0x00000004,
}
BOARD_NAMES = {value: name for name, value in BOARDS.items()}
FIXED_FORMAT = "<IHHQIIIIIII"
FIXED_SIZE = struct.calcsize(FIXED_FORMAT)
HEADER_CRC_OFFSET = 36


class FormatError(ValueError):
    pass


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def make_header(payload, generation, board_id):
    if not payload:
        raise FormatError("payload is empty")
    if len(payload) > PAYLOAD_MAX:
        raise FormatError(
            f"payload is too large: {len(payload)} > {PAYLOAD_MAX}"
        )
    if not 0 <= generation <= 0xFFFFFFFFFFFFFFFF:
        raise FormatError("generation is outside the uint64 range")
    if board_id not in BOARD_NAMES:
        raise FormatError(f"unsupported board ID: 0x{board_id:08x}")

    fixed = struct.pack(
        FIXED_FORMAT,
        MAGIC,
        VERSION,
        HEADER_SIZE,
        generation,
        len(payload),
        LOAD_ADDRESS,
        ENTRY_ADDRESS,
        board_id,
        crc32(payload),
        0,
        0,
    )
    header = bytearray(fixed + bytes(HEADER_SIZE - FIXED_SIZE))
    struct.pack_into("<I", header, HEADER_CRC_OFFSET, crc32(header))
    return bytes(header)


def pack_slot(payload, generation, board_id):
    header = make_header(payload, generation, board_id)
    return header + payload + bytes([0xFF]) * (SLOT_SIZE - len(header) - len(payload))


def verify_slot(slot, expected_board_id=None):
    if len(slot) != SLOT_SIZE:
        raise FormatError(f"slot size is {len(slot)}, expected {SLOT_SIZE}")

    fields = struct.unpack_from(FIXED_FORMAT, slot)
    (
        magic,
        version,
        header_size,
        generation,
        payload_size,
        load_address,
        entry_address,
        board_id,
        payload_crc,
        header_crc,
        flags,
    ) = fields
    if magic != MAGIC:
        raise FormatError("invalid magic")
    if version != VERSION or header_size != HEADER_SIZE:
        raise FormatError("unsupported header version or size")
    if not 0 < payload_size <= PAYLOAD_MAX:
        raise FormatError("invalid payload size")
    if load_address != LOAD_ADDRESS or entry_address != ENTRY_ADDRESS:
        raise FormatError("invalid load or entry address")
    if board_id not in BOARD_NAMES:
        raise FormatError(f"unsupported board ID: 0x{board_id:08x}")
    if expected_board_id is not None and board_id != expected_board_id:
        raise FormatError("slot is for a different board")
    if flags:
        raise FormatError("unsupported flags")
    if any(slot[FIXED_SIZE:HEADER_SIZE]):
        raise FormatError("reserved header bytes are not zero")

    header = bytearray(slot[:HEADER_SIZE])
    struct.pack_into("<I", header, HEADER_CRC_OFFSET, 0)
    if crc32(header) != header_crc:
        raise FormatError("header CRC mismatch")

    payload = slot[HEADER_SIZE:HEADER_SIZE + payload_size]
    if crc32(payload) != payload_crc:
        raise FormatError("payload CRC mismatch")
    if any(byte != 0xFF for byte in slot[HEADER_SIZE + payload_size:]):
        raise FormatError("slot padding is not erased")

    return {
        "generation": generation,
        "payload_size": payload_size,
        "board_id": board_id,
        "payload_crc32": payload_crc,
        "header_crc32": header_crc,
        "load_address": load_address,
        "entry_address": entry_address,
    }


def command_pack(args):
    payload = args.input.read_bytes()
    board_id = BOARDS[args.board]
    args.output.write_bytes(pack_slot(payload, args.generation, board_id))
    info = verify_slot(args.output.read_bytes(), board_id)
    print_info(args.output, info)


def command_verify(args):
    expected_board_id = BOARDS[args.board] if args.board else None
    info = verify_slot(args.input.read_bytes(), expected_board_id)
    print_info(args.input, info)


def print_info(path, info):
    print(f"file: {path}")
    print(f"generation: {info['generation']}")
    print(f"board: {BOARD_NAMES[info['board_id']]}")
    print(f"payload-size: {info['payload_size']}")
    print(f"load-address: 0x{info['load_address']:08x}")
    print(f"entry-address: 0x{info['entry_address']:08x}")
    print(f"payload-crc32: {info['payload_crc32']:08x}")
    print(f"header-crc32: {info['header_crc32']:08x}")


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(required=True)

    pack_parser = subparsers.add_parser("pack")
    pack_parser.add_argument("input", type=Path)
    pack_parser.add_argument("output", type=Path)
    pack_parser.add_argument("--generation", required=True, type=int)
    pack_parser.add_argument("--board", required=True, choices=BOARDS)
    pack_parser.set_defaults(function=command_pack)

    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("input", type=Path)
    verify_parser.add_argument("--board", choices=BOARDS)
    verify_parser.set_defaults(function=command_verify)

    args = parser.parse_args()
    try:
        args.function(args)
    except (FormatError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
