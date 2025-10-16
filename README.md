# AXI DMA test

This project sets up the AXI-DMA IP in a vivado design. It is built and versioned using [vivgit](https://github.com/QDucasse/vivgit).


### Installation

Clone the project and the `scripts` submodule:
```bash
git clone https://github.com/QDucasse/axidma
cd axidma
git submodule update --init --recursive
```

Create the project:
```bash
vivado -mode batch scripts/create_project.tcl -tclargs axidma
vivado -mode batch scripts/run_synth_impl.tcl -tclargs axidma
```

From there, if you have a petalinux project:
```bash
cd <plnx_project>
petalinux-config --get-hw-description <path-to-axidma>/build/axidma/axidma.sdt
```

Add a reserved memory part using [`udmabuf`](https://github.com/ikwzm/udmabuf) in `<plnx>/project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi`:
```
/ {
    reserved-memory {
        #address-cells= <2>;
        #size-cells= <2>;
        ranges;
        axi_dma_buffer: axi_dma_buffer@68000000 {
            compatible = "shared-dma-pool";
            no-map;                                 // Incompatible with reusable
            reg = <0x0 0x68000000 0x0 0x04000000>;  // Address, size (64MiB here)
            label = "tpiu_buffer";                  // Label to use
        };
    };
    udmabuf@68000000 {
        compatible = "ikwzm,u-dma-buf";            // Name used in the driver
        device-name = "axi_dma_buffer";            // Name of the buffer
        size = <0x04000000>;                       // 64MiB
        memory-region = <&axi_dma_buffer>;         // Link to the reserved-memory defined earlier
    };
};
```

You can then either load the module at run time using `insmod` or embed it in your petalinux project by adding a BitBake recipe:
```bash
petalinux-create modules --name u-dma-buf --enable --force
```

And replace the `files/u-dma-buf.c` with the one in the repository!


You can then build the project:
```bash
petalinux-build
```

This build generates the first stage bootloader that powers the PL part, use it for your SD card or QSPI boot!

You then need to generate the overlay for the `pl.dtsi`:
```bash
cd <plnx_project>
dtc -@ -I dts -O dtb -o pl.dtbo components/plnx_workspace/device-tree/pl-overlay-full/pl.dtsi
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

You can then run the test program found in `src/dma_test.c`, taken from [this tutorial](https://www.hackster.io/whitney-knitter/introduction-to-using-axi-dma-in-embedded-linux-5264ec):

```bash
root@xilinx-zcu104:/home/root# gcc -O2 -o dma_test dma_test.c
root@xilinx-zcu104:/home/root# ./dma_test
Hello World! - Running DMA transfer test application.
Opening a character device file of the Arty's DDR memeory...
Memory map the address of the DMA AXI IP via its AXI lite control interface register block.
Memory map the MM2S source address register block.
Memory map the S2MM destination address register block.
Writing random data to source register block...
Clearing the destination register block...
Source memory block data:      DEADBEEF 44332211 ABABABAB CDCDCDCD 11110000 33332222 55554444 77776666
Destination memory block data: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
Reset the DMA.
Stream to memory-mapped status (0x00000001@0x34): Halted.
Memory-mapped to stream status (0x00000001@0x04): Halted.
Halt the DMA.
Stream to memory-mapped status (0x00000001@0x34): Halted.
Memory-mapped to stream status (0x00000001@0x04): Halted.
Enable all interrupts.
Stream to memory-mapped status (0x00000001@0x34): Halted.
Memory-mapped to stream status (0x00000001@0x04): Halted.
Writing source address of the data from MM2S in DDR...
Memory-mapped to stream status (0x00000001@0x04): Halted.
Writing the destination address for the data from S2MM in DDR...
Stream to memory-mapped status (0x00000001@0x34): Halted.
Run the MM2S channel.
Memory-mapped to stream status (0x00000000@0x04): Running.
Run the S2MM channel.
Stream to memory-mapped status (0x00000000@0x34): Running.
Writing MM2S transfer length of 32 bytes...
Memory-mapped to stream status (0x00000000@0x04): Running.
Writing S2MM transfer length of 32 bytes...
Stream to memory-mapped status (0x00000000@0x34): Running.
Waiting for MM2S synchronization...
Waiting for S2MM sychronization...
Stream to memory-mapped status (0x00001002@0x34): Running.
 Idle.
 IOC interrupt occurred.
Memory-mapped to stream status (0x00001002@0x04): Running.
 Idle.
 IOC interrupt occurred.
Destination memory block: DEADBEEF 44332211 ABABABAB CDCDCDCD 11110000 33332222 55554444 77776666
```


#### Notes:

The experiment was run on a ZynqMP UltraScale+ ZCU104, using Vivado 2024.2 and petalinux-2024.2, you might have to adapt:

- the part name
    ```bash
    # in scripts/create_project.tcl
    set part_name "xczu7ev-ffvc1156-2-e"
    ```

- the board dts ()
    ```bash
    # in scripts/run_synth_impl.tcl
    set board_dts "zcu104-revc"
    ```

- and the address of your `axi_dma` device from the device tree that is then used in the test script:
    ```c
    #define DMA_BASE  0x80000000 // Address of the DMA registers
    #define MM2S_BASE 0x68000000 // Address of the DMA buffer
    #define S2MM_BASE 0x68010000 // Still falls in the DMA buffer!
    ```