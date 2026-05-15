# ESP32Toy 🎮

基于 ESP-IDF 框架的 ESP32 开发项目。

## 概述

ESP32Toy 是一个 ESP32 微控制器开发项目，使用乐鑫官方的 ESP-IDF 框架。本项目作为 ESP32 开发的起点模板，包含标准项目结构和基本配置。

## 硬件要求

- ESP32 开发板（如 ESP32-DevKitC、ESP32-WROVER 等）
- USB 数据线（用于供电和编程）

## 软件要求

- **ESP-IDF** v5.x 或更高版本 ([安装指南](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/))
- **工具链**：ESP-IDF 自带

## 快速开始

### 1. 设置 ESP-IDF 环境

```bash
# Windows (ESP-IDF PowerShell)
%USERPROFILE%\esp\esp-idf\export.ps1

# Linux / macOS
. $HOME/esp/esp-idf/export.sh
```

### 2. 配置项目

```bash
idf.py menuconfig
```

### 3. 编译

```bash
idf.py build
```

### 4. 烧录到设备

```bash
idf.py -p PORT flash monitor
```

> 将 `PORT` 替换为实际端口号（Windows 下如 `COM3`，Linux 下如 `/dev/ttyUSB0`）

## 项目结构

```
ESP32Toy/
├── main/                    # 主应用程序
│   ├── CMakeLists.txt       # 组件构建配置
│   ├── main.c               # 主程序入口
│   └── Kconfig.projbuild    # 项目配置菜单
├── CMakeLists.txt           # 顶层构建配置
├── sdkconfig.defaults       # 默认配置
├── .gitignore
├── README.md
└── ESP32Toy.code-workspace  # VS Code 工作区
```

## 许可证

本项目基于 MIT 许可证开源。详见 `LICENSE` 文件。
