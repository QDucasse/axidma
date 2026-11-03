// udmabuf.h
// User-space side of a u-dma-buf device (https://github.com/ikwzm/udmabuf):
// physical address and size from sysfs, mmap of /dev/<name>, and the cache
// maintenance needed when the buffer is mapped cached.
//
// Cached vs uncached: u-dma-buf maps the buffer uncached when /dev/<name> is
// opened with O_SYNC, cached otherwise (sync_mode=1, the driver default).
// Uncached is simple but slow to read (fuzzsight measured ~350 us for 64 KiB
// on the ZCU104). Cached is fast but on a non-coherent port (HP0 here) the
// caller must:
//   - udmabuf_sync_for_device() after the CPU writes data a DMA will read
//     (MM2S source) or *any* data in a region a DMA will write (S2MM
//     destination, e.g. after a memset), so no dirty line is evicted over it;
//   - udmabuf_sync_for_cpu() after a DMA wrote a region, before reading it.
// Both are no-ops on an uncached mapping or a dma-coherent buffer.
#ifndef UDMABUF_H
#define UDMABUF_H

#include <stddef.h>
#include <stdint.h>

#define UDMABUF_SYSFS_ROOT "/sys/class/u-dma-buf"

/* Same values as the kernel's enum dma_data_direction */
typedef enum {
    UDMABUF_BIDIRECTIONAL = 0,
    UDMABUF_TO_DEVICE     = 1,  // CPU wrote, DMA reads (MM2S)
    UDMABUF_FROM_DEVICE   = 2,  // DMA wrote, CPU reads (S2MM)
} udmabuf_dir_t;

typedef struct {
    char            name[64];
    int             fd;
    int             cached;         // mapped cached
    int             coherent;       // dma_coherent in sysfs, syncs are no-ops
    unsigned long   phys_addr;
    size_t          size;
    void           *buf;
    /* sysfs sync state, kept open/remembered to make repeated syncs cheap */
    int             sync_cpu_fd;
    int             sync_dev_fd;
    size_t          sync_off, sync_len;
    int             sync_dir;
} udmabuf_t;

/* Read "phys_addr" and "size" for a u-dma-buf device out of sysfs.
 * Same signature as fuzzsight's. */
int  udmabuf_sysfs_info(const char *name, unsigned long *phys_addr, size_t *size);

/* Open and map the whole buffer. name is a bare device name ("udmabuf0") or
 * a path ("/dev/udmabuf0"). cached=0 maps it uncached (O_SYNC). A cached
 * request falls back to uncached if the sync attributes are not writable;
 * check b->cached afterwards. */
int  udmabuf_open(udmabuf_t *b, const char *name, int cached);
void udmabuf_close(udmabuf_t *b);

/* Cache maintenance over [off, off+len), see the header comment */
int  udmabuf_sync_for_cpu(udmabuf_t *b, size_t off, size_t len, udmabuf_dir_t dir);
int  udmabuf_sync_for_device(udmabuf_t *b, size_t off, size_t len, udmabuf_dir_t dir);

#endif
