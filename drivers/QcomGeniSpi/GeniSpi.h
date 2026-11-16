// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm GENI serial-engine SPI master, FIFO (PIO) mode. Register layout and sequences follow the study of the
// Linux spi-geni-qcom / qcom-geni-se drivers (no code copied). No board addresses, clocks or pins here.
#ifndef GENI_SPI_H
#define GENI_SPI_H
#include <stdint.h>
#include <stddef.h>

// Serial-engine registers (offsets from the SE base).
#define GENI_FORCE_DEFAULT_REG 0x20
#define GENI_OUTPUT_CTRL 0x24
#define SE_GENI_CGC_CTRL 0x28
#define SE_GENI_STATUS 0x40
#define GENI_SER_M_CLK_CFG 0x48
#define GENI_IF_DISABLE_RO 0x64
#define GENI_FW_REVISION_RO 0x68
#define SE_GENI_CLK_SEL 0x7c
#define SE_GENI_BYTE_GRAN 0x254
#define SE_GENI_DMA_MODE_EN 0x258
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_SPI_CPHA 0x224
#define SE_SPI_LOOPBACK 0x22c
#define SE_SPI_CPOL 0x230
#define SE_SPI_DEMUX_OUTPUT_INV 0x24c
#define SE_SPI_DEMUX_SEL 0x250
#define SE_SPI_TRANS_CFG 0x25c
#define SE_SPI_WORD_LEN 0x268
#define SE_SPI_TX_TRANS_LEN 0x26c
#define SE_SPI_RX_TRANS_LEN 0x270
#define SE_GENI_M_CMD0 0x600
#define SE_GENI_M_CMD_CTRL_REG 0x604
#define SE_GENI_M_IRQ_STATUS 0x610
#define SE_GENI_M_IRQ_EN 0x614
#define SE_GENI_M_IRQ_CLEAR 0x618
#define SE_GENI_S_IRQ_EN 0x644
#define SE_GENI_S_IRQ_CLEAR 0x648
#define SE_DMA_TX_IRQ_CLR 0xc44
#define SE_DMA_RX_IRQ_CLR 0xd44
#define SE_GENI_TX_FIFOn 0x700
#define SE_GENI_RX_FIFOn 0x780
#define SE_GENI_TX_FIFO_STATUS 0x800
#define SE_GENI_RX_FIFO_STATUS 0x804
#define SE_GENI_TX_WATERMARK_REG 0x80c
#define SE_GENI_RX_WATERMARK_REG 0x810
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814
#define SE_IRQ_EN 0xe1c
#define SE_HW_PARAM_0 0xe24
#define SE_DMA_GENERAL_CFG 0xe30
#define SE_GSI_EVENT_EN 0xe18
// Serial-engine DMA registers (qcom-geni-se.c / geni-se.h of the pinned mainline tree).
#define SE_DMA_TX_PTR_L 0xc30
#define SE_DMA_TX_PTR_H 0xc34
#define SE_DMA_TX_ATTR 0xc38
#define SE_DMA_TX_LEN 0xc3c
#define SE_DMA_TX_IRQ_STAT 0xc40
#define SE_DMA_TX_IRQ_EN_SET 0xc4c
#define SE_DMA_TX_LEN_IN 0xc54
#define SE_DMA_TX_FSM_RST 0xc58
#define SE_DMA_RX_PTR_L 0xd30
#define SE_DMA_RX_PTR_H 0xd34
#define SE_DMA_RX_ATTR 0xd38
#define SE_DMA_RX_LEN 0xd3c
#define SE_DMA_RX_IRQ_STAT 0xd40
#define SE_DMA_RX_IRQ_EN_SET 0xd4c
#define SE_DMA_RX_LEN_IN 0xd54
#define SE_DMA_RX_FSM_RST 0xd58
// SE_DMA_{TX,RX}_IRQ_STAT / _EN_SET fields
#define GENI_DMA_DONE (1u<<0)
#define GENI_DMA_EOT (1u<<1)
#define GENI_DMA_AHB_ERR (1u<<2)        // TX_SBE / RX_SBE
#define GENI_DMA_RESET_DONE (1u<<3)
#define GENI_DMA_TX_ATTR_EOT_BUF (1u<<0)
// Wrapper register (offset from the QUP wrapper base).
#define QUPV3_HW_VER_REG 0x4

#define GENI_PROTO_NONE 0
#define GENI_PROTO_SPI 1
#define GENI_PROTO_INVALID 255

#define GENI_M_CMD_TX_ONLY 1
#define GENI_M_CMD_RX_ONLY 2
#define GENI_M_CMD_TX_RX 3              // full duplex: spi-geni-qcom.c issues SPI_TX_ONLY|SPI_RX_ONLY (its SPI_TX_RX=7 is never used)
#define GENI_M_PARAM_FRAGMENTATION (1u<<2)

