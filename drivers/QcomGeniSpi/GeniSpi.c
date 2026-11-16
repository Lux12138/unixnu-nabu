// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "GeniSpi.h"

static uint32_t rd(struct geni_spi *s, uint32_t reg) { return s->se.rd(s->se.ctx, reg); }
static void wr(struct geni_spi *s, uint32_t reg, uint32_t v) { s->se.wr(s->se.ctx, reg, v); }
static void rmw_set(struct geni_spi *s, uint32_t reg, uint32_t bits) { wr(s, reg, rd(s, reg) | bits); }

static void irq_clear(struct geni_spi *s) {
    wr(s, SE_GSI_EVENT_EN, 0);
    wr(s, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
    wr(s, SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
    wr(s, SE_DMA_TX_IRQ_CLR, 0xffffffffu);
    wr(s, SE_DMA_RX_IRQ_CLR, 0xffffffffu);
    wr(s, SE_IRQ_EN, 0xffffffffu);
}

// FIFO words hold fifo_width_bits/8 packed bytes at 8 bits per word.
static unsigned bytes_per_word(const struct geni_spi *s) { return s->info.fifo_width_bits / 8; }

int geni_spi_read_info(struct geni_spi *s) {
    if (!s || !s->se.rd || !s->wrapper.rd) return GENI_ERR_PARAM;
    uint32_t fw = rd(s, GENI_FW_REVISION_RO);
    s->info.fw_revision = fw;
    s->info.protocol = (fw >> GENI_FW_PROTO_SHIFT) & 0xff;
    s->info.if_disable = rd(s, GENI_IF_DISABLE_RO);
    s->info.fifo_disabled = (s->info.if_disable & GENI_FIFO_IF_DISABLE) != 0;
    s->info.geni_status = rd(s, SE_GENI_STATUS);
    s->info.hw_param0 = rd(s, SE_HW_PARAM_0);
    s->info.qup_hw_version = s->wrapper.rd(s->wrapper.ctx, QUPV3_HW_VER_REG);
    uint32_t major = s->info.qup_hw_version >> 28, minor = (s->info.qup_hw_version >> 16) & 0xfff;
    uint32_t mask = ((major == 3 && minor >= 10) || major > 3) ? 0xff0000u : 0x3f0000u;
    s->info.tx_fifo_depth = (s->info.hw_param0 & mask) >> 16;
    s->info.fifo_width_bits = (s->info.hw_param0 >> 24) & 0x3f;
    return 0;
}

// Packing for 8-bit words, 4 per FIFO word, MSB first inside each byte; same values the vendor sequence programs.
static void config_packing(struct geni_spi *s, unsigned bpw, unsigned pack_words, int msb_first) {
    uint32_t cfg[GENI_PACK_VECTORS] = {0};
    int idx_start = msb_first ? (int)bpw - 1 : 0, idx = idx_start;
    int idx_delta = msb_first ? -8 : 8;
    unsigned ceil_bpw = (bpw + 7) & ~7u;
    unsigned iter = ceil_bpw * pack_words / 8;
    int temp_bpw = (int)bpw;
    if (iter == 0 || iter > GENI_PACK_VECTORS) return;
    for (unsigned i = 0; i < iter; i++) {
        int len = (temp_bpw < 8 ? temp_bpw : 8) - 1;
        cfg[i] = ((uint32_t)idx << GENI_PACK_START_SHIFT) | ((uint32_t)(msb_first != 0) << GENI_PACK_DIR_SHIFT) |
                 ((uint32_t)len << GENI_PACK_LEN_SHIFT);
        if (temp_bpw <= 8) {
            idx = (int)(i + 1) * 8 + idx_start;
            temp_bpw = (int)bpw;
        } else {
            idx += idx_delta;
            temp_bpw -= 8;
        }
    }
    cfg[iter - 1] |= GENI_PACK_STOP;
    uint32_t cfg0 = cfg[0] | (cfg[1] << GENI_PACK_VECTOR_SHIFT);
    uint32_t cfg1 = cfg[2] | (cfg[3] << GENI_PACK_VECTOR_SHIFT);
    wr(s, SE_GENI_TX_PACKING_CFG0, cfg0);
    wr(s, SE_GENI_TX_PACKING_CFG1, cfg1);
    wr(s, SE_GENI_RX_PACKING_CFG0, cfg0);
    wr(s, SE_GENI_RX_PACKING_CFG1, cfg1);
    if (pack_words || bpw == 32) wr(s, SE_GENI_BYTE_GRAN, bpw / 16);
}

static void set_word_len(struct geni_spi *s, unsigned bpw, int msb_first) {
    unsigned pack = (s->info.fifo_width_bits % bpw) ? 1 : s->info.fifo_width_bits / bpw;
    config_packing(s, bpw, pack, msb_first);
    wr(s, SE_SPI_WORD_LEN, (bpw - GENI_WORD_LEN_MIN) & 0x3ff);
}

int geni_spi_init(struct geni_spi *s) {
    int r = geni_spi_read_info(s);
    if (r) return r;
    if (s->info.protocol != GENI_PROTO_SPI) return GENI_ERR_PROTO;
    if (s->use_dma && (!s->dma.tx_virt || !s->dma.rx_virt || !s->dma.tx_phys || !s->dma.rx_phys || !s->dma.size))
        return GENI_ERR_PARAM;
    if (s->info.fifo_disabled && !s->allow_fifo_disabled && !s->use_dma) return GENI_ERR_NOTSUP;   // GPI DMA path not implemented
    if (s->info.tx_fifo_depth < 4 || s->info.fifo_width_bits != 32) return GENI_ERR_NODEV;
    // geni_se_init
    irq_clear(s);
    rmw_set(s, SE_GENI_CGC_CTRL, GENI_DEFAULT_CGC_EN);
    rmw_set(s, SE_DMA_GENERAL_CFG, GENI_AHB_SEC_SLV_CGC_ON | GENI_DMA_AHB_SLV_CGC_ON | GENI_DMA_TX_CGC_ON | GENI_DMA_RX_CGC_ON);
    wr(s, GENI_OUTPUT_CTRL, GENI_DEFAULT_IO_OUTPUT_CTRL);
    wr(s, GENI_FORCE_DEFAULT_REG, GENI_FORCE_DEFAULT);
    rmw_set(s, SE_IRQ_EN, GENI_IRQ_M | GENI_IRQ_S | GENI_IRQ_DMA_TX | GENI_IRQ_DMA_RX);
    wr(s, SE_GENI_DMA_MODE_EN, rd(s, SE_GENI_DMA_MODE_EN) & ~GENI_DMA_MODE_EN);
    wr(s, SE_GSI_EVENT_EN, 0);
    wr(s, SE_GENI_RX_WATERMARK_REG, s->info.tx_fifo_depth - 3);
    wr(s, SE_GENI_RX_RFR_WATERMARK_REG, s->info.tx_fifo_depth - 2);
    rmw_set(s, SE_GENI_M_IRQ_EN, GENI_M_COMMON_IRQ_EN);
    rmw_set(s, SE_GENI_S_IRQ_EN, GENI_S_COMMON_IRQ_EN);
    s->tx_wm = 1;
    uint32_t major = s->info.qup_hw_version >> 28, minor = (s->info.qup_hw_version >> 16) & 0xfff;
    s->oversampling = (major == 1 && minor == 0) ? 2 : 1;
    // FIFO mode selection
    irq_clear(s);
    rmw_set(s, SE_GENI_M_IRQ_EN, GENI_M_CMD_DONE | GENI_M_TX_FIFO_WATERMARK | GENI_M_RX_FIFO_WATERMARK | GENI_M_RX_FIFO_LAST);
    wr(s, SE_GENI_DMA_MODE_EN, rd(s, SE_GENI_DMA_MODE_EN) & ~GENI_DMA_MODE_EN);
    wr(s, SE_SPI_LOOPBACK, 0);
    wr(s, SE_SPI_DEMUX_SEL, 0);
    wr(s, SE_SPI_CPHA, 0);
    wr(s, SE_SPI_CPOL, 0);
    wr(s, SE_SPI_DEMUX_OUTPUT_INV, 0);
    wr(s, SE_SPI_TRANS_CFG, rd(s, SE_SPI_TRANS_CFG) & ~GENI_CS_TOGGLE);   // chip select is never driven manually
    s->cur_bpw = 0;
    s->cur_speed_hz = 0;
    s->last_mode = 0;
    s->last_cs = 0;
    if (!s->poll_limit) s->poll_limit = 1000000;
    if (s->use_dma) {
        // geni_se_select_dma_mode: drop the FIFO interrupt sources and switch the engine to its DMA master.
        irq_clear(s);
        wr(s, SE_GENI_M_IRQ_EN, rd(s, SE_GENI_M_IRQ_EN) & ~(GENI_M_CMD_DONE | GENI_M_TX_FIFO_WATERMARK | GENI_M_RX_FIFO_WATERMARK | GENI_M_RX_FIFO_LAST));
        wr(s, SE_GENI_DMA_MODE_EN, rd(s, SE_GENI_DMA_MODE_EN) | GENI_DMA_MODE_EN);
    }
    if (s->poll_only == 1) wr(s, SE_IRQ_EN, 0);     // status registers stay pollable; no unhandled interrupt reaches the GIC
    return 0;
}

int geni_spi_setup(struct geni_spi *s, unsigned mode, unsigned chip_select) {
    if (chip_select > 15) return GENI_ERR_PARAM;
    unsigned changed = s->last_mode ^ mode;
    int cs_changed = ((unsigned)s->last_cs != chip_select);
    s->last_cs = (int)chip_select;
    s->last_mode = mode;
    if (changed & GENI_SPI_LSB_FIRST) s->cur_bpw = 0;
    if (changed & GENI_SPI_LOOP) wr(s, SE_SPI_LOOPBACK, (mode & GENI_SPI_LOOP) ? GENI_LOOPBACK_ENABLE : 0);
    if (cs_changed) wr(s, SE_SPI_DEMUX_SEL, chip_select);
    if (changed & GENI_SPI_CPHA) wr(s, SE_SPI_CPHA, (mode & GENI_SPI_CPHA) ? GENI_CPHA_BIT : 0);
    if (changed & GENI_SPI_CPOL) wr(s, SE_SPI_CPOL, (mode & GENI_SPI_CPOL) ? GENI_CPOL_BIT : 0);
    if ((changed & GENI_SPI_CS_HIGH) || (cs_changed && (mode & GENI_SPI_CS_HIGH)))
        wr(s, SE_SPI_DEMUX_OUTPUT_INV, (mode & GENI_SPI_CS_HIGH) ? (1u << chip_select) : 0);
    return 0;
}

int geni_spi_set_clock(struct geni_spi *s, unsigned speed_hz, unsigned sclk_hz, unsigned clk_sel) {
    if (!speed_hz || !sclk_hz || clk_sel > 7 || !s->oversampling) return GENI_ERR_PARAM;
    unsigned long long denom = (unsigned long long)s->oversampling * speed_hz;
    unsigned long long div = (sclk_hz + denom - 1) / denom;
    if (div == 0 || div > GENI_CLK_DIV_MAX) return GENI_ERR_PARAM;
    wr(s, SE_GENI_CLK_SEL, clk_sel);
    wr(s, GENI_SER_M_CLK_CFG, ((uint32_t)div << GENI_CLK_DIV_SHIFT) | GENI_SER_CLK_EN);
    s->cur_speed_hz = speed_hz;
    return 0;
}

struct xfer {
    const uint8_t *tx;
    uint8_t *rx;
    size_t tx_rem, rx_rem, len;
};

static int handle_tx(struct geni_spi *s, struct xfer *x) {
    unsigned bpw = bytes_per_word(s);
    size_t max = (size_t)(s->info.tx_fifo_depth - s->tx_wm) * bpw;
    /* l547: ask the FIFO how much it actually has room for before writing. The value above is a
     * worst-case assumption; M_TX_FIFO_WR_ERR on l544 (64513) and l545 (32257) -- with
     * TX_FIFO_STATUS still holding 13 words in both -- says the assumption was wrong, not the block
     * size. words-present is the low bits of SE_GENI_TX_FIFO_STATUS (0x800); the field width is not
     * documented in this header, so it is masked generously and then clamped to the depth we read out
     * of HW_PARAM0 rather than trusted. One register read per call, on a loop that already polls
     * M_IRQ_STATUS every iteration. */
    uint32_t fs = rd(s, SE_GENI_TX_FIFO_STATUS);
    unsigned words = fs & 0x3fu;
    if (words > s->info.tx_fifo_depth) words = s->info.tx_fifo_depth;
    size_t room = (size_t)(s->info.tx_fifo_depth - words) * bpw;
    if (words >= s->info.tx_fifo_depth) room = 0;
    if (room >= bpw) room -= bpw;        /* one word of slack: WC semantics are inferred, not documented */
    if (room < max) max = room;
    if (x->tx_rem < max) max = x->tx_rem;
    if (!max) return 1;        /* nothing drained yet: keep waiting, do not report "finished" */
    const uint8_t *p = x->tx + (x->len - x->tx_rem);
    size_t i = 0;
    while (i < max) {
        uint32_t word = 0;
        size_t n = max - i < bpw ? max - i : bpw;
        for (size_t j = 0; j < n; j++) word |= (uint32_t)p[i++] << (8 * j);
        wr(s, SE_GENI_TX_FIFOn, word);
    }
    s->stat_tx_sent += (uint32_t)max;   /* l535: bytes pushed into the TX FIFO by this call */
    x->tx_rem -= max;
    if (!x->tx_rem) {
        wr(s, SE_GENI_TX_WATERMARK_REG, 0);
        return 0;
    }
    return 1;
}

static void handle_rx(struct geni_spi *s, struct xfer *x) {
    unsigned bpw = bytes_per_word(s);
    uint32_t st = rd(s, SE_GENI_RX_FIFO_STATUS);
    size_t bytes = (size_t)(st & GENI_RX_WC_MASK) * bpw;
    if (st & GENI_RX_LAST) {
        unsigned valid = (st >> GENI_RX_LAST_VALID_SHIFT) & GENI_RX_LAST_VALID_MASK;
        if (valid && valid < 4) bytes -= bpw - valid;
    }
    if (!x->rx) {
        for (size_t i = 0; i < (bytes + bpw - 1) / bpw; i++) (void)rd(s, SE_GENI_RX_FIFOn);
        return;
    }
    if (x->rx_rem < bytes) bytes = x->rx_rem;
    uint8_t *p = x->rx + (x->len - x->rx_rem);
    size_t i = 0;
    while (i < bytes) {
        uint32_t word = rd(s, SE_GENI_RX_FIFOn);
        size_t n = bytes - i < bpw ? bytes - i : bpw;
        for (size_t j = 0; j < n; j++) p[i++] = (uint8_t)(word >> (8 * j));
    }
    s->stat_rx_fed += (uint32_t)bytes;   /* l535: bytes actually handed to the caller's buffer */
    x->rx_rem -= bytes;
}

/* l535: zero the inside-transfer counters so one transfer means one sample. Called by the probe
 * before each arm; it touches no register and changes no transfer behaviour. */
void geni_spi_reset_stats(struct geni_spi *s) {
    if (!s) return;
    s->stat_mirq_pre = 0;
    s->stat_rx_fed = 0;
    s->stat_tx_sent = 0;
    s->stat_polls = 0;
}

static int wait_bit(struct geni_spi *s, uint32_t bit) {
    for (unsigned n = 0; n < s->poll_limit; n++) {
        uint32_t st = rd(s, SE_GENI_M_IRQ_STATUS);
        if (st & bit) {
            wr(s, SE_GENI_M_IRQ_CLEAR, st);
            return 0;
        }
        if (s->se.delay_us) s->se.delay_us(s->se.ctx, 1);
    }
    return GENI_ERR_TIMEOUT;
}

static void recover(struct geni_spi *s) {
    wr(s, SE_GENI_TX_WATERMARK_REG, 0);
    wr(s, SE_GENI_M_CMD_CTRL_REG, GENI_M_CMD_CANCEL);
    if (wait_bit(s, GENI_M_CMD_CANCEL_DONE | GENI_M_CMD_DONE) == 0) return;
    wr(s, SE_GENI_M_CMD_CTRL_REG, GENI_M_CMD_ABORT);
    (void)wait_bit(s, GENI_M_CMD_ABORT_DONE | GENI_M_CMD_DONE);
}

static int transfer_dma(struct geni_spi *s, const uint8_t *tx, uint8_t *rx, size_t len, int keep_cs) {
    struct geni_dma *d = &s->dma;
    if (len > d->size) return GENI_ERR_PARAM;
    uint32_t cmd = 0;
    if (tx) {
        volatile uint8_t *tv = d->tx_virt;   // volatile: the buffer may be an uncached device-type mapping, so no merged stores
        for (size_t i = 0; i < len; i++) tv[i] = tx[i];
        if (d->clean) d->clean(d->ctx, d->tx_virt, len);
        cmd |= GENI_M_CMD_TX_ONLY;
        wr(s, SE_SPI_TX_TRANS_LEN, (uint32_t)len);
    }
    if (rx) {
        volatile uint8_t *rv0 = d->rx_virt;
        for (size_t i = 0; i < len; i++) rv0[i] = 0x5a;   // canary: bytes the engine never wrote stay recognisable
        if (d->clean_inval) d->clean_inval(d->ctx, d->rx_virt, len);
        cmd |= GENI_M_CMD_RX_ONLY;
        wr(s, SE_SPI_RX_TRANS_LEN, (uint32_t)len);
    }
    if (cmd == (GENI_M_CMD_TX_ONLY | GENI_M_CMD_RX_ONLY)) cmd = GENI_M_CMD_TX_RX;
    wr(s, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
    wr(s, SE_DMA_TX_IRQ_CLR, 0xffffffffu);
    wr(s, SE_DMA_RX_IRQ_CLR, 0xffffffffu);
    wr(s, SE_GENI_M_CMD0, (cmd << GENI_M_OPCODE_SHIFT) | (keep_cs ? GENI_M_PARAM_FRAGMENTATION : 0));
    // geni_se_rx_init_dma / geni_se_tx_init_dma: the LEN write starts each direction.
    if (rx) {
        wr(s, SE_DMA_RX_IRQ_EN_SET, GENI_DMA_DONE | GENI_DMA_EOT | GENI_DMA_AHB_ERR);
        wr(s, SE_DMA_RX_PTR_L, (uint32_t)d->rx_phys);
        wr(s, SE_DMA_RX_PTR_H, (uint32_t)(d->rx_phys >> 32));
        wr(s, SE_DMA_RX_ATTR, 0);
        wr(s, SE_DMA_RX_LEN, (uint32_t)len);
    }
    if (tx) {
        wr(s, SE_DMA_TX_IRQ_EN_SET, GENI_DMA_DONE | GENI_DMA_EOT | GENI_DMA_AHB_ERR);
        wr(s, SE_DMA_TX_PTR_L, (uint32_t)d->tx_phys);
        wr(s, SE_DMA_TX_PTR_H, (uint32_t)(d->tx_phys >> 32));
        wr(s, SE_DMA_TX_ATTR, GENI_DMA_TX_ATTR_EOT_BUF);
        wr(s, SE_DMA_TX_LEN, (uint32_t)len);
    }
    int tx_done = !tx, rx_done = !rx, status = GENI_ERR_TIMEOUT;
    for (unsigned n = 0; n < s->poll_limit; n++) {
        uint32_t ts = tx ? rd(s, SE_DMA_TX_IRQ_STAT) : 0, rs = rx ? rd(s, SE_DMA_RX_IRQ_STAT) : 0;
        uint32_t ms = rd(s, SE_GENI_M_IRQ_STATUS);
        if (ts) wr(s, SE_DMA_TX_IRQ_CLR, ts);
        if (rs) wr(s, SE_DMA_RX_IRQ_CLR, rs);
        if (ms) wr(s, SE_GENI_M_IRQ_CLEAR, ms);
        if ((ts & GENI_DMA_AHB_ERR) || (rs & GENI_DMA_AHB_ERR) || (ms & GENI_M_ERRORS)) { status = GENI_ERR_IO; break; }
        if (ts & GENI_DMA_DONE) tx_done = 1;
        if (rs & GENI_DMA_DONE) rx_done = 1;
        if (tx_done && rx_done) { status = 0; break; }
        if (s->se.delay_us) s->se.delay_us(s->se.ctx, 1);
    }
    s->last_tx_in = tx ? rd(s, SE_DMA_TX_LEN_IN) : 0;
    s->last_rx_in = rx ? rd(s, SE_DMA_RX_LEN_IN) : 0;
    if (status == 0) {
        if (rx) {
            if (d->inval) d->inval(d->ctx, d->rx_virt, len);
            volatile uint8_t *rv = d->rx_virt;
            for (size_t i = 0; i < len; i++) rx[i] = rv[i];
        }
        return 0;
    }
    s->snap_dma_tx = rd(s, SE_DMA_TX_IRQ_STAT);
    s->snap_dma_rx = rd(s, SE_DMA_RX_IRQ_STAT);
    s->snap_dma_txin = rd(s, SE_DMA_TX_LEN_IN);
    s->snap_dma_rxin = rd(s, SE_DMA_RX_LEN_IN);
    s->snap_irq = rd(s, SE_GENI_M_IRQ_STATUS);
    s->snap_status = rd(s, SE_GENI_STATUS);
    s->snap_txfs = rd(s, SE_GENI_TX_FIFO_STATUS);
    s->snap_rxfs = rd(s, SE_GENI_RX_FIFO_STATUS);
    s->snap_valid = 1;
    recover(s);
    wr(s, SE_DMA_TX_FSM_RST, 1);
    wr(s, SE_DMA_RX_FSM_RST, 1);
    return status;
}

int geni_spi_transfer(struct geni_spi *s, const uint8_t *tx, uint8_t *rx, size_t len, int keep_cs) {
    if (!s || !len || len > GENI_TRANS_LEN_MASK || (!tx && !rx) || !s->cur_speed_hz) return GENI_ERR_PARAM;
    if (s->cur_bpw != 8) {
        set_word_len(s, 8, !(s->last_mode & GENI_SPI_LSB_FIRST));
        s->cur_bpw = 8;
    }
    if (s->use_dma) return transfer_dma(s, tx, rx, len, keep_cs);
    struct xfer x = {tx, rx, tx ? len : 0, rx ? len : 0, len};
    uint32_t cmd = 0;
    if (tx) {
        cmd |= GENI_M_CMD_TX_ONLY;
        wr(s, SE_SPI_TX_TRANS_LEN, (uint32_t)len);
    }
    if (rx) {
        cmd |= GENI_M_CMD_RX_ONLY;
        wr(s, SE_SPI_RX_TRANS_LEN, (uint32_t)len);
    }
    if (cmd == (GENI_M_CMD_TX_ONLY | GENI_M_CMD_RX_ONLY)) cmd = GENI_M_CMD_TX_RX;
    wr(s, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
    wr(s, SE_GENI_M_CMD0, (cmd << GENI_M_OPCODE_SHIFT) | (keep_cs ? GENI_M_PARAM_FRAGMENTATION : 0));
    if (tx && handle_tx(s, &x)) wr(s, SE_GENI_TX_WATERMARK_REG, s->tx_wm);
    int status = GENI_ERR_TIMEOUT;
    for (unsigned n = 0; n < s->poll_limit; n++) {
        uint32_t st = rd(s, SE_GENI_M_IRQ_STATUS);
        if (!st) {
            if (s->se.delay_us) s->se.delay_us(s->se.ctx, 1);
            continue;
        }
        /* l535: copy the status word BEFORE it is acknowledged. Writing the value we just read into
         * SE_GENI_M_IRQ_CLEAR is what clears it, so from the next line on the register is back to 0
         * and any later read -- exactly what l534 did after the transfer returned -- measures the
         * clear, not the completion. stat_polls counts only iterations that saw a non-zero word. */
        s->stat_mirq_pre = st;
        s->stat_polls = s->stat_polls + 1u;
        wr(s, SE_GENI_M_IRQ_CLEAR, st);
        if (st & GENI_M_ERRORS) {
            status = GENI_ERR_IO;
            break;
        }
        if (st & (GENI_M_RX_FIFO_WATERMARK | GENI_M_RX_FIFO_LAST)) handle_rx(s, &x);
        if ((st & GENI_M_TX_FIFO_WATERMARK) && tx) handle_tx(s, &x);
        if (st & GENI_M_CMD_DONE) {
            if (rx && x.rx_rem) handle_rx(s, &x);
            status = (x.tx_rem || x.rx_rem) ? GENI_ERR_IO : 0;
            if (x.tx_rem) wr(s, SE_GENI_TX_WATERMARK_REG, 0);
            return status;
        }
    }
    s->snap_dma_tx = rd(s, SE_DMA_TX_IRQ_STAT);
    s->snap_dma_rx = rd(s, SE_DMA_RX_IRQ_STAT);
    s->snap_dma_txin = rd(s, SE_DMA_TX_LEN_IN);
    s->snap_dma_rxin = rd(s, SE_DMA_RX_LEN_IN);
    s->snap_irq = rd(s, SE_GENI_M_IRQ_STATUS);
    s->snap_status = rd(s, SE_GENI_STATUS);
    s->snap_txfs = rd(s, SE_GENI_TX_FIFO_STATUS);
    s->snap_rxfs = rd(s, SE_GENI_RX_FIFO_STATUS);
    s->snap_valid = 1;
    recover(s);
    return status;
}
