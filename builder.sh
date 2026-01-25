#!/bin/bash

# WeActSTM32H5 build and flash script (CMake presets)
PROJECT_NAME="WeActSTM32H5"
BUILD_DIR="build"
BUILD_TYPE="Debug"
FLASH_ADDRESS="0x08000000"

UART_TOOL="picocom"
UART_BAUD="115200"
UART_DEVICE=""

ARM_OBJCOPY="arm-none-eabi-objcopy"
ARM_SIZE="arm-none-eabi-size"
DFU_UTIL="dfu-util"
STM32CLI="STM32_Programmer_CLI"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m'

show_usage() {
    echo -e "${BLUE}Usage:${NC}"
    echo -e "  $0 clean"
    echo -e "  $0 build [--type Debug|Release]"
    echo -e "  $0 flash [--type Debug|Release] [--address ADDR] [--no-build]"
    echo -e "  $0 all [--type Debug|Release] [--address ADDR]"
    echo -e "  $0 enumerate"
    echo -e "  $0 monitor [--tool TOOL] [--baud BAUD] [--device DEVICE]"
}

print_artifact_info() {
    local image="$1"
    echo -e "${BLUE}[INFO]${NC} Flash image: $(cd "$(dirname "$image")" && pwd)/$(basename "$image")"
    ls -l "$image" || true
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$image" || true
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$image" || true
    else
        echo -e "${YELLOW}[WARN]${NC} No sha256 tool found (shasum/sha256sum)"
    fi
}

warn_if_artifact_stale() {
    local artifact="$1"

    # If any relevant project file is newer than the artifact, warn loudly.
    # This guards against accidentally flashing an old build (e.g. running `flash` without rebuilding).
    local newer
    newer=$(find \
        Core AZURE_RTOS USBX cmake \
        CMakeLists.txt CMakePresets.json WeActSTM32H5.ioc \
        -type f \( -name '*.c' -o -name '*.h' -o -name '*.s' -o -name '*.S' -o -name '*.ld' -o -name '*.cmake' -o -name 'CMakeLists.txt' -o -name '*.ioc' \) \
        -newer "$artifact" 2>/dev/null | head -n 1)

    if [ -n "$newer" ]; then
        echo -e "${YELLOW}[WARN]${NC} Build artifact looks stale vs: $newer"
        echo -e "${YELLOW}[WARN]${NC} Re-run: $0 build --type ${BUILD_TYPE} (or omit --no-build)"
        return 1
    fi

    return 0
}

clean_project() {
    echo -e "${BLUE}[INFO]${NC} Cleaning build directory..."
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR" || return 1
    fi
    echo -e "${GREEN}[SUCCESS]${NC} Clean complete"
}

configure_project() {
    local preset="$1"
    echo -e "${BLUE}[INFO]${NC} Configuring (${preset})..."
    cmake --preset "$preset" || return 1
}

build_project() {
    local preset="$1"
    echo -e "${BLUE}[INFO]${NC} Building (${preset})..."
    cmake --build --preset "$preset" || return 1

    local elf_path="$BUILD_DIR/$preset/${PROJECT_NAME}.elf"
    local bin_path="$BUILD_DIR/$preset/${PROJECT_NAME}.bin"

    if [ ! -f "$elf_path" ]; then
        echo -e "${RED}[ERROR]${NC} ELF not found: $elf_path"
        return 1
    fi

    if command -v "$ARM_OBJCOPY" >/dev/null 2>&1; then
        "$ARM_OBJCOPY" -O binary "$elf_path" "$bin_path" || return 1
        "$ARM_OBJCOPY" -O ihex "$elf_path" "$BUILD_DIR/$preset/${PROJECT_NAME}.hex" || return 1
        echo -e "${GREEN}[SUCCESS]${NC} Binary created: $bin_path"
    else
        echo -e "${YELLOW}[WARN]${NC} $ARM_OBJCOPY not found; skipping .bin generation"
    fi

    if command -v "$ARM_SIZE" >/dev/null 2>&1; then
        "$ARM_SIZE" "$elf_path" || true
    fi
}

ensure_stm32_cli() {
    if command -v "$STM32CLI" >/dev/null 2>&1; then
        return 0
    fi

    local candidates=()
    case "$(uname -s)" in
        Darwin)
            candidates+=(
                "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin"
                "/Applications/STM32CubeProgrammer.app/Contents/MacOS"
            )
            while IFS= read -r -d '' path; do
                candidates+=("$path")
            done < <(find /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins -path '*cubeprogrammer*' -type d -name bin -print0 2>/dev/null)
            ;;
        Linux)
            candidates+=(
                "/opt/st/stm32cubeprogrammer/bin"
                "/opt/STM32CubeProgrammer/bin"
                "/usr/local/bin"
            )
            ;;
        MINGW*|MSYS*|CYGWIN*)
            candidates+=(
                "/c/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin"
                "/c/Program Files (x86)/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin"
            )
            ;;
    esac

    for dir in "${candidates[@]}"; do
        if [ -x "$dir/$STM32CLI" ]; then
            export PATH="$dir:$PATH"
            return 0
        fi
    done

    return 1
}

