// s2mm_test.c
// S2MM-only capture for the s2mm_async(_ila) designs: axi_pattern_gen -> async
// AXIS FIFO -> AXI DMA S2MM -> u-dma-buf. The generator emits a 32-bit
// counter with TLAST every FRAME_BYTES (32), so each S2MM transfer stops at
// a packet boundary and is re-armed at the next free byte of the buffer.
// Because the data is a counter, the capture is checked for continuity: a
// gap means data was lost or written to the wrong place.
//
// Note: the generator latches its start input, so clearing the GPIO at the
// end does not stop it; only a PL reset does. A second run therefore starts
// from wherever the counter is, which the continuity check tolerates.

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/axi_dma.h"
#include "lib/axi_regs.h"
#include "lib/udmabuf.h"

#ifndef DMA_BASE
#define DMA_BASE        0x80000000
#endif
#ifndef GPIO_BASE
#define GPIO_BASE       0x80010000
#endif
#define GPIO_MAP_SIZE   0x10000
#define GPIO_DATA       0x00
#define GPIO_TRI        0x04

#define DEFAULT_UDMABUF "udmabuf0"
#define DEFAULT_BYTES   0x4000
#define WAIT_TIMEOUT_S  1.0

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-d udmabuf] [-n bytes] [-o dump.bin] [-u] [-y] [-v]\n"
        "  -d  u-dma-buf device (default " DEFAULT_UDMABUF ")\n"
        "  -n  bytes to capture (default 0x%x)\n"
        "  -o  write the capture to a file\n"
        "  -u  map the buffer uncached instead of cached + sync\n"
        "  -y  do not wait for Enter before starting the generator\n"
        "  -v  print every transfer\n", prog, DEFAULT_BYTES);
}

static void hexdump(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        printf("%02x%s", b[i], (i % 16 == 15) ? "\n" : (i % 4 == 3) ? " " : "");
    if (n % 16) putchar('\n');
}

/* Count breaks in the +1 sequence, print the first few */
static size_t check_counter(const uint32_t *w, size_t nwords)
{
    size_t gaps = 0;
    for (size_t i = 1; i < nwords; i++) {
        if (w[i] != w[i - 1] + 1) {
            if (gaps < 8)
                printf("  gap at byte 0x%zx: 0x%08x -> 0x%08x\n",
                       i * 4, w[i - 1], w[i]);
            gaps++;
        }
    }
    return gaps;
}

static int dump_to_file(const char *path, const void *buf, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    size_t w = fwrite(buf, 1, n, f);
    if (fclose(f) != 0 || w != n) { perror(path); return -1; }
    printf("Wrote 0x%zx bytes to %s\n", n, path);
    return 0;
}

