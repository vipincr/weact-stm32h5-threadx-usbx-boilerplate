cmake_minimum_required(VERSION 3.22)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Project root for convenience
set(PROJ_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# =============================================================================
# Compile Definitions
# =============================================================================
set(MX_Defines_Syms
    TX_INCLUDE_USER_DEFINE_FILE
    TX_SINGLE_MODE_NON_SECURE=1
    UX_INCLUDE_USER_DEFINE_FILE
    USBX_STANDALONE_BRINGUP
    USE_HAL_DRIVER
    STM32H562xx
    $<$<CONFIG:Debug>:DEBUG>
)

# =============================================================================
# Include Directories
# =============================================================================
set(MX_Include_Dirs
    ${PROJ_ROOT}/Core/Inc
    ${PROJ_ROOT}/AZURE_RTOS/App
    ${PROJ_ROOT}/USBX/App
    ${PROJ_ROOT}/USBX/Target
    ${PROJ_ROOT}/Drivers/STM32H5xx_HAL_Driver/Inc
    ${PROJ_ROOT}/Drivers/STM32H5xx_HAL_Driver/Inc/Legacy
    ${PROJ_ROOT}/Drivers/CMSIS/Device/ST/STM32H5xx/Include
    ${PROJ_ROOT}/Drivers/CMSIS/Include
    ${PROJ_ROOT}/Middlewares/ST/threadx/common/inc
    ${PROJ_ROOT}/Middlewares/ST/threadx/ports/cortex_m33/gnu/inc
    ${PROJ_ROOT}/Middlewares/ST/usbx/common/core/inc
    ${PROJ_ROOT}/Middlewares/ST/usbx/ports/generic/inc
    ${PROJ_ROOT}/Middlewares/ST/usbx/common/usbx_stm32_device_controllers
    ${PROJ_ROOT}/Middlewares/ST/usbx/common/usbx_device_classes/inc
)

# =============================================================================
# Application Sources (Core, AZURE_RTOS, USBX App)
# =============================================================================
file(GLOB MX_Application_C_Src
    ${PROJ_ROOT}/Core/Src/*.c
    ${PROJ_ROOT}/AZURE_RTOS/App/*.c
    ${PROJ_ROOT}/USBX/App/*.c
)

file(GLOB MX_Application_ASM_Src
    ${PROJ_ROOT}/Core/Src/*.S
    ${PROJ_ROOT}/startup_stm32h562xx.s
)

set(MX_Application_Src
    ${MX_Application_C_Src}
    ${MX_Application_ASM_Src}
)

# =============================================================================
# STM32 HAL/LL Drivers
# =============================================================================
file(GLOB STM32_Drivers_Src
    ${PROJ_ROOT}/Drivers/STM32H5xx_HAL_Driver/Src/*.c
)

# =============================================================================
# USBX Middleware Sources
# =============================================================================
file(GLOB USBX_Core_Src
    ${PROJ_ROOT}/Middlewares/ST/usbx/common/core/src/*.c
)

file(GLOB USBX_DeviceClasses_Src
    ${PROJ_ROOT}/Middlewares/ST/usbx/common/usbx_device_classes/src/*.c
)

file(GLOB USBX_STM32_DCD_Src
    ${PROJ_ROOT}/Middlewares/ST/usbx/common/usbx_stm32_device_controllers/*.c
)

set(USBX_Src
    ${USBX_Core_Src}
    ${USBX_DeviceClasses_Src}
    ${USBX_STM32_DCD_Src}
)

# =============================================================================
# ThreadX Middleware Sources
# =============================================================================
file(GLOB ThreadX_Common_Src
    ${PROJ_ROOT}/Middlewares/ST/threadx/common/src/*.c
)

file(GLOB ThreadX_Port_Src
    ${PROJ_ROOT}/Middlewares/ST/threadx/ports/cortex_m33/gnu/src/*.S
)

set(ThreadX_Src
    ${ThreadX_Common_Src}
    ${ThreadX_Port_Src}
)

# =============================================================================
# Libraries
# =============================================================================

# Interface library for includes and symbols
add_library(stm32cubemx INTERFACE)
target_include_directories(stm32cubemx INTERFACE ${MX_Include_Dirs})
target_compile_definitions(stm32cubemx INTERFACE ${MX_Defines_Syms})

# STM32 HAL Drivers library
add_library(STM32_Drivers OBJECT)
target_sources(STM32_Drivers PRIVATE ${STM32_Drivers_Src})
target_link_libraries(STM32_Drivers PUBLIC stm32cubemx)

# USBX library
add_library(USBX OBJECT)
target_sources(USBX PRIVATE ${USBX_Src})
target_link_libraries(USBX PUBLIC stm32cubemx)

# ThreadX library
add_library(ThreadX OBJECT)
target_sources(ThreadX PRIVATE ${ThreadX_Src})
target_link_libraries(ThreadX PUBLIC stm32cubemx)

# =============================================================================
# Link Configuration
# =============================================================================
set(MX_LINK_LIBS
    STM32_Drivers
    USBX
    ThreadX
    ${TOOLCHAIN_LINK_LIBRARIES}
)

# Add application sources to the project
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${MX_Application_Src})

# Add libraries to the project
target_link_libraries(${CMAKE_PROJECT_NAME} ${MX_LINK_LIBS})

# Add the map file to the list of files to be removed with 'clean' target
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES ADDITIONAL_CLEAN_FILES ${CMAKE_PROJECT_NAME}.map)

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()
