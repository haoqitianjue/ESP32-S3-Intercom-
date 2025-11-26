# ESP32-S3 无线多人对讲系统 / ESP32-S3 Wireless Multi-Person Intercom System

[English](#english) | [中文](#chinese)

---

<a name="chinese"></a>
## 🎙️ 中文说明

基于 ESP32-S3 和 iLBC 编解码的低延迟无线对讲系统。

### ✨ 主要特性

- ✅ **ESP32-S3 + ESP-NOW 协议** - 远距离通信（150-200米城市环境）
- ✅ **iLBC-20ms 音频编解码** - 15.2kbps 低比特率，高音质
- ✅ **Long Range (LR) 模式** - Espressif 专利技术，接收灵敏度 -102dBm
- ✅ **超低延迟** - 端到端延迟 <50ms
- ✅ **多人通信** - 支持最多 10 设备同时通信
- ✅ **VAD 语音检测** - 智能语音激活，降低功耗
- ✅ **噪声抑制** - 持续噪声抑制算法
- ✅ **自听音功能** - 实时监听自己的语音

### 📋 硬件要求

**必需硬件：**
- ESP32-S3 模块（推荐 WROOM-1U，支持外置天线）
- I2S 音频接口（PDM 麦克风 + I2S 扬声器）
- 2.4GHz 外置天线（推荐 5dBi 全向天线）

**推荐配置：**
- MCU: ESP32-S3-WROOM-1U (16MB Flash)
- 麦克风: PDM 数字麦克风
- 扬声器: 8Ω 0.5-1W
- 天线: 2.4GHz 5dBi 全向天线

**⚠️ 注意：本项目仅提供软件代码，不包含硬件设计文件（原理图、PCB）。用户需根据引脚定义自行设计硬件或购买兼容开发板。**

### 🚀 快速开始

#### 1. 安装 PlatformIO

请访问 [PlatformIO 官网](https://platformio.org/) 安装 PlatformIO IDE 或 CLI。

#### 2. 克隆项目

```bash
git clone https://github.com/haoqitianjue/ESP32-S3-Intercom-.git
cd ESP32-S3-Intercom-
```

#### 3. 配置硬件引脚

编辑 `src/main.cpp`，根据你的硬件修改引脚定义：

```cpp
// I2S 引脚配置
#define I2S_MIC_CLK      16  // PDM 麦克风时钟
#define I2S_MIC_DATA     18  // PDM 麦克风数据
#define I2S_SPK_BCLK     5   // 扬声器位时钟
#define I2S_SPK_LRCK     6   // 扬声器字选择
#define I2S_SPK_DATA     7   // 扬声器数据
```

#### 4. 编译上传

```bash
platformio run --target upload
```

#### 5. 配置参数

根据使用场景修改 `src/main.cpp` 中的配置：

```cpp
#define WIFI_CHANNEL 1       // WiFi 信道（1-13）
#define VOLUME_INITIAL 50    // 初始音量（0-100）
```

### 📖 文档

- [使用说明](docs/USER_GUIDE_CN.md) - 详细使用教程
- [硬件接口](docs/HARDWARE_CN.md) - 引脚定义和硬件要求
- [配置指南](docs/CONFIG_CN.md) - 参数配置说明
- [常见问题](docs/FAQ_CN.md) - 常见问题解答

### 🔧 技术参数

| 参数 | 值 |
|------|-----|
| 音频编解码 | iLBC-20ms |
| 比特率 | 15.2 kbps |
| 采样率 | 8000 Hz |
| 帧长度 | 20ms (160 samples) |
| 通信协议 | ESP-NOW (802.11b + LR) |
| 通信距离 | 150-200米（城市环境）<br>300-500米（空旷环境） |
| 端到端延迟 | <50ms |
| 最大设备数 | 10 |

### 📄 开源许可

本项目采用 [MIT License](LICENSE) 开源协议。

### ⚠️ 免责声明

- 本项目仅提供软件代码，不包含硬件设计文件（原理图、PCB）
- 用户需自行设计硬件或购买兼容设备
- 请遵守当地无线电管理法规

### 🤝 贡献

欢迎提交 Issue 和 Pull Request！

### 📧 联系方式

- GitHub Issues: [提交问题](https://github.com/haoqitianjue/ESP32-S3-Intercom-/issues)

---

<a name="english"></a>
## 🎙️ English

Low-latency wireless intercom system based on ESP32-S3 and iLBC codec.

### ✨ Key Features

- ✅ **ESP32-S3 + ESP-NOW Protocol** - Long-range communication (150-200m urban, 300-500m open field)
- ✅ **iLBC-20ms Audio Codec** - 15.2kbps low bitrate, high quality
- ✅ **Long Range (LR) Mode** - Espressif patented technology, -102dBm sensitivity
- ✅ **Ultra-Low Latency** - End-to-end latency <50ms
- ✅ **Multi-Person Communication** - Support up to 10 devices simultaneously
- ✅ **VAD (Voice Activity Detection)** - Intelligent voice activation
- ✅ **Noise Suppression** - Continuous noise reduction algorithm
- ✅ **Sidetone Function** - Real-time self-monitoring

### 📋 Hardware Requirements

**Required Hardware:**
- ESP32-S3 module (Recommend WROOM-1U with external antenna support)
- I2S audio interface (PDM microphone + I2S speaker)
- 2.4GHz external antenna (Recommend 5dBi omnidirectional)

**Recommended Configuration:**
- MCU: ESP32-S3-WROOM-1U (16MB Flash)
- Microphone: PDM digital microphone
- Speaker: 8Ω 0.5-1W
- Antenna: 2.4GHz 5dBi omnidirectional

**⚠️ Note: This project only provides software code, does not include hardware design files (schematics, PCB). Users need to design hardware according to pin definitions or purchase compatible development boards.**

### 🚀 Quick Start

#### 1. Install PlatformIO

Visit [PlatformIO Official Website](https://platformio.org/) to install PlatformIO IDE or CLI.

#### 2. Clone Project

```bash
git clone https://github.com/haoqitianjue/ESP32-S3-Intercom-.git
cd ESP32-S3-Intercom-
```

#### 3. Configure Hardware Pins

Edit `src/main.cpp` and modify pin definitions according to your hardware:

```cpp
// I2S Pin Configuration
#define I2S_MIC_CLK      16  // PDM microphone clock
#define I2S_MIC_DATA     18  // PDM microphone data
#define I2S_SPK_BCLK     5   // Speaker bit clock
#define I2S_SPK_LRCK     6   // Speaker word select
#define I2S_SPK_DATA     7   // Speaker data
```

#### 4. Compile and Upload

```bash
platformio run --target upload
```

#### 5. Configure Parameters

Modify configuration in `src/main.cpp` according to your use case:

```cpp
#define WIFI_CHANNEL 1       // WiFi channel (1-13)
#define VOLUME_INITIAL 50    // Initial volume (0-100)
```

### 📖 Documentation

- [User Guide](docs/USER_GUIDE_EN.md) - Detailed usage tutorial
- [Hardware Interface](docs/HARDWARE_EN.md) - Pin definitions and hardware requirements
- [Configuration Guide](docs/CONFIG_EN.md) - Parameter configuration instructions
- [FAQ](docs/FAQ_EN.md) - Frequently asked questions

### 🔧 Technical Specifications

| Parameter | Value |
|-----------|-------|
| Audio Codec | iLBC-20ms |
| Bitrate | 15.2 kbps |
| Sample Rate | 8000 Hz |
| Frame Length | 20ms (160 samples) |
| Protocol | ESP-NOW (802.11b + LR) |
| Range | 150-200m (urban)<br>300-500m (open field) |
| End-to-End Latency | <50ms |
| Max Devices | 10 |

### 📄 License

This project is licensed under the [MIT License](LICENSE).

### ⚠️ Disclaimer

- This project only provides software code, does not include hardware design files (schematics, PCB)
- Users need to design hardware themselves or purchase compatible devices
- Please comply with local radio regulations

### 🤝 Contributing

Issues and Pull Requests are welcome!

### 📧 Contact

- GitHub Issues: [Submit Issue](https://github.com/haoqitianjue/ESP32-S3-Intercom-/issues)

---

## 🌟 Star History

If this project helps you, please give it a ⭐ Star!

## 📸 Screenshots

(Add your project screenshots here)

---

**Made with ❤️ for the open source community**

