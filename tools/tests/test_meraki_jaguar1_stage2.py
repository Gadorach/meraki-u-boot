import importlib.util
import unittest
from pathlib import Path


TOOL_PATH = Path(__file__).parents[1] / "meraki_jaguar1_stage2.py"
SPEC = importlib.util.spec_from_file_location("meraki_jaguar1_stage2", TOOL_PATH)
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


class Stage2SlotTest(unittest.TestCase):
    BOARD_ID = TOOL.BOARDS["ms42p"]

    def test_round_trip(self):
        payload = bytes(range(256)) * 17
        slot = TOOL.pack_slot(payload, 42, self.BOARD_ID)
        info = TOOL.verify_slot(slot, self.BOARD_ID)

        self.assertEqual(len(slot), TOOL.SLOT_SIZE)
        self.assertEqual(info["generation"], 42)
        self.assertEqual(info["payload_size"], len(payload))
        self.assertEqual(info["board_id"], self.BOARD_ID)
        self.assertEqual(info["load_address"], TOOL.LOAD_ADDRESS)

    def test_rejects_corrupt_header(self):
        slot = bytearray(TOOL.pack_slot(b"stage2", 1, self.BOARD_ID))
        slot[12] ^= 1

        with self.assertRaisesRegex(TOOL.FormatError, "header CRC"):
            TOOL.verify_slot(slot)

    def test_rejects_corrupt_payload(self):
        slot = bytearray(TOOL.pack_slot(b"stage2", 1, self.BOARD_ID))
        slot[TOOL.HEADER_SIZE] ^= 1

        with self.assertRaisesRegex(TOOL.FormatError, "payload CRC"):
            TOOL.verify_slot(slot)

    def test_rejects_non_erased_padding(self):
        slot = bytearray(TOOL.pack_slot(b"stage2", 1, self.BOARD_ID))
        slot[-1] = 0

        with self.assertRaisesRegex(TOOL.FormatError, "padding"):
            TOOL.verify_slot(slot)

    def test_rejects_oversized_payload(self):
        with self.assertRaisesRegex(TOOL.FormatError, "too large"):
            TOOL.pack_slot(bytes(TOOL.PAYLOAD_MAX + 1), 1, self.BOARD_ID)

    def test_rejects_wrong_board(self):
        slot = TOOL.pack_slot(b"stage2", 1, self.BOARD_ID)

        with self.assertRaisesRegex(TOOL.FormatError, "different board"):
            TOOL.verify_slot(slot, TOOL.BOARDS["ms220-48"])


if __name__ == "__main__":
    unittest.main()
