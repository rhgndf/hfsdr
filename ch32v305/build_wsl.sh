#!/bin/bash
set -e
export PATH="$PATH:/home/hacker/.local/xPacks/@xpack-dev-tools/riscv-none-elf-gcc/15.2.0-1.1/.content/bin/"
cmake --build "/mnt/c/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr-public/ch32v305/build" -j
