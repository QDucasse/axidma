#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BUF_SIZE 0x04000000

int main() {
    int fd = open("/dev/udmabuf0", O_RDONLY);
    if(fd < 0) { perror("open"); return -1; }

    void *buf = mmap(NULL, BUF_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    if(buf == MAP_FAILED) { perror("mmap"); return -1; }

    int out = open("dma_dump.bin", O_CREAT | O_WRONLY, 0666);
    if(out < 0) { perror("open output"); return -1; }

    ssize_t written = write(out, buf, BUF_SIZE);
    if(written != BUF_SIZE) { perror("write"); }

    munmap(buf, BUF_SIZE);
    close(fd);
    close(out);

    printf("DMA buffer dumped to dma_dump.bin\n");
    return 0;
}