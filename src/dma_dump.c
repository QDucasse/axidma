// dma_dump.c
// Dump (part of) a u-dma-buf to a file, e.g. after s2mm_test.

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "lib/udmabuf.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-d udmabuf] [-s offset] [-n bytes] [-o file]\n"
        "  defaults: udmabuf0, the whole buffer, dma_dump.bin\n", prog);
}

int main(int argc, char **argv)
{
    const char *name = "udmabuf0", *out = "dma_dump.bin";
    size_t off = 0, n = 0;
    int opt;

    while ((opt = getopt(argc, argv, "d:s:n:o:h")) != -1) {
        switch (opt) {
        case 'd': name = optarg; break;
        case 's': off = strtoull(optarg, NULL, 0); break;
        case 'n': n = strtoull(optarg, NULL, 0); break;
        case 'o': out = optarg; break;
        default:  usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }

    udmabuf_t ub;
    if (udmabuf_open(&ub, name, 1) < 0)
        return 1;
    if (off > ub.size) {
        fprintf(stderr, "offset 0x%zx past the 0x%zx buffer\n", off, ub.size);
        udmabuf_close(&ub);
        return 1;
    }
    if (n == 0 || n > ub.size - off)
        n = ub.size - off;

    /* Whatever a DMA wrote must not be shadowed by stale cache lines */
    if (udmabuf_sync_for_cpu(&ub, off, n, UDMABUF_FROM_DEVICE) < 0) {
        udmabuf_close(&ub);
        return 1;
    }

    int ret = 1;
    FILE *f = fopen(out, "wb");  // truncates, unlike the old O_CREAT-only open
    if (!f) {
        perror(out);
    } else {
        size_t w = fwrite((char *)ub.buf + off, 1, n, f);
        if (fclose(f) != 0 || w != n)
            perror(out);
        else {
            printf("Dumped 0x%zx bytes of %s (offset 0x%zx) to %s\n", n, ub.name, off, out);
            ret = 0;
        }
    }
    udmabuf_close(&ub);
    return ret;
}
