# 配置指南

## 📋 配置文件位置

所有配置参数位于 `src/main.cpp` 文件中。

## 🔧 基本配置

### WiFi 信道配置

```cpp
#define WIFI_CHANNEL 1  // WiFi 通信信道 (1-13)
```

**说明：**
- 所有设备必须使用相同信道
- 推荐信道：1、6、11、13
- 避开拥挤信道可提升通信质量

**信道对比：**
| 信道 | 频率 | 特点 |
|------|------|------|
| 1 | 2412MHz | 常用，可能拥挤 |
| 6 | 2437MHz | 标准推荐 |
| 11 | 2462MHz | 美国标准 |
| 13 | 2472MHz | 较少使用，低干扰 |

---

### 音频参数配置

```cpp
#define SAMPLE_RATE      8000    // 采样率 (Hz)
#define BLOCK_SIZE       160     // 帧大小 (samples)
#define AUDIO_GAIN       1.0f    // 麦克风增益 (0.5-2.0)
```

**说明：**
- `SAMPLE_RATE`: 固定 8000Hz（iLBC 要求）
- `BLOCK_SIZE`: 固定 160 samples（20ms @ 8kHz）
- `AUDIO_GAIN`: 根据麦克风灵敏度调整
  - 0.5-0.8: 高灵敏度麦克风
  - 1.0: 标准麦克风
  - 1.2-2.0: 低灵敏度麦克风

---

### 音量配置

```cpp
#define VOLUME_STEP 5      // 音量步进 (%)
#define VOLUME_INITIAL 50  // 初始音量 (0-100)
```

**说明：**
- `VOLUME_STEP`: 每次旋转编码器的音量变化
- `VOLUME_INITIAL`: 开机默认音量

---

## 🎙️ VAD 配置

### VAD 阈值配置

编辑 `src/vad_enhanced.h`：

```cpp
#define VAD_ENERGY_THRESHOLD 1800        // 能量阈值
#define VAD_AMPLITUDE_THRESHOLD 3500     // 幅度阈值
#define VAD_ZERO_CROSSING_MIN 12         // 最小过零率
#define VAD_ZERO_CROSSING_MAX 65         // 最大过零率
#define VAD_PEAK_TO_AVERAGE_RATIO 3.2f   // 峰均比
```

**调整建议：**

**环境安静（降低灵敏度）：**
```cpp
#define VAD_ENERGY_THRESHOLD 2500
#define VAD_AMPLITUDE_THRESHOLD 4500
```

**环境嘈杂（提高灵敏度）：**
```cpp
#define VAD_ENERGY_THRESHOLD 1500
#define VAD_AMPLITUDE_THRESHOLD 3000
```

---

### VAD 时序配置

```cpp
#define VAD_REQUIRED_VOICE_FRAMES 1      // 触发帧数 (1-3)
#define VAD_REQUIRED_SILENCE_FRAMES 12   // 结束帧数 (8-15)
#define VAD_MIN_VOICE_DURATION_MS 60     // 最小语音时长 (ms)
#define VAD_HANGOVER_TIME_MS 200         // 尾音保持时间 (ms)
```

**调整建议：**

**快速响应（降低延迟）：**
```cpp
#define VAD_REQUIRED_VOICE_FRAMES 1
#define VAD_REQUIRED_SILENCE_FRAMES 8
#define VAD_HANGOVER_TIME_MS 150
```

**稳定性优先（减少误触发）：**
```cpp
#define VAD_REQUIRED_VOICE_FRAMES 3
#define VAD_REQUIRED_SILENCE_FRAMES 15
#define VAD_HANGOVER_TIME_MS 250
```

---

## 📡 网络配置

### ESP-NOW 配置

```cpp
#define MAX_PACKET_SIZE 250  // ESP-NOW 最大负载 (bytes)
```

**说明：**
- ESP-NOW 协议限制最大 250 字节
- 当前音频包 40 字节（iLBC-20ms）
- 不建议修改

---

### 设备数量配置

```cpp
#define MAX_SUPPORTED_DEVICES 20  // 最大支持设备数
```

**说明：**
- 理论支持 20 设备
- 实际建议 5-8 设备（带宽限制）
- 增加设备数会增加内存占用

---

## 🔊 音频混音配置

```cpp
#define SOURCE_TIMEOUT_MS 100         // 音频源超时 (ms)
#define MAX_SIMULTANEOUS_TALKERS 8    // 最大同时说话人数
```

**说明：**
- `SOURCE_TIMEOUT_MS`: 音频源无数据后的超时时间
- `MAX_SIMULTANEOUS_TALKERS`: 同时混音的最大设备数

---

## 🔌 硬件引脚配置

### I2S 引脚

```cpp
// PDM 麦克风
#define I2S_MIC_CLK      16
#define I2S_MIC_DATA     18

// I2S 扬声器
#define I2S_SPK_BCLK     5
#define I2S_SPK_LRCK     6
#define I2S_SPK_DATA     7
```

### 控制引脚

```cpp
// EC11 旋转编码器
#define ENCODER_PIN_A 15
#define ENCODER_PIN_B 4
#define ENCODER_BUTTON_PIN 12

// ES16 开关
#define ROTARY_PIN_1 35
#define ROTARY_PIN_2 36
#define ROTARY_PIN_4 37
#define ROTARY_PIN_8 38

// PTT 按键
#define PTT_PIN 21

// 电池检测
#define BATTERY_ADC_PIN 8
```

---

## ⚙️ 高级配置

### I2S DMA 配置

```cpp
#define I2S_DMA_BUF_COUNT 8    // DMA 缓冲区数量
#define I2S_DMA_BUF_LEN 160    // DMA 缓冲区长度
```

**说明：**
- 增加缓冲区数量可提高稳定性，但增加延迟
- 减少缓冲区数量可降低延迟，但可能丢帧
- 不建议修改

---

### 噪声抑制配置

编辑 `src/continuous_noise_suppressor.h`：

```cpp
#define NOISE_GATE_THRESHOLD -26.0f  // 噪声门限 (dB)
#define NOISE_ATTENUATION 0.05f      // 噪声衰减系数
```

**调整建议：**
- 降低 `NOISE_GATE_THRESHOLD` 可抑制更多噪声
- 增加 `NOISE_ATTENUATION` 可加强抑制效果

---

## 📝 配置模板

### 场景1：室内对讲（50-100米）

```cpp
#define WIFI_CHANNEL 1
#define AUDIO_GAIN 1.0f
#define VAD_ENERGY_THRESHOLD 2000
#define VAD_REQUIRED_VOICE_FRAMES 1
```

### 场景2：室外对讲（150-200米）

```cpp
#define WIFI_CHANNEL 13
#define AUDIO_GAIN 1.2f
#define VAD_ENERGY_THRESHOLD 1800
#define VAD_REQUIRED_VOICE_FRAMES 2
```

### 场景3：嘈杂环境

```cpp
#define AUDIO_GAIN 0.8f
#define VAD_ENERGY_THRESHOLD 3000
#define VAD_AMPLITUDE_THRESHOLD 4500
#define NOISE_GATE_THRESHOLD -30.0f
```

---

## ⚠️ 注意事项

1. **修改配置后必须重新编译上传**
2. **所有设备必须使用相同的信道配置**
3. **不建议修改采样率和帧大小**
4. **修改 VAD 参数需要实际测试调整**

---

**最后更新：2024-11-23**

