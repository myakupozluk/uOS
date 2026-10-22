# uOS

uOS is an experimental AArch64 operating system for the Raspberry Pi 4.

The kernel currently includes early boot code, UART and GPIO drivers, memory
management, threading, a VFS, and FAT32 support. It is still under active
development and is not production-ready.

## Requirements

- Raspberry Pi 4
- An AArch64 bare-metal GCC toolchain (`aarch64-none-elf-gcc`)
- `uBuild`

## Build

From the repository root:

```sh
ubuild uBuild.u
```

The build produces `build/uOSkernel8.img`. The image can be used with a
Raspberry Pi 4 boot setup.

The build configuration targets the Cortex-A72 and uses the linker script at
`src/link.ld` to define the kernel and memory layout.

## Status

This project is a work in progress. Hardware support and filesystem behavior
may change as the kernel develops.
