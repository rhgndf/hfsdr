# HFSDR agents file

## Dev instructions
- Read `ch32v305/README.md` to find out the instructions on how to build the firmware
- Use C23 features for C files. Declare variables where it is needed, not all at the top of the function.
- Use C++23/26 features for the C files. Suggest to refactor code to use those features if necessary.
- If appropriate, suggest to the developer that using C++23 or newer might simplify and make the code more maintainable
- Try to fix as many warnings as possible. If it is a stack space warning, signal to the developer and suggest methods to remove the warning without increasing too much RAM usage.

## Optimization instructions
- Run objdump or any tool available to view the assembly of the output elf file
- We are on RV32IMAFC, so multiply is single cycle, but divide is around 5 cycles.
- Flash is zero-wait up to 128kb