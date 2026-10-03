OUTPUT_FORMAT("elf64-littleaarch64")

/* RPi4 2GB actual memory layout */
TOTAL_RAM = 0x80000000;                    /* 2GB physical */
VIDEOCORE_RESERVED = 16 * 0x100000;        /* 16MB reserved by firmware at boot */
GPU_MEM_MB = 76;                            /* gpu_mem setting in config.txt */
GPU_MEM = GPU_MEM_MB * 0x100000;

/* Actually available to ARM CPU */
ARM_RAM_START = 0x00000000;
ARM_RAM_END = TOTAL_RAM - VIDEOCORE_RESERVED - GPU_MEM;
ARM_RAM_SIZE = ARM_RAM_END - ARM_RAM_START;

/*
 * Video heap occupies the lower part of RAM up to exactly 1 GiB.
 *
 * Kernel itself is also located below this boundary, so video heap
 * starts immediately after the kernel/bss.
 */
VIDEO_HEAP_END = 0x40000000;               /* 1 GiB */

/* Allocate memory regions */
KERNEL_STACK_SIZE = 128 * 0x400;            /* 128KB per core */
NUM_CORES = 4;
TOTAL_STACK = KERNEL_STACK_SIZE * NUM_CORES; /* 512KB total */


SECTIONS
{
    /*
     * Kernel
     */
    . = 0x80000;

    __kernel_start = .;
    __kernel_text_start__ = .;

    .text.boot :
    {
        KEEP(*(.text.boot))
    }

    .text :
    {
        *(.text*)
    }

    . = ALIGN(4096);
    __kernel_text_end__ = .;


    /*
     * Read-only data
     */
    __kernel_rodata_start__ = .;

    .rodata :
    {
        *(.rodata .rodata.* .gnu.linkonce.r*)
    }

    . = ALIGN(4096);
    __kernel_rodata_end__ = .;


    /*
     * Writable data
     */
    __kernel_data_start__ = .;

    .got :
    {
        *(.got .got.*)
    }

    PROVIDE(_data = .);

    .data :
    {
        *(.data .data.* .gnu.linkonce.d*)
    }

    . = ALIGN(4096);
    __kernel_data_end__ = .;


    /*
     * BSS
     */
    .bss (NOLOAD) : ALIGN(4096)
    {
        . = ALIGN(4096);

        __bss_start = .;

        *(.bss.heap_ctx)
        *(.bss .bss.*)
        *(COMMON)

        . = ALIGN(4096);

        __bss_end = .;
    }


    /*
     * End of kernel
     */
    __end = .;
    PROVIDE(__kernel_end = .);


    /*
     * ============================================================
     * VIDEO HEAP
     * ============================================================
     *
     * Starts immediately after the kernel and extends to 1 GiB.
     */
    . = ALIGN(4096);

    __vheap_base = .;

    . = VIDEO_HEAP_END;

    __vheap_end = .;


    /*
     * ============================================================
     * NORMAL HEAP
     * ============================================================
     *
     * Starts at 1 GiB and consumes all remaining RAM below stacks.
     */
    . = ALIGN(4096);

    __heap_base = .;

    . = ARM_RAM_END - TOTAL_STACK;

    __heap_end = .;


    /*
     * ============================================================
     * KERNEL STACKS
     * ============================================================
     *
     * Located at the very top of usable ARM RAM.
     */
    . = ALIGN(4096);

    __stack_start = .;


    __stack0_bottom = .;
    . += KERNEL_STACK_SIZE;
    __stack0_top = .;


    __stack1_bottom = .;
    . += KERNEL_STACK_SIZE;
    __stack1_top = .;


    __stack2_bottom = .;
    . += KERNEL_STACK_SIZE;
    __stack2_top = .;


    __stack3_bottom = .;
    . += KERNEL_STACK_SIZE;
    __stack3_top = .;


    __stack_end = .;


    /*
     * ============================================================
     * SANITY CHECKS
     * ============================================================
     */

    ASSERT(
        __stack_end <= ARM_RAM_END,
        "ERROR: Memory layout exceeds available ARM RAM!"
    )

    ASSERT(
        __vheap_end > __vheap_base,
        "ERROR: Video heap has negative size!"
    )

    ASSERT(
        __heap_end > __heap_base,
        "ERROR: Normal heap has negative size!"
    )

    ASSERT(
        __vheap_end <= ARM_RAM_END,
        "ERROR: Video heap exceeds ARM RAM!"
    )

    ASSERT(
        __stack_end - 0x80000 <= ARM_RAM_SIZE,
        "ERROR: Total memory usage exceeds available RAM!"
    )


    /DISCARD/ :
    {
        *(.comment)
        *(.gnu*)
        *(.note*)
        *(.eh_frame*)
    }
}


/*
 * BSS size
 */
__bss_size = (__bss_end - __bss_start);


/*
 * ============================================================
 * MEMORY MAP EXPORTS
 * ============================================================
 */

PROVIDE(__arm_ram_end = ARM_RAM_END);
PROVIDE(__arm_ram_size = ARM_RAM_SIZE);

PROVIDE(__total_ram = TOTAL_RAM);
PROVIDE(__videocore_reserved = VIDEOCORE_RESERVED);
PROVIDE(__gpu_mem = GPU_MEM);

PROVIDE(__video_heap_end = VIDEO_HEAP_END);


/*
 * Default to core 0 stack
 */
PROVIDE(__stack_top = __stack0_top);
PROVIDE(__stack_bottom = __stack0_bottom);


/*
 * Heap sizes
 */
PROVIDE(__vheap_size = __vheap_end - __vheap_base);
PROVIDE(__heap_size = __heap_end - __heap_base);