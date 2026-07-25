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
  | Application/UI | 1 | 432 words / 1728 bytes |
  | FreeRTOS idle | 0 | 144 words / 576 bytes |

- The firmware and NMSIS DSP library are built with `-fstack-usage`. With
  release LTO, the current direct frames are 176 bytes for `i2s_task`, 96 bytes
  for `usb_task`, 192 bytes for `Application_Task`, 320 bytes for `UI_Draw`,
  and 64 bytes for the idle task. Complete sizing also includes each function's
  callees, the 256-byte RISC-V integer/FPU switch frame, and margin for prebuilt
  newlib calls not represented in this target's `.su` records. Returning
  maskable ISRs use the 1.5 KiB startup stack as a dedicated post-scheduler
  interrupt stack; their assembly wrappers preserve the caller-saved FPU state
  before calling ordinary C bodies. The release/LTO compiler call graph reports
  about 688 bytes for the worst priority-compatible nested-interrupt path,
  leaving about 848 bytes of static margin in that stack.
- The kernel tick is 1 kHz, derived from a free-running 144 MHz CH32 SysTick.
  Tickless idle installs an absolute compare but intentionally busy-waits
  instead of executing `WFI`. The wait tests `CNT >= deadline` as well as
  enabled PFIC pending interrupts, so a missed equality compare cannot stall
  until the 64-bit counter wraps.