// Register field values.
#define GENI_FORCE_DEFAULT (1u<<0)
#define GENI_SER_CLK_EN (1u<<0)
#define GENI_CLK_DIV_SHIFT 4
#define GENI_CLK_DIV_MAX 0xfffu
#define GENI_FIFO_IF_DISABLE (1u<<0)
#define GENI_FW_PROTO_SHIFT 8
#define GENI_DMA_MODE_EN (1u<<0)
#define GENI_DEFAULT_CGC_EN 0x7fu
#define GENI_DEFAULT_IO_OUTPUT_CTRL 0x7fu
#define GENI_M_OPCODE_SHIFT 27
#define GENI_M_PARAMS_MASK 0x7ffffffu
#define GENI_M_CMD_CANCEL (1u<<2)
#define GENI_M_CMD_ABORT (1u<<1)
#define GENI_STATUS_M_ACTIVE (1u<<0)
#define GENI_TRANS_LEN_MASK 0xffffffu
#define GENI_WORD_LEN_MIN 4
#define GENI_CS_TOGGLE (1u<<1)
#define GENI_CPHA_BIT (1u<<0)
#define GENI_CPOL_BIT (1u<<2)
#define GENI_LOOPBACK_ENABLE 1u
// SE_IRQ_EN
#define GENI_IRQ_DMA_RX (1u<<0)
#define GENI_IRQ_DMA_TX (1u<<1)
#define GENI_IRQ_M (1u<<2)
#define GENI_IRQ_S (1u<<3)
// SE_DMA_GENERAL_CFG
#define GENI_DMA_RX_CGC_ON (1u<<0)
#define GENI_DMA_TX_CGC_ON (1u<<1)
#define GENI_DMA_AHB_SLV_CGC_ON (1u<<2)
#define GENI_AHB_SEC_SLV_CGC_ON (1u<<3)
// M_IRQ bits
#define GENI_M_CMD_DONE (1u<<0)
#define GENI_M_CMD_OVERRUN (1u<<1)
#define GENI_M_ILLEGAL_CMD (1u<<2)
#define GENI_M_CMD_FAILURE (1u<<3)
#define GENI_M_CMD_CANCEL_DONE (1u<<4)
#define GENI_M_CMD_ABORT_DONE (1u<<5)
#define GENI_M_IO_DATA_DEASSERT (1u<<22)
#define GENI_M_IO_DATA_ASSERT (1u<<23)
#define GENI_M_RX_FIFO_RD_ERR (1u<<24)
#define GENI_M_RX_FIFO_WR_ERR (1u<<25)
#define GENI_M_RX_FIFO_WATERMARK (1u<<26)
#define GENI_M_RX_FIFO_LAST (1u<<27)
#define GENI_M_TX_FIFO_RD_ERR (1u<<28)
#define GENI_M_TX_FIFO_WR_ERR (1u<<29)
#define GENI_M_TX_FIFO_WATERMARK (1u<<30)
#define GENI_M_COMMON_IRQ_EN (0x7eu | GENI_M_IO_DATA_DEASSERT | GENI_M_IO_DATA_ASSERT | GENI_M_RX_FIFO_RD_ERR | \
                              GENI_M_RX_FIFO_WR_ERR | GENI_M_TX_FIFO_RD_ERR | GENI_M_TX_FIFO_WR_ERR)
#define GENI_S_COMMON_IRQ_EN (0x3eu | 0x3e00u | (1u<<24) | (1u<<25))
#define GENI_M_ERRORS (GENI_M_CMD_OVERRUN | GENI_M_ILLEGAL_CMD | GENI_M_CMD_FAILURE | GENI_M_RX_FIFO_RD_ERR | \
                       GENI_M_RX_FIFO_WR_ERR | GENI_M_TX_FIFO_RD_ERR | GENI_M_TX_FIFO_WR_ERR)
// RX_FIFO_STATUS
#define GENI_RX_LAST (1u<<31)
#define GENI_RX_LAST_VALID_SHIFT 28
#define GENI_RX_LAST_VALID_MASK 7u
#define GENI_RX_WC_MASK 0x1ffffffu
// Packing vector fields
#define GENI_PACK_START_SHIFT 5
#define GENI_PACK_DIR_SHIFT 4
#define GENI_PACK_LEN_SHIFT 1
#define GENI_PACK_STOP (1u<<0)
#define GENI_PACK_VECTOR_SHIFT 10
#define GENI_PACK_VECTORS 4

#define GENI_SPI_CPHA 0x01
#define GENI_SPI_CPOL 0x02
#define GENI_SPI_CS_HIGH 0x04
#define GENI_SPI_LSB_FIRST 0x08
#define GENI_SPI_LOOP 0x20

