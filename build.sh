#!/bin/bash
# ESPClaw 构建脚本 - 支持多种开发板
# 用法: ./build.sh <board> [action]
# 每个板子使用独立的 sdkconfig 文件，切换时不互相覆盖

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# 获取板子配置
get_board_config() {
    local board=$1
    case "$board" in
        xiao_c3)       echo "esp32c3:sdkconfig.defaults.xiao_esp32c3" ;;
        xiao_c5)       echo "esp32c5:sdkconfig.defaults.xiao_esp32c5" ;;
        xiao_c6)       echo "esp32c6:sdkconfig.defaults.xiao_esp32c6" ;;
        xiao_s3)       echo "esp32s3:sdkconfig.defaults.xiao_esp32s3" ;;
        xiao_s3_sense) echo "esp32s3:sdkconfig.defaults.xiao_esp32s3_sense" ;;
        xiao_s3_plus)  echo "esp32s3:sdkconfig.defaults.xiao_esp32s3_plus" ;;
        c3)            echo "esp32c3:sdkconfig.defaults.esp32c3" ;;
        c5)            echo "esp32c5:sdkconfig.defaults.esp32c5" ;;
        s3)            echo "esp32s3:sdkconfig.defaults.esp32s3" ;;
        *)             echo "" ;;
    esac
}

# 帮助信息
show_help() {
    echo "ESPClaw 构建脚本"
    echo ""
    echo "用法: $0 <board> [action]"
    echo ""
    echo "支持的板子:"
    echo "  xiao_c3       - Seeed XIAO ESP32C3 (4MB Flash, 无 PSRAM)"
    echo "  xiao_c5       - Seeed XIAO ESP32C5 (8MB Flash, 8MB Quad PSRAM)"
    echo "  xiao_c6       - Seeed XIAO ESP32C6 (4MB Flash, 无 PSRAM)"
    echo "  xiao_s3       - Seeed XIAO ESP32S3 (8MB Flash, 8MB Octal PSRAM)"
    echo "  xiao_s3_sense - Seeed XIAO ESP32S3 Sense (8MB Flash, 8MB PSRAM, SD卡)"
    echo "  xiao_s3_plus  - Seeed XIAO ESP32S3 Plus (16MB Flash, 8MB PSRAM)"
    echo "  c3            - 通用 ESP32C3 开发板"
    echo "  c5            - 通用 ESP32C5 开发板"
    echo "  s3            - 通用 ESP32S3 开发板"
    echo ""
    echo "操作:"
    echo "  build    - 仅编译"
    echo "  flash    - 编译并烧录"
    echo "  monitor  - 打开串口监视器"
    echo "  menuconfig - 配置（会保存到对应板子的 sdkconfig）"
    echo "  all      - 编译、烧录、监视 (默认)"
    echo ""
    echo "示例:"
    echo "  $0 xiao_s3 flash      # 为 XIAO S3 编译并烧录"
    echo "  $0 xiao_s3 menuconfig # 配置 XIAO S3"
    echo "  $0 xiao_c5 build      # 仅编译 XIAO C5"
    echo ""
    echo "配置持久化:"
    echo "  编辑 sdkconfig.user 文件保存 WiFi/API 等个人配置"
    echo "  切换板子时会自动合并，无需重复配置"
    exit 0
}

# 检查参数
if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    show_help
fi

BOARD=$1
ACTION=${2:-all}

# 获取配置
CONFIG_STR=$(get_board_config "$BOARD")
if [ -z "$CONFIG_STR" ]; then
    log_error "未知板子: $BOARD"
    echo "支持的板子: xiao_c3 xiao_c5 xiao_c6 xiao_s3 xiao_s3_sense xiao_s3_plus c3 c5 s3"
    exit 1
fi

TARGET=$(echo "$CONFIG_STR" | cut -d: -f1)
BOARD_CONFIG=$(echo "$CONFIG_STR" | cut -d: -f2)
SDKCONFIG_FILE="sdkconfig.${BOARD}"

log_info "板子: $BOARD"
log_info "目标芯片: $TARGET"
log_info "板级配置: $BOARD_CONFIG"
log_info "SDK配置: $SDKCONFIG_FILE"

# 构建 SDKCONFIG_DEFAULTS（包含用户配置）
SDKDEFAULTS="${BOARD_CONFIG}"
if [ -f "sdkconfig.user" ]; then
    log_info "合并用户配置: sdkconfig.user"
    SDKDEFAULTS="${BOARD_CONFIG};sdkconfig.user"
fi

# IDF 通用参数（ESP-IDF 5.5 正确语法）
IDF_ARGS="-D SDKCONFIG=${SDKCONFIG_FILE} -D SDKCONFIG_DEFAULTS=\"${SDKDEFAULTS}\""

# 检测串口
detect_port() {
    for p in /dev/ttyACM* /dev/ttyUSB* /dev/cu.usbmodem* /dev/cu.usbserial*; do
        if [ -e "$p" ]; then
            echo "$p"
            return
        fi
    done
    echo ""
}

case "$ACTION" in
    menuconfig)
        log_info "打开配置菜单..."
        eval idf.py ${IDF_ARGS} menuconfig
        ;;
    build)
        log_info "开始编译..."
        eval idf.py ${IDF_ARGS} set-target "$TARGET"
        eval idf.py ${IDF_ARGS} build
        log_info "编译完成!"
        ;;
    flash)
        PORT=$(detect_port)
        if [ -z "$PORT" ]; then
            log_warn "未检测到串口设备"
            log_info "请手动指定: idf.py -p /dev/ttyXXX flash"
            exit 0
        fi
        log_info "检测到串口: $PORT"
        log_info "编译并烧录..."
        eval idf.py ${IDF_ARGS} set-target "$TARGET"
        eval idf.py ${IDF_ARGS} build
        eval idf.py ${IDF_ARGS} -p "$PORT" flash
        log_info "烧录完成!"
        ;;
    monitor)
        PORT=$(detect_port)
        if [ -z "$PORT" ]; then
            log_error "未检测到串口设备"
        fi
        log_info "启动串口监视器 (Ctrl+] 退出)..."
        eval idf.py ${IDF_ARGS} -p "$PORT" monitor
        ;;
    all)
        PORT=$(detect_port)
        if [ -z "$PORT" ]; then
            log_warn "未检测到串口设备，仅编译"
            log_info "开始编译..."
            eval idf.py ${IDF_ARGS} set-target "$TARGET"
            eval idf.py ${IDF_ARGS} build
            log_info "编译完成!"
            exit 0
        fi
        log_info "检测到串口: $PORT"
        log_info "编译、烧录、监视..."
        eval idf.py ${IDF_ARGS} set-target "$TARGET"
        eval idf.py ${IDF_ARGS} build
        eval idf.py ${IDF_ARGS} -p "$PORT" flash monitor
        ;;
    *)
        log_error "未知操作: $ACTION"
        echo "支持的操作: build, flash, monitor, menuconfig, all"
        exit 1
        ;;
esac
