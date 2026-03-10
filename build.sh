#!/bin/bash
# ESPClaw 构建脚本 - 支持多种开发板
# 用法: ./build.sh <board> [action]
#   board: xiao_c3 | xiao_c5 | xiao_s3 | c3 | c5 | s3
#   action: build | flash | monitor | all (默认: all)

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# 板子配置映射
declare -A BOARD_CONFIG=(
    ["xiao_c3"]="esp32c3:sdkconfig.defaults.xiao_esp32c3:partitions_4mb.csv"
    ["xiao_c5"]="esp32c5:sdkconfig.defaults.xiao_esp32c5:partitions_8mb_ota.csv"
    ["xiao_c6"]="esp32c6:sdkconfig.defaults.xiao_esp32c6:partitions_4mb.csv"
    ["xiao_s3"]="esp32s3:sdkconfig.defaults.xiao_esp32s3:partitions_8mb_ota.csv"
    ["c3"]="esp32c3:sdkconfig.defaults.esp32c3:partitions_4mb.csv"
    ["c5"]="esp32c5:sdkconfig.defaults.esp32c5:partitions_4mb.csv"
    ["s3"]="esp32s3:sdkconfig.defaults.esp32s3:partitions_4mb.csv"
)

# 帮助信息
show_help() {
    echo "ESPClaw 构建脚本"
    echo ""
    echo "用法: $0 <board> [action]"
    echo ""
    echo "支持的板子:"
    echo "  xiao_c3  - Seeed XIAO ESP32C3 (4MB Flash, 无 PSRAM)"
    echo "  xiao_c5  - Seeed XIAO ESP32C5 (8MB Flash, 8MB Quad PSRAM)"
    echo "  xiao_c6  - Seeed XIAO ESP32C6 (4MB Flash, 无 PSRAM)"
    echo "  xiao_s3  - Seeed XIAO ESP32S3 (8MB Flash, 8MB Octal PSRAM)"
    echo "  c3       - 通用 ESP32C3 开发板"
    echo "  c5       - 通用 ESP32C5 开发板"
    echo "  s3       - 通用 ESP32S3 开发板"
    echo ""
    echo "操作:"
    echo "  build    - 仅编译"
    echo "  flash    - 编译并烧录"
    echo "  monitor  - 打开串口监视器"
    echo "  all      - 编译、烧录、监视 (默认)"
    echo ""
    echo "示例:"
    echo "  $0 xiao_s3 flash     # 为 XIAO S3 编译并烧录"
    echo "  $0 xiao_c5 build     # 仅编译 XIAO C5"
    exit 0
}

# 检查参数
if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    show_help
fi

BOARD=$1
ACTION=${2:-all}

# 验证板子
if [ -z "${BOARD_CONFIG[$BOARD]}" ]; then
    log_error "未知板子: $BOARD"
    echo "支持的板子: ${!BOARD_CONFIG[*]}"
    exit 1
fi

# 解析配置
IFS=':' read -r TARGET CONFIG_FILE PARTITION_FILE <<< "${BOARD_CONFIG[$BOARD]}"
log_info "板子: $BOARD"
log_info "目标芯片: $TARGET"
log_info "配置文件: $CONFIG_FILE"
log_info "分区表: $PARTITION_FILE"

# 清理旧配置
log_info "清理旧配置..."
rm -rf sdkconfig build

# 复制配置文件
log_info "应用配置..."
cp "$CONFIG_FILE" sdkconfig.defaults

# 设置目标
log_info "设置目标: $TARGET"
idf.py set-target "$TARGET"

# 编译
log_info "开始编译..."
idf.py build

if [ "$ACTION" = "build" ]; then
    log_info "编译完成!"
    exit 0
fi

# 检测串口
detect_port() {
    for p in /dev/ttyACM* /dev/ttyUSB* /dev/cu.usbmodem*; do
        if [ -e "$p" ]; then
            echo "$p"
            return
        fi
    done
    echo ""
}

PORT=$(detect_port)
if [ -z "$PORT" ]; then
    log_warn "未检测到串口设备，请手动指定:"
    echo "  idf.py -p /dev/ttyXXX flash monitor"
    exit 0
fi

log_info "检测到串口: $PORT"

# 烧录
log_info "烧录固件..."
idf.py -p "$PORT" flash

if [ "$ACTION" = "flash" ]; then
    log_info "烧录完成!"
    exit 0
fi

# 监视器
log_info "启动串口监视器 (Ctrl+] 退出)..."
idf.py -p "$PORT" monitor
