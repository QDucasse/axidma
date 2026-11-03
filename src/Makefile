# Build the AXI DMA library and examples.
#   on the board:      make
#   cross-compiling:   make CROSS_COMPILE=aarch64-linux-gnu-
# Addresses from the block design can be overridden, e.g.
#   make CPPFLAGS='-DDMA_BASE=0xA0000000'

CC       = $(CROSS_COMPILE)gcc
AR       = $(CROSS_COMPILE)ar
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra
CPPFLAGS += -I.

LIB_SRCS = lib/axi_regs.c lib/udmabuf.c lib/axi_dma.c
LIB_OBJS = $(LIB_SRCS:.c=.o)
LIB      = lib/libaxidma.a
EXAMPLES = s2mm_test dma_test dma_dump

all: $(EXAMPLES)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(EXAMPLES): %: %.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c $(wildcard lib/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(EXAMPLES) *.o $(LIB_OBJS) $(LIB)

.PHONY: all clean