detect_dfu_device() {
    if ! ensure_stm32_cli; then
        return 1
    fi

    local dfu_list
    dfu_list=$("$STM32CLI" -l usb -v 2>/dev/null)
    echo "$dfu_list" | grep -qi "DFU Interface" || return 1

    DFU_PORT=$(echo "$dfu_list" | awk -F': ' '/Device Index/ {print $2; exit}')
    if [ -z "$DFU_PORT" ]; then
        return 1
    fi
    return 0
}

post_flash_check() {
    echo -e "${BLUE}[INFO]${NC} Checking for USB CDC/MSC enumeration..."
    local found=0

    case "$(uname -s)" in
        Darwin)
            if command -v system_profiler >/dev/null 2>&1; then
                system_profiler SPUSBDataType | grep -i -E "STM32|Gramini|JPEG Encoder|Mass Storage|CDC|ACM|USB Serial|0483|5710|0x0483|0x5710" >/dev/null 2>&1 && found=1
            fi
            ls /dev/cu.* /dev/tty.* 2>/dev/null | grep -i "usb\|modem" >/dev/null 2>&1 && found=1
            ;;
        Linux)
            if command -v lsusb >/dev/null 2>&1; then
                lsusb | grep -i -E "STM32|Gramini|Mass Storage|CDC|ACM" >/dev/null 2>&1 && found=1
            fi
            ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | grep -q "/dev/tty" && found=1
            ;;
        *)
            echo -e "${YELLOW}[WARN]${NC} OS not recognized for auto-check"
            return 0
            ;;
    esac

    if [ "$found" -eq 1 ]; then
        echo -e "${GREEN}[SUCCESS]${NC} USB device appears to be enumerated"
    else
        echo -e "${YELLOW}[WARN]${NC} No CDC/MSC device detected. If the board stays in DFU or USB is not initialized, enumeration won't appear."
    fi
}

enumerate_devices() {
    echo -e "${BLUE}[INFO]${NC} Enumerating USB and UART devices..."

    case "$(uname -s)" in
        Darwin)
            echo -e "${BLUE}[INFO]${NC} USB devices (filtered):"
            if command -v system_profiler >/dev/null 2>&1; then
                system_profiler SPUSBDataType | grep -i -E "STM32|Gramini|JPEG Encoder|Mass Storage|CDC|ACM|USB Serial|0483|5710|0x0483|0x5710" || true
            else
                echo -e "${YELLOW}[WARN]${NC} system_profiler not available"
            fi

            echo -e "${BLUE}[INFO]${NC} UART devices (/dev/cu.* and /dev/tty.*):"
            ls /dev/cu.* /dev/tty.* 2>/dev/null || echo -e "${YELLOW}[WARN]${NC} No UART devices found"
            ;;
        Linux)
            echo -e "${BLUE}[INFO]${NC} USB devices (filtered):"
            if command -v lsusb >/dev/null 2>&1; then
                lsusb | grep -i -E "STM32|Gramini|Mass Storage|CDC|ACM" || true
            else
                echo -e "${YELLOW}[WARN]${NC} lsusb not available"
            fi

            echo -e "${BLUE}[INFO]${NC} UART devices (/dev/ttyACM* /dev/ttyUSB*):"
            ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo -e "${YELLOW}[WARN]${NC} No UART devices found"
            ;;
        *)
            echo -e "${YELLOW}[WARN]${NC} OS not recognized for enumeration"
            return 1
            ;;
    esac
}

flash_project() {
    local preset="$1"
    local elf_path="$BUILD_DIR/$preset/${PROJECT_NAME}.elf"
    local bin_path="$BUILD_DIR/$preset/${PROJECT_NAME}.bin"
    local image="$elf_path"

    # For STM32_Programmer_CLI with an explicit flash address, prefer a raw binary.
    if [ -f "$bin_path" ]; then
        image="$bin_path"
    fi

    if [ ! -f "$image" ]; then
        echo -e "${RED}[ERROR]${NC} Image not found: $image"
        return 1
    fi

    print_artifact_info "$image"

    if detect_dfu_device; then
        echo -e "${BLUE}[INFO]${NC} DFU device detected (${DFU_PORT}). Flashing with STM32_Programmer_CLI..."
        # Refuse to flash if inputs are newer than the artifact (unless the user intentionally bypasses build).
        warn_if_artifact_stale "$image" || return 1
        "$STM32CLI" -c port="$DFU_PORT" -d "$image" "$FLASH_ADDRESS" -v || return 1
        echo -e "${GREEN}[SUCCESS]${NC} DFU flash complete"
        echo -e "${YELLOW}[WARN]${NC} DFU reset is not supported; please reset the board manually"
        post_flash_check
        return 0
    fi

    if ! ensure_stm32_cli; then
        echo -e "${RED}[ERROR]${NC} No DFU device detected and STM32_Programmer_CLI not found."
        echo -e "${YELLOW}[HELP]${NC} Install STM32CubeProgrammer from: https://www.st.com/en/development-tools/stm32cubeprog.html"
        return 1
    fi

    echo -e "${BLUE}[INFO]${NC} Flashing with STM32_Programmer_CLI to ${FLASH_ADDRESS}..."
    warn_if_artifact_stale "$image" || return 1
    "$STM32CLI" -c port=usb1 -d "$image" "$FLASH_ADDRESS" || return 1
    echo -e "${GREEN}[SUCCESS]${NC} Flash complete"
    post_flash_check
}

