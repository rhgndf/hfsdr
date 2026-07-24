cmake_minimum_required(VERSION 3.20)

set(CROSS_COMPILE riscv-none-elf-)
find_program(RISCV_GCC ${CROSS_COMPILE}gcc REQUIRED)
find_program(RISCV_GXX ${CROSS_COMPILE}g++ REQUIRED)
find_program(RISCV_AR ${CROSS_COMPILE}ar REQUIRED)
find_program(RISCV_SIZE ${CROSS_COMPILE}size REQUIRED)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_AR ${RISCV_AR} CACHE FILEPATH "Archiver" FORCE)
set(CMAKE_ASM_COMPILER ${RISCV_GCC} CACHE FILEPATH "ASM compiler" FORCE)
set(CMAKE_C_COMPILER ${RISCV_GCC} CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER ${RISCV_GXX} CACHE FILEPATH "C++ compiler" FORCE)

project(ch32v305_sdr
  LANGUAGES ASM C CXX
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RISCV_ARCH rv32imafc_zifencei)
set(RISCV_ABI ilp32f)

set(EXECUTABLE ${PROJECT_NAME}.elf)

set(NMSIS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/lib/NMSIS/NMSIS)
set(RISCV_ARCH ${RISCV_ARCH} CACHE STRING "NMSIS RISC-V architecture" FORCE)
set(RISCV_ABI ${RISCV_ABI} CACHE STRING "NMSIS RISC-V ABI" FORCE)
set(RISCV_MODEL medany CACHE STRING "NMSIS RISC-V code model" FORCE)
set(DISABLEFLOAT16 ON CACHE BOOL "Disable NMSIS float16 kernels" FORCE)
# Work around a lowercase ${project} reference in NMSIS DSP's CMakeLists.txt.
set(project NMSISDSP)
add_subdirectory(lib/NMSIS/NMSIS/DSP/Source)
target_compile_options(NMSISDSP PRIVATE
  -fstack-usage
  -Wstack-usage=512
)

file(GLOB SRC_FILES CONFIGURE_DEPENDS
  lib/Peripheral/src/*.c
)

file(GLOB_RECURSE SRC_FILES_RECURSE CONFIGURE_DEPENDS
  src/*.c
  src/test/*.c
  src/*.cpp
  lib/tinyusb/src/*.c
)

set(FATFS_SOURCES
  lib/fatfs/source/ff.c
  lib/fatfs/source/ffunicode.c
)

set(FREERTOS_KERNEL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/lib/FreeRTOS-Kernel)
set(FREERTOS_SOURCES
  ${FREERTOS_KERNEL_ROOT}/tasks.c
  ${FREERTOS_KERNEL_ROOT}/queue.c
  ${FREERTOS_KERNEL_ROOT}/list.c
  ${FREERTOS_KERNEL_ROOT}/portable/GCC/RISC-V/port.c
  ${FREERTOS_KERNEL_ROOT}/portable/GCC/RISC-V/portASM.S
  src/freertos/port_ch32v30x.S
)

add_executable(${EXECUTABLE} 
  lib/Startup/startup_ch32v30x_D8C.S
  lib/Core/core_riscv.c
  
  ${SRC_FILES}
  ${SRC_FILES_RECURSE}
  ${FATFS_SOURCES}
  ${FREERTOS_SOURCES}

)

target_include_directories(${EXECUTABLE} PRIVATE 
  src
  src/freertos
  ${FREERTOS_KERNEL_ROOT}/include
  ${FREERTOS_KERNEL_ROOT}/portable/GCC/RISC-V
  ${FREERTOS_KERNEL_ROOT}/portable/GCC/RISC-V/chip_specific_extensions/RISCV_no_extensions
  lib/Core/
  lib/Debug/
  lib/Peripheral/inc
  lib/fatfs/source
  lib/tinyusb/src
  ${NMSIS_ROOT}/Core/Include
  ${NMSIS_ROOT}/DSP/Include
)

target_compile_definitions(${EXECUTABLE} PRIVATE
  -DCH32V30x_D8C
  -DHSE_VALUE=24000000
  -DCFG_TUD_WCH_USBIP_USBHS=1
  -DCFG_TUSB_MCU=OPT_MCU_CH32V307
  configENABLE_FPU=1
  configENABLE_VPU=0
)

target_compile_options(${EXECUTABLE} PRIVATE
  -march=${RISCV_ARCH}
  -mabi=${RISCV_ABI}
  #-msmall-data-limit=8
  #-mno-save-restore
  
  #-fmessage-length=0 
  #-fsigned-char

  -fdata-sections
  -ffunction-sections
  -fstack-usage
  -Wstack-usage=512

  -Wunused -Wuninitialized

  -MMD -MP
  -flto=2
  -Wall
  -g3
  -Ofast
)


set(LINKER_SCRIPT  ${CMAKE_SOURCE_DIR}/lib/Ld/Link.ld)
set_property(TARGET ${EXECUTABLE} APPEND PROPERTY LINK_DEPENDS ${LINKER_SCRIPT})

target_link_options(${EXECUTABLE} PRIVATE
  -T ${LINKER_SCRIPT}
  -march=${RISCV_ARCH}
  -mabi=${RISCV_ABI}
  #-msmall-data-limit=8
  #-mno-save-restore
  
  #-fmessage-length=0 
  #-fsigned-char

  -fdata-sections
  -ffunction-sections
  -fstack-usage
  -Wstack-usage=512

  -Wunused -Wuninitialized
  
  --specs=nosys.specs
  --specs=nano.specs
  
  -nostartfiles
  
  -lc
  -lm
  -lstdc++
  -lsupc++
  -ffreestanding
  -flto=2
  -Wl,-Map=${PROJECT_NAME}.map,--cref
  -Wl,--gc-sections
  -Wl,--print-memory-usage
)

target_link_libraries(${EXECUTABLE} PRIVATE
  NMSISDSP
)

add_custom_command(TARGET ${EXECUTABLE} 
    POST_BUILD
    COMMAND ${RISCV_SIZE} ARGS ${EXECUTABLE}
)
