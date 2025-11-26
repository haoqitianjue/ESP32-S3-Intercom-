# 项目结构说明

## 📁 目录结构

```
ESP32-S3-Intercom-/
├── src/                          # 源代码目录
│   ├── main.cpp                  # 主程序
│   ├── ilbc_codec.cpp/h          # iLBC 编解码器封装
│   ├── vad_enhanced.cpp/h        # VAD 语音检测
│   ├── continuous_noise_suppressor.cpp/h  # 噪声抑制
│   ├── voice_prompts.cpp/h       # 语音提示
│   └── voice_data.h              # 语音数据
├── lib/                          # 第三方库
│   ├── arduino-libilbc-main/     # iLBC 编解码库
│   └── arduino-audio-tools-1.1.2/ # 音频工具库
├── docs/                         # 文档目录
│   ├── USER_GUIDE_CN.md          # 使用说明（中文）
│   ├── HARDWARE_CN.md            # 硬件接口说明
│   ├── CONFIG_CN.md              # 配置指南
│   ├── FAQ_CN.md                 # 常见问题
│   └── PROJECT_STRUCTURE.md      # 项目结构说明
├── include/                      # 头文件目录
├── platformio.ini                # PlatformIO 配置
├── README.md                     # 项目说明（中英文）
├── LICENSE                       # 开源许可证
└── .gitignore                    # Git 忽略文件

不包含（不开源）：
├── hardware/                     # 硬件设计文件
├── schematic/                    # 原理图
├── pcb/                          # PCB 文件
└── DEVELOPER_LOG.md              # 开发日志
```

## 📄 文件说明

### 核心源文件

#### src/main.cpp
- **功能**: 主程序入口
- **包含**: WiFi/ESP-NOW 初始化、I2S 音频、VAD 检测、音频混音
- **行数**: ~2700 行
- **关键函数**:
  - `setup()`: 系统初始化
  - `loop()`: 主循环
  - `audioTask()`: 音频采集任务
  - `networkTask()`: 网络发送任务
  - `processReceivedAudio()`: 音频混音处理

#### src/ilbc_codec.cpp/h
- **功能**: iLBC 编解码器封装
- **接口**:
  - `initILBCCodecs()`: 初始化编解码器
  - `encodeILBC()`: 编码 PCM → iLBC
  - `decodeILBC()`: 解码 iLBC → PCM

#### src/vad_enhanced.cpp/h
- **功能**: VAD 语音活动检测
- **算法**: 能量、幅度、过零率、峰均比
- **状态机**: 4 状态（静音、起始、活跃、尾音）

#### src/continuous_noise_suppressor.cpp/h
- **功能**: 持续噪声抑制
- **算法**: 噪声门限 + 衰减

#### src/voice_prompts.cpp/h
- **功能**: 语音提示音播放
- **包含**: 开机提示、音量提示、频道提示

#### src/voice_data.h
- **功能**: 语音数据数组
- **格式**: int16_t PCM 数据

### 第三方库

#### arduino-libilbc-main
- **来源**: [pschatzmann/arduino-libilbc](https://github.com/pschatzmann/arduino-libilbc)
- **功能**: iLBC 音频编解码
- **许可**: BSD License

#### arduino-audio-tools-1.1.2
- **来源**: [pschatzmann/arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools)
- **功能**: 音频处理工具（本项目仅使用部分功能）
- **许可**: GPL-3.0 License

### 配置文件

#### platformio.ini
- **平台**: espressif32
- **开发板**: esp32-s3-devkitc-1
- **框架**: Arduino
- **Flash**: 8MB

## 🔧 编译配置

### 内存使用

```
Flash: ~1.2MB / 8MB (15%)
SRAM:  ~180KB / 512KB (35%)
```

### 依赖库

- ESP32 Arduino Core (自动安装)
- arduino-libilbc (已包含)
- arduino-audio-tools (已包含)

## 📝 代码规范

### 命名规范

- **宏定义**: 全大写下划线分隔 `WIFI_CHANNEL`
- **函数**: 驼峰命名 `initWiFi()`
- **变量**: 驼峰命名 `currentChannel`
- **常量**: 全大写 `MAX_PACKET_SIZE`

### 注释规范

- **文件头**: 简要说明文件功能
- **函数**: 说明功能、参数、返回值
- **关键代码**: 行内注释说明逻辑
- **语言**: 中文（面向中文开发者）

## 🚀 构建流程

### 1. 依赖检查
```bash
platformio lib list
```

### 2. 编译
```bash
platformio run
```

### 3. 上传
```bash
platformio run --target upload
```

### 4. 监控
```bash
platformio device monitor
```

## 📊 性能指标

| 指标 | 值 |
|------|-----|
| 音频延迟 | <50ms |
| CPU 占用 | ~60% |
| 内存占用 | ~180KB |
| 功耗（发射） | ~170mA @ 3.3V |
| 功耗（接收） | ~100mA @ 3.3V |

## 🔄 开发流程

1. 修改代码
2. 编译测试
3. 上传到设备
4. 串口调试
5. 实际测试
6. 提交代码

## 📚 参考资料

- [ESP32-S3 技术规格书](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_cn.pdf)
- [ESP-NOW 协议文档](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-reference/network/esp_now.html)
- [iLBC 编解码器规范](https://tools.ietf.org/html/rfc3951)

---

**最后更新：2024-11-23**