#define GENI_ERR_PARAM (-22)
#define GENI_ERR_PROTO (-71)
#define GENI_ERR_NODEV (-19)
#define GENI_ERR_TIMEOUT (-110)
#define GENI_ERR_IO (-5)
#define GENI_ERR_NOTSUP (-95)

struct geni_io {
    void *ctx;
    uint32_t (*rd)(void *ctx, uint32_t reg);
    void (*wr)(void *ctx, uint32_t reg, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);
};

struct geni_spi_info {
    uint32_t fw_revision, protocol, if_disable, hw_param0, geni_status;
    uint32_t qup_hw_version;
    unsigned tx_fifo_depth, fifo_width_bits;
    int fifo_disabled;
};

struct geni_spi {
    struct geni_io se;                  // serial engine registers
    struct geni_io wrapper;             // QUP wrapper registers (hardware version only)
    struct geni_spi_info info;
    unsigned oversampling, tx_wm;
    unsigned cur_bpw;
    unsigned cur_speed_hz;
    uint32_t last_mode;
    int last_cs;
    unsigned poll_limit;                // iterations of the completion poll before cancel/abort
    int poll_only;                      // 1: switch the engine interrupt outputs off after init (completion polled); 2: leave them on
    uint32_t snap_irq, snap_status, snap_txfs, snap_rxfs;  // registers read just before a timed-out transfer is cancelled
    uint32_t snap_dma_tx, snap_dma_rx, snap_dma_txin, snap_dma_rxin;
    int snap_valid;
    uint32_t last_tx_in, last_rx_in;                  // SE_DMA_{TX,RX}_LEN_IN after the latest DMA transfer: bytes the engine moved
    // l535: counters taken INSIDE the transfer, where they still mean something. The lesson from
    // l534 on hardware: reading M_IRQ_STATUS or RX_FIFO_STATUS after geni_spi_transfer() returns can
    // only ever give zero, because the completion poll below writes SE_GENI_M_IRQ_CLEAR with the very
    // status word it consumed, and handle_rx() drains the RX FIFO into the caller's buffer. Those two
    // actions are the transfer working as designed, so a post-call snapshot measures the driver, not
    // the wire. Here: the last status word seen before it was cleared, how many bytes actually reached
    // the caller's rx buffer, how many were sent, and how many poll iterations it took. All four are
    // cleared by geni_spi_reset_stats() so one transfer means one sample. Nothing in the engine's
    // behaviour changes -- these are stores into the struct, not register writes. Backup: .pre-l535.
    uint32_t stat_mirq_pre, stat_rx_fed, stat_tx_sent, stat_polls;
    // SE DMA mode (for engines whose FIFO interface is disabled and that cannot be re-enabled from the host): transfers go
    // through two caller-provided buffers that the serial engine's DMA master can reach at tx_phys/rx_phys. The caller owns
    // the translation (SMMU) and cache policy; the callbacks maintain the CPU caches around each transfer.
    int use_dma;
    struct geni_dma {
        void *ctx;
        void (*clean)(void *ctx, void *virt, size_t len);        // write back CPU-dirty lines before the engine reads
        void (*clean_inval)(void *ctx, void *virt, size_t len);  // write back and drop before the engine writes
        void (*inval)(void *ctx, void *virt, size_t len);        // drop stale lines after the engine wrote
        uint8_t *tx_virt, *rx_virt;
        uint64_t tx_phys, rx_phys;
        size_t size;
    } dma;
    int allow_fifo_disabled;            // try FIFO mode even when FIFO_IF_DISABLE is set (the Linux fallback when no GPI channel exists)
};

// Read-only: protocol, FIFO geometry, FIFO-disable flag, version. Safe to run before any register write; only valid when
// the SE clocks are already running (the caller must know that).
int geni_spi_read_info(struct geni_spi *s);
// Select FIFO mode and program the controller defaults. Refuses engines that are not configured for SPI or whose FIFO
// interface is disabled (those need the GPI DMA path, which this driver does not implement).
int geni_spi_init(struct geni_spi *s);
int geni_spi_setup(struct geni_spi *s, unsigned mode, unsigned chip_select);
// Program the serial clock: divider from the SE source clock (sclk_hz) and the requested rate; clk_sel is the source index.
int geni_spi_set_clock(struct geni_spi *s, unsigned speed_hz, unsigned sclk_hz, unsigned clk_sel);
// With use_dma set the transfer runs through the SE DMA engine.
// One full-duplex (rx!=NULL, tx!=NULL), TX-only or RX-only transfer of len bytes (8 bits per word), polled.
int geni_spi_transfer(struct geni_spi *s, const uint8_t *tx, uint8_t *rx, size_t len, int keep_cs);
/* l535: zero the inside-transfer counters (see the struct fields above) so a later read belongs to one
 * transfer. Defined in GeniSpi.c next to handle_rx(); touches no register. */
void geni_spi_reset_stats(struct geni_spi *s);
#endif
