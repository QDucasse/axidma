// axi_dma.c
#include "axi_dma.h"

/* Reset and halt settle in a few AXI-Lite cycles; this is a generous bound */
#define AXI_DMA_CTRL_TIMEOUT_S  0.1

static uint32_t rd(axi_dma_t *d, axi_dma_chan_t ch, uint32_t off)
{
    return axi_regs_read(&d->regs, (uint32_t)ch + off);
}

static void wr(axi_dma_t *d, axi_dma_chan_t ch, uint32_t off, uint32_t v)
{
    axi_regs_write(&d->regs, (uint32_t)ch + off, v);
}

const char *axi_dma_chan_name(axi_dma_chan_t ch)
{
    return ch == AXI_DMA_MM2S ? "MM2S" : "S2MM";
}

int axi_dma_open(axi_dma_t *d, uintptr_t base, unsigned length_width)
{
    if (length_width < 8 || length_width > 26) {
        fprintf(stderr, "axi_dma: length width %u outside the IP's 8..26\n",
                length_width);
        return -1;
    }
    d->max_length = (1u << length_width) - 1;
    if (axi_regs_open(&d->regs, base, DMA_MAP_SIZE) < 0) {
        fprintf(stderr, "axi_dma: cannot map the registers at 0x%lx\n",
                (unsigned long)base);
        return -1;
    }
    return 0;
}

void axi_dma_close(axi_dma_t *d)
{
    axi_regs_close(&d->regs);
}

int axi_dma_reset(axi_dma_t *d)
{
    /* Either channel's reset bit resets the whole core */
    wr(d, AXI_DMA_S2MM, AXI_DMA_CR, CR_RESET);
    if (axi_regs_poll(&d->regs, AXI_DMA_S2MM + AXI_DMA_CR, CR_RESET, 0,
                      AXI_DMA_CTRL_TIMEOUT_S) < 0 ||
        axi_regs_poll(&d->regs, AXI_DMA_MM2S + AXI_DMA_CR, CR_RESET, 0,
                      AXI_DMA_CTRL_TIMEOUT_S) < 0) {
        /* Reset only completes once both stream clocks are running and the
         * stream reset is released */
        fprintf(stderr, "axi_dma: reset did not complete (stream clocks/resets?)\n");
        return -1;
    }
    return 0;
}

int axi_dma_start(axi_dma_t *d, axi_dma_chan_t ch, uint32_t irq_en)
{
    wr(d, ch, AXI_DMA_CR, CR_RUN | (irq_en & CR_ALL_IRQ_EN));
    if (axi_regs_poll(&d->regs, ch + AXI_DMA_SR, SR_HALTED, 0,
                      AXI_DMA_CTRL_TIMEOUT_S) < 0) {
        fprintf(stderr, "axi_dma: %s did not leave the halted state\n",
                axi_dma_chan_name(ch));
        axi_dma_print_status(stderr, ch, rd(d, ch, AXI_DMA_SR));
        return -1;
    }
    return 0;
}

int axi_dma_stop(axi_dma_t *d, axi_dma_chan_t ch, double timeout_s)
{
    wr(d, ch, AXI_DMA_CR, rd(d, ch, AXI_DMA_CR) & ~CR_RUN);
    return axi_regs_poll(&d->regs, ch + AXI_DMA_SR, SR_HALTED, SR_HALTED,
                         timeout_s);
}

uint32_t axi_dma_arm(axi_dma_t *d, axi_dma_chan_t ch, uint64_t addr, uint32_t len)
{
    if (len == 0)
        return 0;
    if (len > d->max_length)
        len = d->max_length;

    wr(d, ch, AXI_DMA_SR, SR_ALL_IRQ);  // W1C, so the next IOC is this one's
    wr(d, ch, AXI_DMA_ADDR, (uint32_t)addr);
    /* ADDR_MSB only exists with c_addr_width > 32: skip it otherwise */
    if (addr >> 32)
        wr(d, ch, AXI_DMA_ADDR_MSB, (uint32_t)(addr >> 32));
    wr(d, ch, AXI_DMA_LENGTH, len);
    return len;
}

int axi_dma_wait(axi_dma_t *d, axi_dma_chan_t ch, double timeout_s)
{
    double deadline = timeout_s > 0 ? axi_now() + timeout_s : 0;
    unsigned spin = 0;

    for (;;) {
        uint32_t sr = rd(d, ch, AXI_DMA_SR);
        if (sr & (SR_ANY_ERR | SR_ERR_IRQ)) {
            fprintf(stderr, "axi_dma: %s error\n", axi_dma_chan_name(ch));
            axi_dma_print_status(stderr, ch, sr);
            return AXI_DMA_ERR;
        }
        /* IOC was cleared by axi_dma_arm(), so it marks this transfer's
         * completion even if a read right after the LENGTH write still sees
         * the previous IDLE. IOC is set whether or not its IRQ is enabled. */
        if ((sr & (SR_IOC_IRQ | SR_IDLE)) == (SR_IOC_IRQ | SR_IDLE))
            return AXI_DMA_OK;
        if (timeout_s > 0 && (++spin & 1023) == 0 && axi_now() > deadline)
            return AXI_DMA_TIMEOUT;
    }
}

uint32_t axi_dma_s2mm_bytes(axi_dma_t *d)
{
    return rd(d, AXI_DMA_S2MM, AXI_DMA_LENGTH);
}

long axi_dma_s2mm_drain(axi_dma_t *d, uint64_t addr, uint32_t len, double window_s)
{
    long drained = 0;

    for (;;) {
        if (!axi_dma_arm(d, AXI_DMA_S2MM, addr, len))
            return -1;
        int r = axi_dma_wait(d, AXI_DMA_S2MM, window_s);
        if (r == AXI_DMA_ERR)
            return -1;
        if (r == AXI_DMA_TIMEOUT)
            break;
        drained += axi_dma_s2mm_bytes(d);
    }
    /* Leave a clean, idle channel: the reset cancels the pending transfer */
    if (axi_dma_reset(d) < 0)
        return -1;
    return drained;
}

uint32_t axi_dma_status(axi_dma_t *d, axi_dma_chan_t ch)
{
    return rd(d, ch, AXI_DMA_SR);
}

void axi_dma_print_status(FILE *f, axi_dma_chan_t ch, uint32_t sr)
{
    static const struct { uint32_t bit; const char *name; } bits[] = {
        { SR_IDLE,        "idle"         },
        { SR_SG_INCLUDED, "sg-included"  },
        { SR_DMA_INT_ERR, "dma-int-err"  },
        { SR_DMA_SLV_ERR, "dma-slv-err"  },
        { SR_DMA_DEC_ERR, "dma-dec-err"  },
        { SR_SG_INT_ERR,  "sg-int-err"   },
        { SR_SG_SLV_ERR,  "sg-slv-err"   },
        { SR_SG_DEC_ERR,  "sg-dec-err"   },
        { SR_IOC_IRQ,     "ioc-irq"      },
        { SR_DLY_IRQ,     "dly-irq"      },
        { SR_ERR_IRQ,     "err-irq"      },
    };

    fprintf(f, "%s status 0x%08x: %s", axi_dma_chan_name(ch), sr,
            (sr & SR_HALTED) ? "halted" : "running");
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
        if (sr & bits[i].bit)
            fprintf(f, ", %s", bits[i].name);
    fputc('\n', f);
}
