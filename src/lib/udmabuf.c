// udmabuf.c
#include "udmabuf.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Read a sysfs attribute of a u-dma-buf device into buf, NUL terminated */
static int sysfs_read(const char *name, const char *attr, char *buf, size_t n)
{
    char path[256];
    snprintf(path, sizeof(path), UDMABUF_SYSFS_ROOT "/%s/%s", name, attr);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    memset(buf, 0, n);
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    return r < 0 ? -1 : 0;
}

/* Write a string to a sysfs attribute of a u-dma-buf device */
static int sysfs_write(const char *name, const char *attr, const char *val)
{
    char path[256];
    snprintf(path, sizeof(path), UDMABUF_SYSFS_ROOT "/%s/%s", name, attr);

    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t r = write(fd, val, strlen(val));
    close(fd);
    return r < 0 ? -1 : 0;
}

static int sysfs_open_wr(const char *name, const char *attr)
{
    char path[256];
    snprintf(path, sizeof(path), UDMABUF_SYSFS_ROOT "/%s/%s", name, attr);
    return open(path, O_WRONLY);
}

int udmabuf_sysfs_info(const char *name, unsigned long *phys_addr, size_t *size)
{
    char path[256];
    char attr[64];
    struct stat sb;

    snprintf(path, sizeof(path), UDMABUF_SYSFS_ROOT "/%s", name);
    if (stat(path, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        fprintf(stderr, "u-dma-buf device '%s' not found in " UDMABUF_SYSFS_ROOT
                " (is the u-dma-buf module loaded?)\n", name);
        return -1;
    }

    if (sysfs_read(name, "phys_addr", attr, sizeof(attr)) < 0) {
        perror("read phys_addr");
        return -1;
    }
    if (sscanf(attr, "%lx", phys_addr) != 1) return -1;

    if (sysfs_read(name, "size", attr, sizeof(attr)) < 0) {
        perror("read size");
        return -1;
    }
    if (sscanf(attr, "%zu", size) != 1) return -1;

    return 0;
}

int udmabuf_open(udmabuf_t *b, const char *name, int cached)
{
    memset(b, 0, sizeof(*b));
    b->fd = b->sync_cpu_fd = b->sync_dev_fd = -1;
    b->sync_dir = -1;

    /* Tolerate a full device path as well as a bare name */
    const char *slash = strrchr(name, '/');
    if (slash) name = slash + 1;
    snprintf(b->name, sizeof(b->name), "%s", name);

    if (udmabuf_sysfs_info(b->name, &b->phys_addr, &b->size) < 0)
        return -1;

    char attr[16];
    if (sysfs_read(b->name, "dma_coherent", attr, sizeof(attr)) == 0)
        b->coherent = attr[0] == '1';

    if (cached && !b->coherent) {
        b->sync_cpu_fd = sysfs_open_wr(b->name, "sync_for_cpu");
        b->sync_dev_fd = sysfs_open_wr(b->name, "sync_for_device");
        if (b->sync_cpu_fd < 0 || b->sync_dev_fd < 0) {
            fprintf(stderr, "[~] udmabuf %s: sync attributes not writable, "
                    "mapping uncached\n", b->name);
            cached = 0;
        }
    }
    b->cached = cached;

    char dev[96];
    snprintf(dev, sizeof(dev), "/dev/%s", b->name);
    b->fd = open(dev, cached ? O_RDWR : (O_RDWR | O_SYNC));
    if (b->fd < 0) {
        perror(dev);
        udmabuf_close(b);
        return -1;
    }

    b->buf = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    if (b->buf == MAP_FAILED) {
        perror("mmap udmabuf");
        b->buf = NULL;
        udmabuf_close(b);
        return -1;
    }
    return 0;
}

void udmabuf_close(udmabuf_t *b)
{
    if (b->buf) munmap(b->buf, b->size);
    if (b->fd >= 0) close(b->fd);
    if (b->sync_cpu_fd >= 0) close(b->sync_cpu_fd);
    if (b->sync_dev_fd >= 0) close(b->sync_dev_fd);
    b->buf = NULL;
    b->fd = b->sync_cpu_fd = b->sync_dev_fd = -1;
}

/* Point the driver's sync window at [off, off+len) in direction dir. The
 * attributes persist in the driver, so they are only rewritten on change. */
static int sync_setup(udmabuf_t *b, size_t off, size_t len, udmabuf_dir_t dir)
{
    if (off > b->size || len > b->size - off) {
        fprintf(stderr, "udmabuf %s: sync [0x%zx, +0x%zx) out of the 0x%zx buffer\n",
                b->name, off, len, b->size);
        return -1;
    }
    if (off == b->sync_off && len == b->sync_len && (int)dir == b->sync_dir)
        return 0;

    char val[32];
    /* Shrink the size first: the driver checks offset+size against the
     * buffer on each write, so this order is valid for any old window */
    if (sysfs_write(b->name, "sync_size", "0") < 0) return -1;
    snprintf(val, sizeof(val), "%zu", off);
    if (sysfs_write(b->name, "sync_offset", val) < 0) return -1;
    snprintf(val, sizeof(val), "%zu", len);
    if (sysfs_write(b->name, "sync_size", val) < 0) return -1;
    snprintf(val, sizeof(val), "%d", (int)dir);
    if (sysfs_write(b->name, "sync_direction", val) < 0) return -1;

    b->sync_off = off;
    b->sync_len = len;
    b->sync_dir = dir;
    return 0;
}

static int sync_trigger(udmabuf_t *b, int fd, size_t off, size_t len,
                        udmabuf_dir_t dir, const char *what)
{
    if (!b->cached || b->coherent || len == 0)
        return 0;
    if (sync_setup(b, off, len, dir) < 0 || write(fd, "1", 1) != 1) {
        fprintf(stderr, "udmabuf %s: %s failed\n", b->name, what);
        b->sync_dir = -1;  // force a full rewrite next time
        return -1;
    }
    return 0;
}

int udmabuf_sync_for_cpu(udmabuf_t *b, size_t off, size_t len, udmabuf_dir_t dir)
{
    return sync_trigger(b, b->sync_cpu_fd, off, len, dir, "sync_for_cpu");
}

int udmabuf_sync_for_device(udmabuf_t *b, size_t off, size_t len, udmabuf_dir_t dir)
{
    return sync_trigger(b, b->sync_dev_fd, off, len, dir, "sync_for_device");
}
