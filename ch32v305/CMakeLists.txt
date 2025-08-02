cmake_minimum_required(VERSION 3.20)

project(ch32v305_sdr
  LANGUAGES ASM C CXX
)

set(CROSS_COMPILE riscv-none-elf-)

set(CMAKE_AR ${CROSS_COMPILE}ar)
set(CMAKE_ASM_COMPILER ${CROSS_COMPILE}gcc)
set(CMAKE_C_COMPILER ${CROSS_COMPILE}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}g++)

set(EXECUTABLE ${PROJECT_NAME}.elf)

file(GLOB SRC_FILES CONFIGURE_DEPENDS
  lib/Peripheral/src/*.c
)

file(GLOB_RECURSE SRC_FILES_RECURSE CONFIGURE_DEPENDS
  src/*.c
  lib/tinyusb/src/*.c
)

add_executable(${EXECUTABLE} 
  lib/Startup/startup_ch32v30x_D8C.S
  lib/Core/core_riscv.c
  lib/Debug/debug.c
  
  ${SRC_FILES}
  ${SRC_FILES_RECURSE}

  lib/tinyusb/src/tusb.c
)

target_include_directories(${EXECUTABLE} PRIVATE 
  src
  lib/Core/
  lib/Debug/
  lib/Peripheral/inc
  lib/tinyusb/src
)

target_compile_definitions(${EXECUTABLE} PRIVATE
  -DCH32V30x_D8C
  -DCFG_TUD_WCH_USBIP_USBHS=1
  -DCFG_TUSB_MCU=OPT_MCU_CH32V307
)

target_compile_options(${EXECUTABLE} PRIVATE
  -march=rv32imafc_zifencei
  -mabi=ilp32f
  #-msmall-data-limit=8
  #-mno-save-restore
  
  #-fmessage-length=0 
  #-fsigned-char

  -fdata-sections
  -ffunction-sections

  -Wunused -Wuninitialized

  -std=gnu99 -MMD -MP
  -flto
  -Wall
  -g3
  -Ofast
)


set(LINKER_SCRIPT  ${CMAKE_SOURCE_DIR}/lib/Ld/Link.ld)

target_link_options(${EXECUTABLE} PRIVATE
  -T ${LINKER_SCRIPT}
  -march=rv32imafc
  -mabi=ilp32f
  #-msmall-data-limit=8
  #-mno-save-restore
  
  #-fmessage-length=0 
  #-fsigned-char

  -fdata-sections
  -ffunction-sections

  -Wunused -Wuninitialized
  
  --specs=nano.specs 
  --specs=nosys.specs
  
  -nostartfiles
  
  -lc
  -lm
  -lstdc++
  -lsupc++
  -ffreestanding
  -flto
  -Wl,-Map=${PROJECT_NAME}.map,--cref
  -Wl,--gc-sections
  #-Xlinker -print-memory-usage -Xlinker
)

add_custom_command(TARGET ${EXECUTABLE} 
    POST_BUILD
    COMMAND ${CROSS_COMPILE}size ARGS ${EXECUTABLE}
)