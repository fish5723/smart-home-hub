# Toolchain file for ARM Cortex-M3 cross-compilation (STM32F103)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR ARM)

# Find the ARM GCC toolchain
find_program(CMAKE_C_COMPILER arm-none-eabi-gcc)
find_program(CMAKE_CXX_COMPILER arm-none-eabi-g++)
find_program(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
find_program(CMAKE_SIZE  arm-none-eabi-size)

# If not found in PATH, try common STM32Cube locations
if(NOT CMAKE_C_COMPILER)
    set(ARM_TOOLCHAIN_DIR "C:/Program Files (x86)/GNU Tools ARM Embedded/9 2020-q2-update/bin")
    set(CMAKE_C_COMPILER   "${ARM_TOOLCHAIN_DIR}/arm-none-eabi-gcc.exe")
    set(CMAKE_CXX_COMPILER "${ARM_TOOLCHAIN_DIR}/arm-none-eabi-g++.exe")
    set(CMAKE_ASM_COMPILER "${ARM_TOOLCHAIN_DIR}/arm-none-eabi-gcc.exe")
endif()

# Target CPU flags for STM32F103 (Cortex-M3)
set(MCU_FLAGS "-mcpu=cortex-m3 -mthumb -mfloat-abi=soft")

# Compile flags: separate into sections + optimize for size
set(CMAKE_C_FLAGS   "${MCU_FLAGS} -DSTM32F103xB -ffunction-sections -fdata-sections -Os" CACHE STRING "C flags" FORCE)
set(CMAKE_CXX_FLAGS "${MCU_FLAGS} -DSTM32F103xB -ffunction-sections -fdata-sections -Os -fno-exceptions -fno-rtti" CACHE STRING "C++ flags" FORCE)
set(CMAKE_ASM_FLAGS "${MCU_FLAGS}" CACHE STRING "ASM flags" FORCE)

# Linker flags: gc-sections removes unused code, -lm for math (sinf/cosf in OLED)
set(CMAKE_EXE_LINKER_FLAGS
    "${MCU_FLAGS} -T ${CMAKE_SOURCE_DIR}/STM32F103XX_FLASH.ld --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -lm"
    CACHE STRING "Linker flags" FORCE)

# MinGW generator ignores CMAKE_EXECUTABLE_SUFFIX with CMAKE_SYSTEM_NAME=Generic.
# The output file is just 'f103-node' (no extension). We'll keep the setting anyway.
set(CMAKE_EXECUTABLE_SUFFIX "" CACHE STRING "Executable suffix" FORCE)

# Don't try to link test programs (cross-compilation)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Skip compiler ABI info checks (not meaningful for cross-compilation)
set(CMAKE_C_COMPILER_WORKS 1 CACHE INTERNAL "")
set(CMAKE_CXX_COMPILER_WORKS 1 CACHE INTERNAL "")