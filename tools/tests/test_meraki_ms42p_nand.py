import re
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[2]
NAND_SOURCE = ROOT / "board/meraki/jaguar1/nand.c"
KCONFIG = ROOT / "board/meraki/jaguar1/Kconfig"
ENV_HEADER = ROOT / "include/configs/vcoreiii.h"
STAGE2_CONFIG = ROOT / "configs/meraki_ms42p_jaguar1_stage2_defconfig"


class Ms42pNandContractTest(unittest.TestCase):
    def test_wiring_geometry_ecc_and_partition_contract(self):
        source = NAND_SOURCE.read_text()
        expected_defines = {
            "MS42P_NAND_BASE": "0x54000000",
            "MS42P_NAND_ALE": "BIT(13)",
            "MS42P_NAND_CLE": "BIT(14)",
            "MS42P_NAND_CS": "1",
            "MS42P_NAND_CS_GPIO": "19",
            "MS42P_NAND_GPIO_MODE_PIN": "20",
            "MS42P_NAND_TLB_INDEX": "6",
            "MS42P_NAND_SIZE": "SZ_128M",
            "MS42P_NAND_PAGE_SIZE": "SZ_2K",
            "MS42P_NAND_OOB_SIZE": "64",
            "MS42P_NAND_ERASE_SIZE": "SZ_128K",
            "MS42P_NAND_ECC_STEP": "512",
            "MS42P_NAND_ECC_STRENGTH": "4",
        }

        for name, value in expected_defines.items():
            self.assertRegex(source, rf"#define\s+{name}\s+{re.escape(value)}")

        self.assertIn("mscc_gpio_set_alternate(MS42P_NAND_CS_GPIO, 1)", source)
        self.assertIn("mscc_gpio_set_alternate(MS42P_NAND_GPIO_MODE_PIN, 0)", source)
        self.assertIn("ICPU_PI_MST_CFG_CLK_DIV(2)", source)
        self.assertIn("ICPU_PI_MST_CTRL_WAITCC(16)", source)
        self.assertIn("create_tlb(MS42P_NAND_TLB_INDEX", source)
        self.assertIn("MMU_REGIO_RW, MMU_REGIO_RW", source)
        self.assertIn("chip->options |= NAND_NO_SUBPAGE_WRITE", source)
        self.assertIn("chip->options &= ~NAND_SUBPAGE_READ", source)
        self.assertLess(
            source.index("ret = nand_scan(mtd, 1)"),
            source.index("chip->options &= ~NAND_SUBPAGE_READ"),
        )
        self.assertLess(
            source.index("chip->options &= ~NAND_SUBPAGE_READ"),
            source.index("ret = nand_register(0, mtd)"),
        )
        self.assertIn("chip->ecc.mode = NAND_ECC_SOFT_BCH", source)
        self.assertNotIn("chip->ecc.layout", source)
        self.assertNotIn("add_mtd_partitions", source)

    def test_nand_is_scoped_to_ms42p_stage2(self):
        kconfig = KCONFIG.read_text()
        stage2 = STAGE2_CONFIG.read_text()

        self.assertIn(
            "default y if TARGET_MERAKI_MS42P && MERAKI_JAGUAR1_STAGE2",
            kconfig,
        )
        for symbol in ("BCH", "MTD", "MTD_PARTITIONS", "MTD_RAW_NAND",
                       "NAND_ECC_BCH", "SYS_NAND_SELF_INIT"):
            self.assertRegex(kconfig, rf"select {symbol}\b")

        for option in ("CONFIG_CMD_MTD=y", "CONFIG_CMD_NAND=y",
                       "CONFIG_CMD_UBI=y", "CONFIG_MTD_RAW_NAND=y"):
            self.assertIn(option, stage2)
        self.assertIn('CONFIG_MTDIDS_DEFAULT="nand0=nand0"', stage2)
        self.assertIn(
            'CONFIG_MTDPARTS_DEFAULT="mtdparts=nand0:-(ubi)"', stage2
        )
        self.assertIn("# CONFIG_CMD_UBIFS is not set", stage2)

        excluded = [ROOT / "configs/meraki_ms42p_jaguar1_defconfig"]
        excluded.extend(ROOT.glob("configs/meraki_ms*_jaguar1_stage2_defconfig"))
        for config in excluded:
            if config == STAGE2_CONFIG:
                continue
            text = config.read_text()
            self.assertNotIn("CONFIG_CMD_NAND=y", text, config.name)
            self.assertNotIn("CONFIG_CMD_UBI=y", text, config.name)
            self.assertNotIn("CONFIG_MTD_RAW_NAND=y", text, config.name)

    def test_production_chain_and_recovery_isolation(self):
        env = ENV_HEADER.read_text()
        production = env[env.index('"nand_boot='):env.index('"recovery_requested=')]
        recovery = env[env.index('"nor_recovery='):env.index('"nand_boot=')]
        expected_order = [
            "ubi part ubi",
            "ubi check kernel",
            "ubi check rootfs",
            "ubi read ${fit_addr_r} kernel",
            "iminfo ${fit_addr_r}",
            "run setup_production",
            "bootm ${fit_addr_r}",
        ]

        positions = [production.index(command) for command in expected_order]
        self.assertEqual(positions, sorted(positions))
        self.assertNotIn("ubi.", recovery)
        self.assertNotIn("root=/dev/ubi", recovery)
        self.assertIn("run setup_recovery", recovery)

        config = STAGE2_CONFIG.read_text()
        self.assertIn("run nand_boot || run nor_recovery", config)


if __name__ == "__main__":
    unittest.main()
