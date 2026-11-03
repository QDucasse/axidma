// axi_dma.h
// Xilinx AXI DMA (PG021) in simple / direct register mode, both channels,
// polled from user space through /dev/mem.
//
// Typical S2MM use:
//   axi_dma_open(&d, DMA_BASE, AXI_DMA_LENGTH_WIDTH);
//   axi_dma_reset(&d);
//   axi_dma_start(&d, AXI_DMA_S2MM, 0);
//   axi_dma_arm(&d, AXI_DMA_S2MM, phys, len);      // length write starts it
//   axi_dma_wait(&d, AXI_DMA_S2MM, timeout_s);     // IOC+idle, error or timeout
//   n = axi_dma_s2mm_bytes(&d);                    // bytes up to TLAST
//
// Without the DRE (c_include_{mm2s,s2mm}_dre = 0, the default) addresses
// must be aligned to the channel's memory-map data width.
//
// The DMA should not also be owned by the kernel's xilinx_dma driver: if the
// device tree binds it, the driver may reset the core or ack its interrupts
// behind this code's back. Interrupts are left disabled by default here.
#ifndef AXI_DMA_H
#define AXI_DMA_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "axi_regs.h"

#define DMA_MAP_SIZE                0x10000

/* Absolute offsets, kept for code written against fuzzsight's headers */
#define MM2S_CONTROL_REGISTER       0x00
#define MM2S_STATUS_REGISTER        0x04
#define MM2S_SRC_ADDRESS_REGISTER   0x18
#define MM2S_TRNSFR_LENGTH_REGISTER 0x28
#define S2MM_CONTROL_REGISTER       0x30
#define S2MM_STATUS_REGISTER        0x34
#define S2MM_DST_ADDRESS_REGISTER   0x48
#define S2MM_BUFF_LENGTH_REGISTER   0x58

/* Per-channel offsets, relative to the channel base (axi_dma_chan_t) */
#define AXI_DMA_CR                  0x00
#define AXI_DMA_SR                  0x04
#define AXI_DMA_ADDR                0x18  // MM2S_SA / S2MM_DA
#define AXI_DMA_ADDR_MSB            0x1C  // only with c_addr_width > 32
#define AXI_DMA_LENGTH              0x28  // write starts the transfer

/* Control register */
#define CR_RUN                      (1u << 0)
#define CR_RESET                    (1u << 2)
#define CR_IOC_IRQ_EN               (1u << 12)
#define CR_DLY_IRQ_EN               (1u << 13)
#define CR_ERR_IRQ_EN               (1u << 14)
#define CR_ALL_IRQ_EN               (CR_IOC_IRQ_EN | CR_DLY_IRQ_EN | CR_ERR_IRQ_EN)

/* Status register */
#define SR_HALTED                   (1u << 0)
#define SR_IDLE                     (1u << 1)
#define SR_SG_INCLUDED              (1u << 3)
#define SR_DMA_INT_ERR              (1u << 4)
#define SR_DMA_SLV_ERR              (1u << 5)
#define SR_DMA_DEC_ERR              (1u << 6)
#define SR_SG_INT_ERR               (1u << 8)
#define SR_SG_SLV_ERR               (1u << 9)
#define SR_SG_DEC_ERR               (1u << 10)
#define SR_IOC_IRQ                  (1u << 12)  // write 1 to clear
#define SR_DLY_IRQ                  (1u << 13)  // write 1 to clear
#define SR_ERR_IRQ                  (1u << 14)  // write 1 to clear
#define SR_ALL_IRQ                  (SR_IOC_IRQ | SR_DLY_IRQ | SR_ERR_IRQ)
#define SR_ANY_ERR                  (SR_DMA_INT_ERR | SR_DMA_SLV_ERR | SR_DMA_DEC_ERR | \
                                     SR_SG_INT_ERR | SR_SG_SLV_ERR | SR_SG_DEC_ERR)

/* fuzzsight names */
#define STATUS_IDLE                 SR_IDLE
#define STATUS_IOC_IRQ              SR_IOC_IRQ
#define RESET_DMA                   CR_RESET
#define RUN_DMA                     CR_RUN
#define HALT_DMA                    0x00000000
#define ENABLE_ALL_IRQ              CR_ALL_IRQ_EN

/* c_sg_length_width of the IP (Vivado default 14, both designs here use 26) */
#define AXI_DMA_LENGTH_WIDTH        26

/* Return codes of axi_dma_wait() */
#define AXI_DMA_OK                  0
#define AXI_DMA_ERR                 (-1)  // error bit set, the channel halted
#define AXI_DMA_TIMEOUT             (-2)

typedef enum {
    AXI_DMA_MM2S = 0x00,
    AXI_DMA_S2MM = 0x30,
} axi_dma_chan_t;

typedef struct {
    axi_regs_t  regs;
    uint32_t    max_length;  // largest value LENGTH accepts
} axi_dma_t;

int      axi_dma_open(axi_dma_t *d, uintptr_t base, unsigned length_width);
void     axi_dma_close(axi_dma_t *d);

/* Soft reset of the whole core (both channels), waits for it to clear.
 * Data already queued upstream of S2MM (e.g. in an AXIS FIFO) survives it,
 * see axi_dma_s2mm_drain(). */
int      axi_dma_reset(axi_dma_t *d);

/* Set RS with the given CR_*_IRQ_EN bits, wait for HALTED to clear */
int      axi_dma_start(axi_dma_t *d, axi_dma_chan_t ch, uint32_t irq_en);
/* Clear RS, wait for HALTED (only once an ongoing transfer finished) */
int      axi_dma_stop(axi_dma_t *d, axi_dma_chan_t ch, double timeout_s);

/* Clear the sticky IRQ bits, program the address, then the length, which
 * starts the transfer. len is clamped to max_length; returns the length
 * armed, 0 on a bad argument. */
uint32_t axi_dma_arm(axi_dma_t *d, axi_dma_chan_t ch, uint64_t addr, uint32_t len);

/* Wait for the armed transfer: AXI_DMA_OK, AXI_DMA_ERR (status printed to
 * stderr, the core needs a reset) or AXI_DMA_TIMEOUT. timeout_s <= 0 waits
 * forever. */
int      axi_dma_wait(axi_dma_t *d, axi_dma_chan_t ch, double timeout_s);

/* Bytes the last S2MM transfer actually wrote: less than armed when TLAST
 * came first. Valid once the channel is idle. */
uint32_t axi_dma_s2mm_bytes(axi_dma_t *d);

/* Throw away whatever the stream source still holds: run S2MM transfers
 * into [addr, addr+len) until none completes within window_s. Overwrites
 * that region. Returns the bytes drained, or -1 on a DMA error. */
long     axi_dma_s2mm_drain(axi_dma_t *d, uint64_t addr, uint32_t len, double window_s);

uint32_t axi_dma_status(axi_dma_t *d, axi_dma_chan_t ch);
void     axi_dma_print_status(FILE *f, axi_dma_chan_t ch, uint32_t status);
const char *axi_dma_chan_name(axi_dma_chan_t ch);

#endif
