// axi_regs.c
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // clock_gettime under -std=c11
#endif
#include "axi_regs.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Open /dev/mem and map corresponding address for the AXI-Lite */
int axi_regs_open(axi_regs_t *handle, uintptr_t phys_addr, size_t map_size)
{
    handle->phys_addr = phys_addr;
    handle->map_size  = map_size;
    handle->regs      = NULL;

    handle->fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (handle->fd < 0) {
        perror("open /dev/mem");
        return -1;
    }

    void *map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     handle->fd, (off_t)phys_addr);
    if (map == MAP_FAILED) {
        perror("mmap /dev/mem");
        close(handle->fd);
        handle->fd = -1;
        return -1;
    }

    handle->regs = (volatile uint32_t *)map;
    return 0;
}

/* Close mapped /dev/mem portion */
void axi_regs_close(axi_regs_t *handle)
{
    if (handle->regs)
        munmap((void *)handle->regs, handle->map_size);
    if (handle->fd >= 0)
        close(handle->fd);
    handle->regs = NULL;
    handle->fd   = -1;
}

/* Read value at offset */
uint32_t axi_regs_read(axi_regs_t *handle, uint32_t offset)
{
    return handle->regs[offset / 4];
}

/* Write value at offset */
void axi_regs_write(axi_regs_t *handle, uint32_t offset, uint32_t value)
{
    handle->regs[offset / 4] = value;
}

/* Print read values as defined in the descriptions */
void axi_regs_print(axi_regs_t *handle, const axi_reg_desc_t *descs, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%-25s : 0x%08x\n", descs[i].name,
               axi_regs_read(handle, descs[i].offset));
}

double axi_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int axi_regs_poll(axi_regs_t *handle, uint32_t offset, uint32_t mask,
                  uint32_t value, double timeout_s)
{
    double deadline = timeout_s > 0 ? axi_now() + timeout_s : 0;

    for (;;) {
        /* Check the clock only every 1024 reads, the read itself is cheap */
        for (int i = 0; i < 1024; i++)
            if ((axi_regs_read(handle, offset) & mask) == value)
                return 0;
        if (timeout_s > 0 && axi_now() > deadline)
            return (axi_regs_read(handle, offset) & mask) == value ? 0 : -1;
    }
}
