# 使用说明

## 🚀 快速开始

### 1. 硬件准备

确保你的硬件包含以下组件：
- ESP32-S3-WROOM-1U 模块
- PDM 数字麦克风
- I2S 功放 + 扬声器
- 2.4GHz 外置天线（5dBi）
- EC11 旋转编码器（音量控制）
- ES16 16档位开关（频道选择）
- PTT 按键（可选）

### 2. 软件安装

#### 安装 PlatformIO

**方法1：VSCode 插件**
1. 安装 [Visual Studio Code](https://code.visualstudio.com/)
2. 在扩展商店搜索 "PlatformIO IDE"
3. 点击安装

**方法2：命令行**
```bash
pip install platformio
```

#### 克隆项目

```bash
git clone https://github.com/haoqitianjue/ESP32-S3-Intercom-.git
cd ESP32-S3-Intercom-
```

### 3. 配置硬件引脚

根据你的硬件连接，修改 `src/main.cpp` 中的引脚定义：

```cpp
// I2S引脚配置
#define I2S_MIC_CLK      16  // PDM麦克风时钟
#define I2S_MIC_DATA     18  // PDM麦克风数据
#define I2S_SPK_BCLK     5   // 扬声器位时钟
#define I2S_SPK_LRCK     6   // 扬声器字选择
#define I2S_SPK_DATA     7   // 扬声器数据
```

### 4. 编译上传

```bash
# 编译
platformio run

# 上传到设备
platformio run --target upload

# 查看串口输出
platformio device monitor
```

## 🎛️ 基本操作

### 开机

1. 连接电源
2. 等待系统初始化（约2-3秒）
3. 听到开机提示音
4. 绿色 LED 常亮表示正常工作

### 频道选择

使用 ES16 16档位开关选择频道（0-15）：
- 旋转开关到对应档位
- 听到提示音确认切换
- 同一频道的设备可以互相通信

### 音量调节

使用 EC11 旋转编码器调节音量：
- 顺时针旋转：音量增加（每次 5%）
- 逆时针旋转：音量减少（每次 5%）
- 听到高音/低音提示音确认

### 自听音开关

按下 EC11 编码器按键：
- 开启自听音：可以听到自己的声音
- 关闭自听音：不听到自己的声音
- 默认开启

### 语音通话

**自动模式（VAD）：**
- 直接对着麦克风说话
- 系统自动检测语音并发送
- 无需按键操作

**手动模式（PTT）：**
- 按住 PTT 按键（GPIO21）
- 对着麦克风说话
- 松开按键停止发送

## ⚙️ 高级配置

### WiFi 信道设置

编辑 `src/main.cpp`：

```cpp
#define WIFI_CHANNEL 1  // 修改为 1-13
```

**信道选择建议：**
- 信道 1：常用，可能拥挤
- 信道 6：标准推荐
- 信道 11：美国标准
- 信道 13：较少使用，低干扰

**注意：所有设备必须使用相同信道！**

### 音频参数调整

```cpp
#define SAMPLE_RATE      8000    // 采样率（不建议修改）
#define BLOCK_SIZE       160     // 帧大小（不建议修改）
#define AUDIO_GAIN       1.0f    // 麦克风增益（0.5-2.0）
```

### VAD 灵敏度调整

编辑 `src/vad_enhanced.h`：

```cpp
#define VAD_ENERGY_THRESHOLD 1800        // 能量阈值（降低=更灵敏）
#define VAD_AMPLITUDE_THRESHOLD 3500     // 幅度阈值
#define VAD_REQUIRED_VOICE_FRAMES 1      // 触发帧数（1-3）
#define VAD_REQUIRED_SILENCE_FRAMES 12   // 结束帧数（8-15）
```

## 🔧 串口调试命令

连接串口（115200 波特率），输入以下命令：

| 命令 | 功能 |
|------|------|
| `w` | 查看 WiFi 配置信息 |
| `s` | 查看系统状态 |
| `v` | 音量控制菜单 |
| `g` | 会话管理菜单 |

### 查看 WiFi 配置

```
输入: w

输出:
📡 WiFi配置信息
  当前协议:
    - 原始值: 0x09
    - 11b: ✅
    - LR:  ✅ (Long Range模式已启用)
  发射功率: 20.0dBm
  当前信道: 1
```

## 📊 LED 指示说明

根据你的硬件设计，LED 可能有以下状态：

| LED 颜色 | 状态 | 说明 |
|---------|------|------|
| 绿色常亮 | 正常工作 | 系统运行正常 |
| 蓝色闪烁 | 接收音频 | 正在接收其他设备的语音 |
| 红色闪烁 | 发送音频 | 正在发送语音 |
| 红色常亮 | 低电量 | 电池电压 <3.4V |

## ⚠️ 常见问题

### 1. 无法通信

**检查清单：**
- ✅ 所有设备使用相同信道
- ✅ 所有设备使用相同频道（ES16开关）
- ✅ 天线正确连接
- ✅ 距离在有效范围内（<200米）

### 2. 音质不佳

**优化方法：**
- 调整麦克风增益（AUDIO_GAIN）
- 调整 VAD 阈值
- 检查麦克风和扬声器连接
- 远离干扰源

### 3. 延迟过高

**检查项：**
- 确认 LR 模式已启用（串口输入 `w` 查看）
- 检查 WiFi 信道是否拥挤
- 减少同时通话设备数量

更多问题请查看 [常见问题](FAQ_CN.md)。