monitor_terminal() {
    local tool="$UART_TOOL"
    local baud="$UART_BAUD"
    local device="$UART_DEVICE"
    local retry=1
    local retry_delay=0.5

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --tool)
                tool="$2"; shift 2;;
            --baud)
                baud="$2"; shift 2;;
            --device)
                device="$2"; shift 2;;
            --retry)
                retry=1; shift 1;;
            --no-retry)
                retry=0; shift 1;;
            --retry-delay)
                retry_delay="$2"; shift 2;;
            *)
                echo -e "${RED}[ERROR]${NC} Unknown option: $1"; return 1;;
        esac
    done

    if [ -z "$device" ]; then
        echo -e "${RED}[ERROR]${NC} No UART device specified"
        return 1
    fi

    if ! command -v "$tool" >/dev/null 2>&1; then
        echo -e "${RED}[ERROR]${NC} Terminal tool not found: $tool"
        return 1
    fi

    echo -e "${BLUE}[INFO]${NC} Connecting: $device @ $baud"

    while true; do
        if [ ! -e "$device" ]; then
            if [ "$retry" -eq 1 ]; then
                echo -e "${YELLOW}[WARN]${NC} Device not found: $device (retrying...)"
                sleep "$retry_delay"
                continue
            else
                echo -e "${RED}[ERROR]${NC} Device not found: $device"
                return 1
            fi
        fi

        case "$tool" in
            picocom) "$tool" -b "$baud" "$device" ;;
            minicom) "$tool" -b "$baud" -D "$device" ;;
            screen) "$tool" "$device" "$baud" ;;
            *) "$tool" -b "$baud" "$device" ;;
        esac

        exit_code=$?
        if [ "$exit_code" -eq 0 ]; then
            break
        fi

        if [ "$retry" -eq 1 ]; then
            echo -e "${YELLOW}[WARN]${NC} Disconnected (exit $exit_code). Reconnecting..."
            sleep "$retry_delay"
            continue
        fi

        break
    done
}

if [ $# -eq 0 ]; then
    show_usage
    exit 1
fi

COMMAND="$1"
shift

case "$COMMAND" in
    clean)
        clean_project; exit $?;;
    build)
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --type|-t)
                    BUILD_TYPE="$2"; shift 2;;
                *)
                    echo -e "${RED}[ERROR]${NC} Unknown option: $1"; exit 1;;
            esac
        done
        configure_project "$BUILD_TYPE" && build_project "$BUILD_TYPE"
        exit $?;;
    flash)
        DO_BUILD=1
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --type|-t)
                    BUILD_TYPE="$2"; shift 2;;
                --address|-a)
                    FLASH_ADDRESS="$2"; shift 2;;
                --no-build)
                    DO_BUILD=0; shift 1;;
                *)
                    echo -e "${RED}[ERROR]${NC} Unknown option: $1"; exit 1;;
            esac
        done
        if [ "$DO_BUILD" -eq 1 ]; then
            configure_project "$BUILD_TYPE" || exit $?
            build_project "$BUILD_TYPE" || exit $?
        fi
        flash_project "$BUILD_TYPE"
        exit $?;;
    all)
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --type|-t)
                    BUILD_TYPE="$2"; shift 2;;
                --address|-a)
                    FLASH_ADDRESS="$2"; shift 2;;
                *)
                    echo -e "${RED}[ERROR]${NC} Unknown option: $1"; exit 1;;
            esac
        done
        clean_project || exit $?
        configure_project "$BUILD_TYPE" || exit $?
        build_project "$BUILD_TYPE" || exit $?
        flash_project "$BUILD_TYPE" || exit $?
        exit $?;;
    enumerate)
        enumerate_devices; exit $?;;
    monitor)
        monitor_terminal "$@"; exit $?;;
    *)
        echo -e "${RED}[ERROR]${NC} Unknown command: $COMMAND"
        show_usage
        exit 1;;
esac
