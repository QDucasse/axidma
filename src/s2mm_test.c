/*
* Copyright (C) 2013 - 2016  Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person
* obtaining a copy of this software and associated documentation
* files (the "Software"), to deal in the Software without restriction,
* including without limitation the rights to use, copy, modify, merge,
* publish, distribute, sublicense, and/or sell copies of the Software,
* and to permit persons to whom the Software is furnished to do so,
* subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
* CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in this
* Software without prior written authorization from Xilinx.
*
*/


/*
* This test file sets up the S2MM channel and waits for the CoreSight trace.
*
*/


#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <sys/mman.h>

#define DMA_BASE  0x80000000
#define DMA_SIZE  0x10000
#define S2MM_BASE 0x68000000
#define S2MM_SIZE 0x04000000
#define GPIO_BASE 0x80010000
#define GPIO_SIZE 0x10000   // 64 KB AXI GPIO range

#define S2MM_CONTROL_REGISTER       0x30
#define S2MM_STATUS_REGISTER        0x34
#define S2MM_DST_ADDRESS_REGISTER   0x48
#define S2MM_BUFF_LENGTH_REGISTER   0x58

#define IOC_IRQ_FLAG                1<<12
#define IDLE_FLAG                   1<<1

#define STATUS_HALTED               0x00000001
#define STATUS_IDLE                 0x00000002
#define STATUS_SG_INCLDED           0x00000008
#define STATUS_DMA_INTERNAL_ERR     0x00000010
#define STATUS_DMA_SLAVE_ERR        0x00000020
#define STATUS_DMA_DECODE_ERR       0x00000040
#define STATUS_SG_INTERNAL_ERR      0x00000100
#define STATUS_SG_SLAVE_ERR         0x00000200
#define STATUS_SG_DECODE_ERR        0x00000400
#define STATUS_IOC_IRQ              0x00001000
#define STATUS_DELAY_IRQ            0x00002000
#define STATUS_ERR_IRQ              0x00004000

#define HALT_DMA                    0x00000000
#define RUN_DMA                     0x00000001
#define RESET_DMA                   0x00000004
#define ENABLE_IOC_IRQ              0x00001000
#define ENABLE_DELAY_IRQ            0x00002000
#define ENABLE_ERR_IRQ              0x00004000
#define ENABLE_ALL_IRQ              0x00007000

#define CHUNK_SIZE                 32 // Must match AXI-DMA value
#define TRANSFER_SIZE              0x000004000

unsigned int write_dma(unsigned int *virtual_addr, int offset, unsigned int value)
{
    virtual_addr[offset>>2] = value;

    return 0;
}

unsigned int read_dma(unsigned int *virtual_addr, int offset)
{
    return virtual_addr[offset>>2];
}

void dma_s2mm_status(unsigned int *virtual_addr)
{
    unsigned int status = read_dma(virtual_addr, S2MM_STATUS_REGISTER);

    printf("Stream to memory-mapped status (0x%08x@0x%02x):", status, S2MM_STATUS_REGISTER);

    if (status & STATUS_HALTED) {
        printf(" Halted.\n");
    } else {
        printf(" Running.\n");
    }

    if (status & STATUS_IDLE) {
        printf(" Idle.\n");
    }

    if (status & STATUS_SG_INCLDED) {
        printf(" SG is included.\n");
    }

    if (status & STATUS_DMA_INTERNAL_ERR) {
        printf(" DMA internal error.\n");
    }

    if (status & STATUS_DMA_SLAVE_ERR) {
        printf(" DMA slave error.\n");
    }

    if (status & STATUS_DMA_DECODE_ERR) {
        printf(" DMA decode error.\n");
    }

    if (status & STATUS_SG_INTERNAL_ERR) {
        printf(" SG internal error.\n");
    }

    if (status & STATUS_SG_SLAVE_ERR) {
        printf(" SG slave error.\n");
    }

    if (status & STATUS_SG_DECODE_ERR) {
        printf(" SG decode error.\n");
    }

    if (status & STATUS_IOC_IRQ) {
        printf(" IOC interrupt occurred.\n");
    }

    if (status & STATUS_DELAY_IRQ) {
        printf(" Interrupt on delay occurred.\n");
    }

    if (status & STATUS_ERR_IRQ) {
        printf(" Error interrupt occurred.\n");
    }
}

int dma_s2mm_sync(unsigned int *virtual_addr)
{
    unsigned int s2mm_status = read_dma(virtual_addr, S2MM_STATUS_REGISTER);

    // sit in this while loop as long as the status does not read back 0x00001002 (4098)
    // 0x00001002 = IOC interrupt has occured and DMA is idle
    while(!(s2mm_status & IOC_IRQ_FLAG) || !(s2mm_status & IDLE_FLAG))
    {
        dma_s2mm_status(virtual_addr);
        // dma_mm2s_status(virtual_addr);

        s2mm_status = read_dma(virtual_addr, S2MM_STATUS_REGISTER);
    }

    return 0;
}

