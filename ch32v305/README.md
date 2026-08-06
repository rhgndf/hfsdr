# ch32v305

Firmware build for the CH32V305 SDR target.

## Requirements

- `cmake` 3.20 or newer
- `ninja`
- RISC-V GCC bare-metal toolchain in `PATH`
  - expected tools: `riscv-none-elf-gcc`, `riscv-none-elf-g++`, `riscv-none-elf-ar`, `riscv-none-elf-size`

## Build With Ninja

Configure the project with the Ninja generator:

```bash
cmake -S . -B build -G Ninja
```

Build it:

```bash
cmake --build build
```

The main output is:

- `build/ch32v305_sdr.elf`

The linker also emits:

- `build/ch32v305_sdr.map`

## Flash

From the build directory:

```bash
cd build
wchisp flash ch32v305_sdr.elf
```

## Notes

- Link-time output includes `--print-memory-usage`.
- A post-build `riscv-none-elf-size` report is also printed.
- FreeRTOS Kernel is a submodule pinned to V11.3.0. Initialize submodules before
  building a fresh checkout:

  ```bash
  git submodule update --init --recursive
  ```
- All FreeRTOS objects and tasks use static allocation. The task layout is:

  | Task | Priority | Stack |
  | --- | ---: | ---: |
  | I2S processing | 3 | 192 words / 768 bytes |
  | TinyUSB device | 2 | 256 words / 1024 bytes |
  | Hardware state | 2 | 256 words / 1024 bytes |
  | Recording | 2 | 432 words / 1728 bytes |
  | Application/UI | 1 | 432 words / 1728 bytes |
  | FreeRTOS idle | 0 | 144 words / 576 bytes |
