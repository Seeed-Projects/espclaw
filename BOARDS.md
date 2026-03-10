# ESPClaw 板级配置指南

## 支持的开发板

### Seeed XIAO 系列（官方规格）

| 板子 | 芯片 | Flash | PSRAM | SRAM | 配置文件 |
|------|------|-------|-------|------|----------|
| XIAO ESP32C3 | C3 | 4MB | ❌ 无 | 400KB | `sdkconfig.defaults.xiao_esp32c3` |
| XIAO ESP32C5 | C5 | 8MB | ✅ 8MB Quad | 384KB | `sdkconfig.defaults.xiao_esp32c5` |
| XIAO ESP32C6 | C6 | 4MB | ❌ 无 | 512KB | `sdkconfig.defaults.xiao_esp32c6` |
| XIAO ESP32S3 | S3 | 8MB | ✅ 8MB Octal | 512KB | `sdkconfig.defaults.xiao_esp32s3` |

### 通用 ESP32 开发板

| 板子 | 芯片 | Flash | PSRAM | 配置文件 |
|------|------|-------|-------|----------|
| ESP32-C3 DevKit | C3 | 4MB | 无 | `sdkconfig.defaults.esp32c3` |
| ESP32-C5 DevKit | C5 | 4MB | 无 | `sdkconfig.defaults.esp32c5` |
| ESP32-S3 DevKit | S3 | 4MB | 无 | `sdkconfig.defaults.esp32s3` |

## 快速开始

### 使用构建脚本（推荐）

```bash
# XIAO ESP32S3 (8MB Flash + 8MB Octal PSRAM) - 推荐
./build.sh xiao_s3 flash

# XIAO ESP32C5 (8MB Flash + 8MB Quad PSRAM)
./build.sh xiao_c5 flash

# XIAO ESP32C6 (4MB Flash, 无 PSRAM)
./build.sh xiao_c6 flash

# XIAO ESP32C3 (4MB Flash, 无 PSRAM)
./build.sh xiao_c3 flash
```

### 手动构建

```bash
# 清理旧配置
rm -rf sdkconfig build

# 使用 XIAO S3 配置
cp sdkconfig.defaults.xiao_esp32s3 sdkconfig.defaults

# 编译烧录
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 build flash monitor
```

## 配置差异

### 内存配置对比

| 配置项 | 无 PSRAM (C3/C6) | Quad PSRAM (C5) | Octal PSRAM (S3) |
|--------|------------------|-----------------|------------------|
| LLM 缓冲 | 8 KB | 32 KB | 32 KB |
| 会话历史 | 8 KB | 32 KB | 32 KB |
| 对话轮数 | 8 | 24 | 24 |
| 工具调用 | 5 | 10 | 10 |
| TLS In/Out | 16/4 KB | 32/16 KB | 32/16 KB |
| OTA | ❌ | ✅ | ✅ |
| LittleFS | ❌ | ✅ | ✅ |

### PSRAM 类型说明

| 芯片 | PSRAM 模式 | 速度 | 说明 |
|------|-----------|------|------|
| C5 | Quad SPI (STR) | 40MHz | 仅支持 Quad 模式 |
| S3 | Octal SPI (DDR) | 80MHz | F4R8 配置：Quad Flash + Octal PSRAM |

### XIAO ESP32S3 特殊配置

XIAO ESP32S3 使用 **F4R8** 配置：
- **F4**: Quad SPI Flash @ 80MHz SDR
- **R8**: Octal PSRAM @ 80MHz DDR

根据 ESP-IDF 官方文档，这是 Group B 的安全组合。

## 自定义配置

如果你有其他开发板，可以基于现有配置修改：

```bash
# 复制最接近的配置
cp sdkconfig.defaults.xiao_esp32s3 sdkconfig.defaults.myboard

# 编辑配置
idf.py menuconfig

# 编译
idf.py set-target esp32s3
idf.py build
```

### 常见配置项

```ini
# Flash 大小
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y   # 或 8MB, 16MB

# PSRAM (如果有)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_QUAD=y          # C5 用 Quad
CONFIG_SPIRAM_MODE_OCT=y           # S3 用 Octal

# 分区表
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_4mb.csv"    # 无 OTA
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_8mb_ota.csv" # 有 OTA
```

## 参考资料

- [ESP32-S3 Flash/PSRAM 配置指南](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/flash_psram_config.html)
- [ESP32-C5 外部 RAM 指南](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/external-ram.html)
- [XIAO ESP32C5 官方文档](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/)
- [XIAO ESP32C6 官方文档](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/)