int main(int argc, char **argv)
{
    const char *name = DEFAULT_UDMABUF, *out = NULL;
    size_t want = DEFAULT_BYTES;
    int cached = 1, wait_enter = 1, verbose = 0, opt;

    while ((opt = getopt(argc, argv, "d:n:o:uyvh")) != -1) {
        switch (opt) {
        case 'd': name = optarg; break;
        case 'n': want = strtoull(optarg, NULL, 0); break;
        case 'o': out = optarg; break;
        case 'u': cached = 0; break;
        case 'y': wait_enter = 0; break;
        case 'v': verbose = 1; break;
        default:  usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (want == 0 || want % 4) {
        fprintf(stderr, "-n must be a non-zero multiple of 4\n");
        return 2;
    }

    axi_dma_t dma;
    axi_regs_t gpio;
    udmabuf_t ub;
    int ret = 1;

    if (udmabuf_open(&ub, name, cached) < 0)
        return 1;
    if (want > ub.size) {
        fprintf(stderr, "%s holds 0x%zx bytes, asked for 0x%zx\n", ub.name, ub.size, want);
        udmabuf_close(&ub);
        return 1;
    }
    printf("u-dma-buf %s: phys 0x%lx, 0x%zx bytes, %s\n", ub.name, ub.phys_addr,
           ub.size, ub.coherent ? "coherent" : ub.cached ? "cached" : "uncached");

    if (axi_dma_open(&dma, DMA_BASE, AXI_DMA_LENGTH_WIDTH) < 0)
        goto out_buf;
    if (axi_regs_open(&gpio, GPIO_BASE, GPIO_MAP_SIZE) < 0)
        goto out_dma;

    /* Clear the destination, then push the zeros out of the cache so no
     * dirty line lands on top of the DMA data later */
    memset(ub.buf, 0, want);
    if (udmabuf_sync_for_device(&ub, 0, want, UDMABUF_FROM_DEVICE) < 0)
        goto out_gpio;

    if (axi_dma_reset(&dma) < 0 || axi_dma_start(&dma, AXI_DMA_S2MM, 0) < 0)
        goto out_gpio;
    axi_dma_print_status(stdout, AXI_DMA_S2MM, axi_dma_status(&dma, AXI_DMA_S2MM));

    if (wait_enter) {
        printf("Arm the ILA, then press Enter to start the generator...");
        fflush(stdout);
        getchar();
    }
    axi_regs_write(&gpio, GPIO_TRI, 0);   // channel 1 as output
    axi_regs_write(&gpio, GPIO_DATA, 1);  // start the pattern generator

    size_t off = 0, transfers = 0;
    double t0 = axi_now();
    while (off < want) {
        uint32_t armed = axi_dma_arm(&dma, AXI_DMA_S2MM, ub.phys_addr + off,
                                     (uint32_t)(want - off < dma.max_length ?
                                                want - off : dma.max_length));
        int r = axi_dma_wait(&dma, AXI_DMA_S2MM, WAIT_TIMEOUT_S);
        if (r == AXI_DMA_TIMEOUT) {
            fprintf(stderr, "No data after %.1f s at byte 0x%zx (bitstream "
                    "loaded? generator running?)\n", WAIT_TIMEOUT_S, off);
            axi_dma_print_status(stderr, AXI_DMA_S2MM,
                                 axi_dma_status(&dma, AXI_DMA_S2MM));
            break;
        }
        if (r != AXI_DMA_OK)
            break;
        uint32_t got = axi_dma_s2mm_bytes(&dma);
        if (verbose)
            printf("  transfer %zu: 0x%x/0x%x bytes at 0x%zx\n",
                   transfers, got, armed, off);
        if (got == 0 || got > armed) {
            fprintf(stderr, "S2MM reported 0x%x bytes for a 0x%x transfer\n", got, armed);
            break;
        }
        off += got;
        transfers++;
    }
    double dt = axi_now() - t0;

    axi_regs_write(&gpio, GPIO_DATA, 0);  // see the note at the top

    if (udmabuf_sync_for_cpu(&ub, 0, off, UDMABUF_FROM_DEVICE) < 0)
        goto out_gpio;

    printf("Captured 0x%zx bytes in %zu transfers, %.3f ms (%.1f MB/s)\n",
           off, transfers, dt * 1e3, dt > 0 ? (double)off / dt / 1e6 : 0.0);
    printf("First bytes:\n");
    hexdump(ub.buf, off < 64 ? off : 64);

    size_t gaps = check_counter(ub.buf, off / 4);
    if (off >= 4)
        printf("Counter 0x%08x .. 0x%08x, %zu gap(s)\n", ((uint32_t *)ub.buf)[0],
               ((uint32_t *)ub.buf)[off / 4 - 1], gaps);

    if (out && dump_to_file(out, ub.buf, off) < 0)
        goto out_gpio;

    ret = (off == want && gaps == 0) ? 0 : 1;
    printf("%s\n", ret == 0 ? "PASS" : "FAIL");

out_gpio:
    axi_regs_close(&gpio);
out_dma:
    axi_dma_close(&dma);
out_buf:
    udmabuf_close(&ub);
    return ret;
}
