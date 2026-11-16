// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8150 GCC registers used by UFS, transcribed as facts from the public
// Linux drivers/clk/qcom/gcc-sm8150.c clock driver (nabu / Xiaomi Pad 5).
// Interface kept byte-identical to the sheng (SM8550) GccUfs.h so that
// NativeUfs.cpp needs no change; only the offsets are SM8150's.
//
// Offsets verified 2026-10-20 from gcc-sm8150.c:
//   ufs_phy_gdsc.gdscr                  = 0x77004   (same as SM8550)
//   gcc_ufs_phy_axi_clk                 = 0x77010
//   gcc_ufs_phy_ahb_clk                 = 0x77014
//   gcc_ufs_phy_tx_symbol_0_clk         = 0x77018
//   gcc_ufs_phy_rx_symbol_0_clk         = 0x7701c
//   gcc_ufs_phy_unipro_core_clk         = 0x77058
//   gcc_ufs_phy_ice_core_clk            = 0x7705c
//   gcc_ufs_phy_phy_aux_clk             = 0x77090
//   gcc_ufs_phy_rx_symbol_1_clk         = 0x770ac
//   gcc_aggre_ufs_phy_axi_clk           = 0x770c0
//   gcc_ufs_mem_clkref_clk              = 0x8c000
//
// No write is performed by NativeUfs yet: these are read-only observations
// (the reset and RPMh/TCSR/QMP-PHY programming are not represented).
#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H

// Physical windows from the nabu board DT (clock-controller@100000, ufshc@1d84000).
#define GCC_BASE                      0x00100000ULL
#define UFS_HC_BASE                   0x01d84000ULL
#define UFS_HC_SIZE                   0x3000ULL

#define GCC_UFS_PHY_GDSCR             0x77004
#define GCC_UFS_MEM_PHY_GDSCR         0x77004

#define GCC_UFS_PHY_AXI_ENABLE        0x77010
#define GCC_UFS_PHY_AHB_ENABLE        0x77014
#define GCC_UFS_PHY_TX_SYMBOL0_ENABLE 0x77018
#define GCC_UFS_PHY_RX_SYMBOL0_ENABLE 0x7701c
#define GCC_UFS_PHY_UNIPRO_CORE_ENABLE 0x77058
#define GCC_UFS_PHY_ICE_CORE_ENABLE   0x7705c
#define GCC_UFS_PHY_PHY_AUX_ENABLE    0x77090
#define GCC_UFS_PHY_RX_SYMBOL1_ENABLE 0x770ac
// bus_aggr_clk in the board DT: gcc_aggre_ufs_phy_axi_clk, halt/enable 0x770c0, HW clock gating bit1
#define GCC_AGGRE_UFS_PHY_AXI_ENABLE  0x770c0

#define GCC_UFS_BRANCH_ENABLE_BIT     0x1U

// gcc_ufs_phy_ahb_clk and gcc_aggre_ufs_phy_axi_clk set hwcg_bit=1 in gcc-sm8150.c, so CLK_OFF=1 on
// an idle branch is normal (hardware read: not yet observed on nabu).
// clkref: gcc_ufs_mem_clkref_clk halt/enable 0x8c000.
// The reset and RPMh/TCSR/QMP-PHY programming are not represented here: their
// semantics are not qualified and NativeUfs must not write them speculatively.
#endif