void print_mem(void *virtual_address, int byte_count)
{
    char *data_ptr = virtual_address;

    for(int i=0;i<byte_count;i++){
        printf("%02X", data_ptr[i]);

        // print a space every 4 bytes (0 indexed)
        if(i%4==3){
            printf(" ");
        }
    }

    printf("\n");
}

int main()
{
    printf("Hello World! - Running TPIU AXI-DMA transfer test application.\n");

    printf("Opening a character device file of the ZynqMP's DDR memory...\n");
    int ddr_memory = open("/dev/mem", O_RDWR | O_SYNC);

    printf("Memory map the address of the DMA AXI IP via its AXI lite control interface register block.\n");
    unsigned int *dma_virtual_addr = mmap(NULL, DMA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, ddr_memory, DMA_BASE);


    printf("Opening u-dma-buf character device for S2MM destination...\n");
    int dma_buf_fd = open("/dev/tpiu-doctor0", O_RDWR); // use the device name exported by u-dma-buf
    if(dma_buf_fd < 0) {
        perror("Failed to open u-dma-buf device");
        return -1;
    }

    unsigned int *virtual_dst_addr = mmap(NULL, S2MM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, dma_buf_fd, 0);
    if(virtual_dst_addr == MAP_FAILED) {
        perror("Failed to mmap u-dma-buf memory");
        return -1;
    }

     // Map AXI GPIO
    unsigned int *gpio = mmap(NULL, GPIO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, ddr_memory, GPIO_BASE);
    if(gpio == MAP_FAILED) { perror("Failed to mmap GPIO"); return -1; }

    // Clearing buffer via the u-dma-buf mapping
    memset(virtual_dst_addr, 0, S2MM_SIZE);
    printf("Destination memory block data (first 8 bytes): ");
    print_mem(virtual_dst_addr, 32);

    printf("Reset the DMA.\n");
    write_dma(dma_virtual_addr, S2MM_CONTROL_REGISTER, RESET_DMA);
    dma_s2mm_status(dma_virtual_addr);

    printf("Halt the DMA.\n");
    write_dma(dma_virtual_addr, S2MM_CONTROL_REGISTER, HALT_DMA);
    dma_s2mm_status(dma_virtual_addr);

    printf("Enable all interrupts.\n");
    write_dma(dma_virtual_addr, S2MM_CONTROL_REGISTER, ENABLE_ALL_IRQ);
    unsigned int control = read_dma(dma_virtual_addr, S2MM_CONTROL_REGISTER);
    printf("Stream to memory-mapped control (0x%08x@0x%02x)...\n", control, S2MM_CONTROL_REGISTER);
    dma_s2mm_status(dma_virtual_addr);

    printf("Writing the destination address for the data from S2MM in DDR: 0x%x\n", S2MM_BASE);
    write_dma(dma_virtual_addr, S2MM_DST_ADDRESS_REGISTER, S2MM_BASE);
    dma_s2mm_status(dma_virtual_addr);

    printf("Start running.\n");
    write_dma(dma_virtual_addr, S2MM_CONTROL_REGISTER, RUN_DMA);
    control = read_dma(dma_virtual_addr, S2MM_CONTROL_REGISTER);
    printf("Stream to memory-mapped control (0x%08x@0x%02x)...\n", control, S2MM_CONTROL_REGISTER);
    dma_s2mm_status(dma_virtual_addr);

    printf("Writing S2MM chunk size of 0x%x bytes...\n", CHUNK_SIZE);
    write_dma(dma_virtual_addr, S2MM_BUFF_LENGTH_REGISTER, CHUNK_SIZE);
    dma_s2mm_status(dma_virtual_addr);

    printf("Launching generator.\n");
    gpio[0] = 1;   // Pattern generator

    unsigned int offset = 0;
    unsigned int dst_addr = S2MM_BASE;

    while(offset <= 0x000004000) {

        printf("Waiting for chunk transfer...\n");
        dma_s2mm_sync(dma_virtual_addr);


        // Move to next chunk (wrap around at 64 MB)
        offset += CHUNK_SIZE;
        if (offset >= S2MM_SIZE) offset = 0;
        dst_addr = S2MM_BASE + offset;

        printf("Rewriting destination address (0x%x) and chunk size.\n", dst_addr);
        write_dma(dma_virtual_addr, S2MM_DST_ADDRESS_REGISTER, dst_addr);
        write_dma(dma_virtual_addr, S2MM_BUFF_LENGTH_REGISTER, CHUNK_SIZE);
        dma_s2mm_status(dma_virtual_addr);


        printf("Restarting DMA...\n");
        write_dma(dma_virtual_addr, S2MM_CONTROL_REGISTER, RUN_DMA | ENABLE_ALL_IRQ);

    }

    printf("Wrote 0x%x bytes.\n", offset);

    print_mem(virtual_dst_addr, TRANSFER_SIZE);
    dma_s2mm_status(dma_virtual_addr);

    return 0;
}
