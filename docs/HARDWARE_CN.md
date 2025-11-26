# 硬件接口说明

## 📌 引脚定义

### I2S 音频接口

#### PDM 麦克风（I2S0）
| 功能 | GPIO | 说明 |
|------|------|------|
| I2S_MIC_CLK | GPIO16 | PDM 时钟信号 |
| I2S_MIC_DATA | GPIO18 | PDM 数据输入 |

#### I2S 扬声器（I2S1）
| 功能 | GPIO | 说明 |
|------|------|------|
| I2S_SPK_BCLK | GPIO5 | 位时钟 |
| I2S_SPK_LRCK | GPIO6 | 字选择时钟 |
| I2S_SPK_DATA | GPIO7 | 数据输出 |

### 控制接口

#### EC11 旋转编码器（音量控制）
| 功能 | GPIO | 说明 |
|------|------|------|
| ENCODER_PIN_A | GPIO15 | 编码器 A 相 |
| ENCODER_PIN_B | GPIO4 | 编码器 B 相 |
| ENCODER_BUTTON | GPIO12 | 按键（自听音开关） |

#### ES16 16档位开关（频道选择）
| 功能 | GPIO | 说明 |
|------|------|------|
| ROTARY_PIN_1 | GPIO35 | 最低位 (1) |
| ROTARY_PIN_2 | GPIO36 | 次低位 (2) |
| ROTARY_PIN_4 | GPIO37 | 次高位 (4) |
| ROTARY_PIN_8 | GPIO38 | 最高位 (8) |

#### 其他控制
| 功能 | GPIO | 说明 |
|------|------|------|
| PTT 按键 | GPIO21 | 紧急频道按键 |
| 电池检测 | GPIO8 | ADC 电池电压检测 |

## 🔌 硬件要求

### ESP32-S3 模块
- **推荐型号**: ESP32-S3-WROOM-1U
- **Flash**: 8MB 或以上
- **PSRAM**: 不需要（已禁用）
- **天线**: 支持外置天线（U.FL/IPEX 接口）

### 音频硬件
- **麦克风**: PDM 数字麦克风（如 SPH0645LM4H）
- **扬声器**: 8Ω 0.5-1W
- **功放**: I2S 输入功放（如 MAX98357A）

### 天线
- **频段**: 2.4GHz
- **增益**: 5dBi 全向天线（推荐）
- **接口**: U.FL/IPEX 接头

### 电源
- **电压**: 3.3V（ESP32-S3）
- **电流**: 峰值 500mA（发射时）
- **电池**: 3.7V 锂电池 + LDO/DC-DC

## ⚙️ 配置说明

### 修改引脚定义

编辑 `src/main.cpp`，找到引脚定义部分：

```cpp
// I2S引脚配置 - PDM麦克风
#define I2S_MIC_CLK      16  // 修改为你的硬件引脚
#define I2S_MIC_DATA     18

// I2S扬声器
#define I2S_SPK_BCLK     5
#define I2S_SPK_LRCK     6
#define I2S_SPK_DATA     7

// EC11旋转编码器
#define ENCODER_PIN_A 15
#define ENCODER_PIN_B 4
#define ENCODER_BUTTON_PIN 12

// ES16开关
#define ROTARY_PIN_1 35
#define ROTARY_PIN_2 36
#define ROTARY_PIN_4 37
#define ROTARY_PIN_8 38
```

### I2S 配置参数

```cpp
#define SAMPLE_RATE      8000    // 采样率 8kHz
#define BLOCK_SIZE       160     // 帧大小 160 samples (20ms)
#define I2S_DMA_BUF_COUNT 8      // DMA 缓冲区数量
#define I2S_DMA_BUF_LEN   160    // DMA 缓冲区长度
```

## ⚠️ 注意事项

1. **GPIO 冲突**: 确保引脚不与其他外设冲突
2. **电源稳定**: 发射时电流较大，需要稳定电源
3. **天线匹配**: 外置天线需要正确连接到 U.FL 接口
4. **I2S 时序**: PDM 麦克风和 I2S 扬声器使用不同的 I2S 端口

## 🔧 硬件调试

### 串口命令

连接串口（115200 波特率），输入以下命令：

- `w` - 查看 WiFi 配置信息
- `s` - 查看系统状态
- `v` - 调整音量

### LED 指示

根据你的硬件设计添加 LED 指示：
- 绿色：正常工作
- 蓝色：接收音频
- 红色：发射音频

## 📖 参考资料

- [ESP32-S3 技术规格书](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_cn.pdf)
- [ESP32-S3-WROOM-1 数据手册](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_cn.pdf)
- [I2S 驱动文档](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-reference/peripherals/i2s.html)

