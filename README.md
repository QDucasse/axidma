# AXI DMA test

This project sets up the AXI-DMA IP in a vivado design, showing how to self-test and use the S2MM channel only with an asynchronous FIFO. It is built and versioned using [vivgit](https://github.com/QDucasse/vivgit).


### Project Description

Three projects are present in this repository:
- `axidma`: a self-test design that loops MM2S through a FIFO back into S2MM, so both copies can be compared.
- `s2mm_async`: a pattern generator in its own clock domain feeds an asynchronous AXIS FIFO into S2MM, with only the S2MM channel active. It is the reference design for `s2mm_test`.
- `s2mm_async_ila`: `s2mm_async` plus two ILAs, one on each AXI-Stream side of the FIFO. It is an example of probing AXI-Stream interfaces and runs the same software.

### Installation

Clone the project and the `scripts` submodule:
```bash
git clone https://github.com/QDucasse/axidma
cd axidma
git submodule update --init --recursive
```

Create the project (use `axidma`, `s2mm_async` or `s2mm_async_ila`):
```bash
make PROJECT=<project-name> synth impl   # .bit/.bin/.xsa under build/<project-name>
make PROJECT=<project-name> sdt          # system device tree from the .xsa
```

`make help` lists the other targets (`overlay`/`dtbo` generate the device-tree overlay without petalinux, `deploy` stages the `.bin`/`.dtbo` to a board); see `scripts/README.md`.

From there, if you have a petalinux project:
```bash
cd <plnx_project>
petalinux-config --get-hw-description <path-to-axidma>/build/<project-name>/<project-name>.sdt
```

Add a reserved memory part using [`udmabuf`](https://github.com/ikwzm/udmabuf) in `<plnx>/project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi`:
```
/ {
    reserved-memory {
        #address-cells= <2>;
        #size-cells= <2>;
        ranges;
        dma_buffer: dma_buffer@68000000 {
            compatible = "shared-dma-pool";
            no-map;                                 // Incompatible with reusable
            reg = <0x0 0x68000000 0x0 0x04000000>;  // Address, size
            label = "dma_buffer";                   // Label to use
        };
    };

    udmabuf@60000000 {
        compatible = "ikwzm,u-dma-buf";
        device-name = "udmabuf0";                  // Name of the buffer
        size = <0x04000000>;                       // 64MiB
        memory-region = <&dma_buffer>;             // Link to the reserved-memory defined earlier
    };
};
```

You can then either load the module at run time using `insmod` or embed it in your petalinux project by adding a BitBake recipe:
```bash
petalinux-create modules --name u-dma-buf --enable --force
```

And replace its `files/u-dma-buf.c` with `udmabuf/u-dma-buf.c` from the [udmabuf](https://github.com/ikwzm/udmabuf) submodule.


You can then build the project:
```bash
petalinux-build
```

This build generates the first stage bootloader that powers the PL part, use it for your SD card or QSPI boot!

You then need to generate the overlay for the `pl.dtsi`:
```bash
cd <plnx_project>
dtc -@ -I dts -O dtb -o pl.dtbo project-spec/configs/pl.dtsi
```

> **WARNING:** From the `pl.dtsi`, modify the following line to the name of your `bin`:
> ```
> &fpga_full{
>     firmware-name = "axidma.bin";
> };
> ```
>
> You should also check the address of the `axi-dma` device:
> ```
> &amba{
>     axi_dma_0: dma@80000000 {
>         interrupts = <0x0 0x59 0x4 0x0 0x5a 0x4>;
>     ...
> ```

After booting, you can use `fpgautil` to load the `bin` version of the bitstream (placed under `build/axidma`) along with the device tree overlay:
```bash
cp pl.dtbo /lib/firmware/pl.dtbo
cp axidma.bin /lib/firmware/axidma.bin
fpgautil -b /lib/firmware/axidma.bin -o /lib/firmware/pl.dtbo
```

### Software

`src/lib/` is a small user-space library for the AXI DMA in simple (direct register) mode:

- `axi_regs`: maps an AXI-Lite register block through `/dev/mem`. It uses the same API as fuzzsight/decode-sight.
- `udmabuf`: finds a u-dma-buf device's physical address and size in sysfs, maps it cached or uncached, and does the cache maintenance (`sync_for_cpu`/`sync_for_device`) that a cached mapping needs.
- `axi_dma`: resets the core, starts/stops a channel, arms a transfer, and waits for completion with a timeout and decoded error bits. It also reads back the bytes S2MM actually wrote and drains stale stream data.

Three examples are built on it:

| Program | Design | What it does |
|---|---|---|
| `s2mm_test` | `s2mm_async`, `s2mm_async_ila` | Captures the pattern generator's counter through S2MM, one transfer per TLAST-terminated packet, and checks the counter for gaps. It waits for Enter before starting the generator, so the ILA can be armed (`-y` skips this). |
| `dma_test` | `axidma` | Loops a pseudo-random buffer MM2S → FIFO → S2MM and compares the two copies. |
| `dma_dump` | any | Dumps (part of) the u-dma-buf to a file. |

Build them on the board, or cross-compile:
```bash
cd src
make                                     # on the board
make CROSS_COMPILE=aarch64-linux-gnu-    # from a host
make CPPFLAGS='-DDMA_BASE=0xA0000000'    # if the block design addresses differ
```

```bash
root@xilinx-zcu104:~# ./dma_test -n 0x10000
root@xilinx-zcu104:~# ./s2mm_test -n 0x4000 -o capture.bin
```

The buffer is mapped cached by default and synced around each DMA. Pass `-u` to map it uncached (`O_SYNC`) instead. Both programs exit 0 on PASS.

Leave the DMA to these programs: if the `pl.dtsi` binds the `axi_dma` node to the kernel's `xilinx_dma` driver, that driver may reset the core or acknowledge its status bits while these programs are polling.

#### Notes:

The experiment was run on a ZynqMP UltraScale+ ZCU104, using Vivado 2024.2 and petalinux-2024.2, you might have to adapt:

- the board settings in `config.mk` (`PART_NAME`, `BOARD_NAME`, `BOARD_DTS`, `XILINX_VERSION`), whose commented defaults are the ZCU104 and 2024.2 values

- and the addresses from your block design, which the programs take as compile-time defines (`DMA_BASE`, default `0x80000000`; `GPIO_BASE`, default `0x80010000`). The buffer address no longer needs to be set: it is read from `/sys/class/u-dma-buf/<name>/phys_addr`.
