cmake_minimum_required(VERSION 3.22)

# =============================================================================
# STM32H5 USBX Composite Device (CDC + MSC) Project
# =============================================================================

# Compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Project name
set(CMAKE_PROJECT_NAME WeActSTM32H5)

# Enable compile_commands.json for clangd/IDE indexing
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Project declaration
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable ASM and C
enable_language(C ASM)

# Create executable
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX/Middleware sources (handles all globbing)
add_subdirectory(cmake/stm32cubemx)

# Link the stm32cubemx interface library (provides includes and defines)
target_link_libraries(${CMAKE_PROJECT_NAME} stm32cubemx)

# Fix for GCC libob.a issue with C++ files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)
