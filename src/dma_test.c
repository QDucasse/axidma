// dma_test.c
// MM2S -> AXIS FIFO -> S2MM loopback self-test for the axidma design.
// The source and destination both live in the u-dma-buf: the source at
// offset 0, the destination at the next 4 KiB boundary after it. The source
// is filled with a pseudo-random pattern, looped through the PL, and the
// destination compared against it.

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/axi_dma.h"
#include "lib/udmabuf.h"

#ifndef DMA_BASE
#define DMA_BASE        0x80000000
#endif

#define DEFAULT_UDMABUF "udmabuf0"
#define DEFAULT_BYTES   4096
#define WAIT_TIMEOUT_S  1.0

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-d udmabuf] [-n bytes] [-s seed] [-u]\n"
        "  -d  u-dma-buf device (default " DEFAULT_UDMABUF ")\n"
        "  -n  bytes to loop back (default %d)\n"
        "  -s  pattern seed\n"
        "  -u  map the buffer uncached instead of cached + sync\n",
        prog, DEFAULT_BYTES);
}

static void hexdump(const char *label, const void *p, size_t n)
{
    const uint8_t *b = p;
    printf("%-12s", label);
    for (size_t i = 0; i < n; i++)
        printf("%02x%s", b[i], (i % 4 == 3) ? " " : "");
    putchar('\n');
}

static void fill_pattern(uint8_t *p, size_t n, uint32_t seed)
{
    uint32_t x = seed ? seed : 0x12345678;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;  // xorshift32
        p[i] = (uint8_t)x;
    }
}

int main(int argc, char **argv)
{
    const char *name = DEFAULT_UDMABUF;
    size_t n = DEFAULT_BYTES;
    uint32_t seed = 0xdeadbeef;
    int cached = 1, opt;

    while ((opt = getopt(argc, argv, "d:n:s:uh")) != -1) {
        switch (opt) {
        case 'd': name = optarg; break;
        case 'n': n = strtoull(optarg, NULL, 0); break;
        case 's': seed = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'u': cached = 0; break;
        default:  usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }

    axi_dma_t dma;
    udmabuf_t ub;
    int ret = 1;

    if (udmabuf_open(&ub, name, cached) < 0)
        return 1;
    printf("u-dma-buf %s: phys 0x%lx, 0x%zx bytes, %s\n", ub.name, ub.phys_addr,
           ub.size, ub.coherent ? "coherent" : ub.cached ? "cached" : "uncached");

    size_t dst_off = (n + 0xfff) & ~(size_t)0xfff;
    if (n == 0 || dst_off + n > ub.size) {
        fprintf(stderr, "-n 0x%zx does not fit twice in the buffer\n", n);
        goto out_buf;
    }
    if (axi_dma_open(&dma, DMA_BASE, AXI_DMA_LENGTH_WIDTH) < 0)
        goto out_buf;
    if (n > dma.max_length) {
        fprintf(stderr, "-n 0x%zx above the DMA's 0x%x max length\n", n, dma.max_length);
        goto out_dma;
    }

    uint8_t *src = ub.buf, *dst = (uint8_t *)ub.buf + dst_off;
    fill_pattern(src, n, seed);
    memset(dst, 0, n);
    if (udmabuf_sync_for_device(&ub, 0, n, UDMABUF_TO_DEVICE) < 0 ||
        udmabuf_sync_for_device(&ub, dst_off, n, UDMABUF_FROM_DEVICE) < 0)
        goto out_dma;

    if (axi_dma_reset(&dma) < 0 ||
        axi_dma_start(&dma, AXI_DMA_S2MM, 0) < 0 ||
        axi_dma_start(&dma, AXI_DMA_MM2S, 0) < 0)
        goto out_dma;

    /* Destination first, so S2MM is ready when the data comes around */
    double t0 = axi_now();
    axi_dma_arm(&dma, AXI_DMA_S2MM, ub.phys_addr + dst_off, (uint32_t)n);
    axi_dma_arm(&dma, AXI_DMA_MM2S, ub.phys_addr, (uint32_t)n);

    int r = axi_dma_wait(&dma, AXI_DMA_MM2S, WAIT_TIMEOUT_S);
    if (r == AXI_DMA_OK)
        r = axi_dma_wait(&dma, AXI_DMA_S2MM, WAIT_TIMEOUT_S);
    double dt = axi_now() - t0;
    if (r != AXI_DMA_OK) {
        if (r == AXI_DMA_TIMEOUT)
            fprintf(stderr, "Transfer timed out\n");
        axi_dma_print_status(stderr, AXI_DMA_MM2S, axi_dma_status(&dma, AXI_DMA_MM2S));
        axi_dma_print_status(stderr, AXI_DMA_S2MM, axi_dma_status(&dma, AXI_DMA_S2MM));
        goto out_dma;
    }

    uint32_t got = axi_dma_s2mm_bytes(&dma);
    if (udmabuf_sync_for_cpu(&ub, dst_off, n, UDMABUF_FROM_DEVICE) < 0)
        goto out_dma;

    size_t show = n < 32 ? n : 32;
    hexdump("Source:", src, show);
    hexdump("Destination:", dst, show);
    printf("Looped back 0x%x of 0x%zx bytes in %.1f us\n", got, n, dt * 1e6);

    size_t bad = 0, first = 0;
    for (size_t i = 0; i < n; i++)
        if (src[i] != dst[i] && bad++ == 0)
            first = i;
    if (bad)
        printf("%zu byte(s) differ, first at 0x%zx: 0x%02x != 0x%02x\n",
               bad, first, dst[first], src[first]);

    ret = (got == n && bad == 0) ? 0 : 1;
    printf("%s\n", ret == 0 ? "PASS" : "FAIL");

out_dma:
    axi_dma_close(&dma);
out_buf:
    udmabuf_close(&ub);
    return ret;
}
