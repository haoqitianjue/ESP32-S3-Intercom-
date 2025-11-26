/**
 * ESP32-S3 无线多人对讲系统 - 优化版 (iLBC-20ms)
 * 基于ESP-NOW协议，VAD增强语音检测 + VOX传输控制
 */

 #include <Arduino.h>
 #include <WiFi.h>
 #include <esp_now.h>
 #include <esp_wifi.h>
 #include <driver/i2s.h>
 #include <driver/gpio.h>
 #include <driver/adc.h>
 #include <esp_adc_cal.h>
 #include <freertos/FreeRTOS.h>
 #include <freertos/task.h>
 #include <freertos/queue.h>

 // 包含语音数据头文件
 #include "voice_data.h"
 #include "voice_prompts.h"
 #include "vad_enhanced.h"
 #include "continuous_noise_suppressor.h"
 #include "ilbc_codec.h"

 // 函数声明
 uint8_t readRotarySwitch();
 void playRotarySwitchBeep();
 void playChannelVoice(uint8_t channel);

 //================== 基本配置 ==================
 // MAC地址映射系统 - 替换原有的动态设备ID系统
 bool shouldSendStartupPackets = false;  // 启动包发送标记
 #define WIFI_CHANNEL 1 // WiFi 通信信道 (1-13)

 // 頻道相關定義
 #define CHANNEL_MASK 0xF000  // 高4位用於頻道信息
 #define SEQUENCE_MASK 0x0FFF // 低12位用於序列號
 volatile uint8_t currentChannel = 0;  // 當前選擇的頻道，默認為0

 // ES16 16檔位絕對值8421編碼開關配置
 #define ROTARY_PIN_1 35  // 最低位 (1)
 #define ROTARY_PIN_2 36  // 次低位 (2)
 #define ROTARY_PIN_4 37  // 次高位 (4)
 #define ROTARY_PIN_8 38  // 最高位 (8)

 // 旋轉開關提示音配置
 #define SWITCH_BEEP_DURATION 80    // 提示音持續時間(毫秒)
 #define SWITCH_BEEP_FREQ 1000      // 提示音頻率(Hz)
 #define SWITCH_BEEP_VOLUME 0.1f    // 提示音音量(0.0-1.0) - 统一10%

 // EC11旋转编码器配置
 #define ENCODER_PIN_A 15
 #define ENCODER_PIN_B 4
 #define ENCODER_BUTTON_PIN 12    // EC11按钮引脚，用于控制自听音
 #define VOLUME_STEP 5      // 每次步进5%
 #define VOLUME_INITIAL 50  // 初始音量50% (适合压差式麦克风)

 // I2S引脚配置 - PDM麦克风
 #define I2S_MIC_CLK      16  // PDM CLK连接到IO16
 #define I2S_MIC_DATA     18  // PDM DAT连接到IO18
 #define I2S_SPK_BCLK     5
 #define I2S_SPK_LRCK     6
 #define I2S_SPK_DATA     7

 // I2S DMA 配置
 #define I2S_DMA_BUF_COUNT 8
 #define I2S_DMA_BUF_LEN   BLOCK_SIZE // Samples per DMA buffer

 // 音频参数
 #define SAMPLE_RATE      8000
 #define BLOCK_SIZE       160    // 音频块大小(采样点数), 8kHz时对应20ms. iLBC-20ms requires 160 samples per frame.
 #define AUDIO_GAIN      1.0f   // 麦克风增益 - 降低20%以减少噪音收集

 // 网络参数
 #define MAX_PACKET_SIZE  250   // ESP-NOW最大支持250字节负载

 //================== iLBC编解码器 ==================
 // iLBC编解码器在ilbc_codec.cpp中定义

 // Clamp function
 static inline int16_t clamp16(int32_t sample) {
     if (sample < -32768) return -32768;
     if (sample > 32767) return 32767;
     return (int16_t)sample;
 }
 static inline int8_t clamp8(int val, int min_val, int max_val) {
     if (val < min_val) return min_val;
     if (val > max_val) return max_val;
     return val;
 }


 //================== 音频数据包结构 (iLBC-20ms) ==================
 // 优化：删除deviceId字段，使用ESP-NOW提供的MAC地址识别设备
 // 节省1字节带宽，逻辑更清晰
 typedef struct {
     uint16_t sequence;        // 序列号（高4位存储频道信息）
     uint8_t ilbc_data[38];    // iLBC-20ms压缩后的音频数据 (160 samples -> 38 bytes)
 } __attribute__((packed)) AudioPacket;  // 40字节（原41字节）

 // 頻道相關函數聲明
 void setChannelToPacket(AudioPacket* packet, uint8_t channel);
 uint8_t getChannelFromPacket(const AudioPacket* packet);
 uint16_t getSequenceFromPacket(const AudioPacket* packet);
 void updateChannelDisplay();



 //================== 设备配置 ==================
 #define MAX_SUPPORTED_DEVICES 20  // 扩展到20个设备（内存占用仅6.9KB）
 #define SEQUENCE_WRAP_THRESHOLD 10000

 //================== 音频混音参数 ==================
 #define SOURCE_TIMEOUT_MS 100         // 音频源活跃超时时间(ms)
 #define MAX_SIMULTANEOUS_TALKERS 8    // 最大同时说话人数（防止音量过低）

 // 音频源结构（简化版本）
 typedef struct {
     int16_t buffer[BLOCK_SIZE];  // 音频数据缓冲区
     uint32_t timestamp;          // 最后更新时间戳
     bool active;                 // 活跃状态
 } AudioSource;

 AudioSource audioSources[MAX_SUPPORTED_DEVICES];

 //================== 全局变量 ==================
 // 音频缓冲区
 int16_t micBuffer[BLOCK_SIZE];
 int16_t speakerBuffer[BLOCK_SIZE];

 //================== 自听音相关变量 ==================
 #define SIDETONE_VOLUME 0.333f     // 自听音基础音量系数 (0.0-1.0) - 默认30%系统音量时为10%
 volatile bool sidetoneEnabled = true; // 自听音开关标志，默认开启
 int16_t sidetoneBuffer[BLOCK_SIZE];   // 自听音缓冲区
 int16_t remoteBuffer[BLOCK_SIZE];     // 远程音频缓冲区
 int16_t outputBuffer[BLOCK_SIZE];     // 最终输出混合缓冲区
 volatile bool sidetoneReady = false;  // 自听音是否就绪
 volatile bool remoteReady = false;    // 远程音频是否就绪
 volatile bool sidetoneStatusChanged = false; // 自听音状态变化标志

//================== 音量提示音相关变量 ==================
volatile bool volumeUpPrompt = false;    // 音量增加提示标志
volatile bool volumeDownPrompt = false;  // 音量减少提示标志
unsigned long lastVolumePromptTime = 0;  // 上次提示音播放时间
#define VOLUME_PROMPT_INTERVAL 150       // 提示音播放间隔(ms)

//================== 语音提示相关变量 ==================
#define VOICE_PROMPT_VOLUME 0.1f   // 语音提示音量系数 (0.0-1.0) - 统一10%


// 语音提示播放状态
volatile bool voicePromptPlaying = false;
volatile size_t voicePromptIndex = 0;
volatile const int16_t* currentVoiceData = nullptr;
volatile size_t currentVoiceLength = 0;

 // 旋轉編碼開關去抖動相關變量
 #define ROTARY_DEBOUNCE_TIME 150      // 旋轉開關去抖時間提高到150毫秒
 #define ROTARY_CONFIRM_READS 5        // 讀取穩定確認次數增加到5次
 #define ROTARY_HISTORY_SIZE 3         // 歷史位置緩衝區大小
 volatile uint8_t lastStablePosition = 255;   // 上次穩定的位置值（初始為無效值）
 volatile unsigned long lastRotaryChangeTime = 0; // 上次旋轉開關變化時間
 volatile uint8_t positionHistory[ROTARY_HISTORY_SIZE] = {255, 255, 255}; // 位置歷史緩衝區
 volatile uint8_t historyIndex = 0;     // 歷史緩衝區索引

 // 音量控制变量
 volatile int volumeLevel = VOLUME_INITIAL;  // 音量级别(0-100)
 volatile bool isMuted = false;              // 静音状态
 volatile int lastEncoded = 0;
 volatile unsigned long lastEncoderTime = 0;
 volatile unsigned long lastButtonTime = 0;  // 按钮去抖动时间戳
 volatile bool encoderActive = false;        // 编码器活动标志，用于屏蔽按键误触发
 volatile int encoderActivityCount = 0;      // 编码器活动计数器，用于增强屏蔽


 // 统计信息
 uint16_t txSequence = 0;
 uint32_t sentPackets = 0;
 uint32_t receivedPackets = 0;
 uint32_t totalRetransmitRequests = 0;
 uint32_t totalRetransmits = 0;
 float signalQuality = 100.0;



 // 电池监测相关变量
 float batteryVoltage = 0.0f;           // 当前电池电压
 bool batteryHigh = true;               // 电池高电量标志（3.8V-4.2V）
 bool batteryMedium = false;            // 电池中等电量标志（3.4V-3.7V）
 bool batteryLow = false;               // 电池低电量标志（<3.4V）
 bool batteryCritical = false;          // 电池超低电量标志（≤ 3.3V）



 // 电池监测引脚定义
 #define BATTERY_ADC_PIN 9       // 电池电压检测引脚 (IO9, 对应ADC1_CH8)
 #define BATTERY_GREEN_LED_PIN 45 // 绿色LED引脚（高电平有效）
 #define BATTERY_BLUE_LED_PIN 47  // 蓝色LED引脚（高电平有效）
 #define BATTERY_RED_LED_PIN 48   // 红色LED引脚（高电平有效）



 // 电池电压相关参数
 #define BATTERY_ADC_RESOLUTION 4095.0f  // 12位 ADC分辨率 (2^12 - 1)
 #define BATTERY_ADC_REFERENCE 3.3f      // ADC参考电压
 #define BATTERY_DIVIDER_RATIO 2.01f     // 电压分压比例（基于实测值计算：3.98V/1.98V）
 #define BATTERY_CALIBRATION_FACTOR 1.036f // 校准系数，补偿读数偏差（基于3.98V实测值校准）

 // 电池电压阈值 - 优化电量指示精度
 #define BATTERY_HIGH_VOLTAGE 3.8f      // 高电量阈值（≥ 3.8V）- 绿色LED
 #define BATTERY_MEDIUM_VOLTAGE 3.6f    // 中等电量阈值（3.6V-3.8V）- 蓝色LED
 #define BATTERY_LOW_VOLTAGE 3.4f       // 低电量阈值（3.4V-3.6V）- 红色LED
 #define BATTERY_CRITICAL_VOLTAGE 3.4f  // 超低电量阈值（< 3.4V）- 红色LED+语音提示

 // 电池状态滑动窗口，避免频繁切换
 #define BATTERY_HYSTERESIS 0.1f       // 电压滑动窗口（0.1V）

 // 电池提示音相关参数
 #define BATTERY_BEEP_DURATION 150     // 提示音持续时间(ms)
 #define BATTERY_BEEP_FREQ 1500        // 提示音频率(Hz)
 #define BATTERY_BEEP_VOLUME 0.1f      // 提示音音量(0.0-1.0)

 // 广播地址
 uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};



 //================== VOX传输控制参数 ==================
 // VOX现在只负责传输时序控制，语音检测完全依赖VAD增强模块
 #define VOX_TAIL_MS 170              // VOX尾部时间 - 语音结束后继续传输的时间
 #define VOX_MIN_DURATION_MS 80       // 最小传输持续时间 - 避免过短传输
 #define FADE_OUT_PACKETS 4           // 渐进式静音包数量 - 平滑结束传输
 volatile bool voxActive = false;
 unsigned long voxStartTime = 0;
 unsigned long lastVoiceTime = 0;
 int fadeOutPacketsRemaining = 0;  // 剩余需要发送的静音包数量

 // VOX相关变量
 bool previousVoxActive = false;       // 上一次VOX状态
 int voxEndingPhase = 0;               // VOX结束阶段：0=未结束, 1=正在结束, 2=已应用噪声抑制

 //================== MAC地址映射系统 ==================
 // 设备映射表 - 基于MAC地址的设备管理
 struct DeviceMapping {
     uint8_t mac_addr[6];      // 完整MAC地址（6字节，全球唯一）
     uint8_t array_index;      // 在audioSources数组中的索引 (0-9)
     uint32_t last_seen;       // 最后活跃时间
     uint16_t expected_seq;    // 该设备的期望序列号
     bool is_active;           // 是否活跃
     uint32_t packet_count;    // 收到的包数量
     char mac_str[18];         // MAC地址字符串表示（用于调试）
 };

 DeviceMapping deviceMappings[MAX_SUPPORTED_DEVICES];
 uint8_t activeMappingCount = 0;

 // MAC地址映射函数声明
 void initDeviceMappingSystem();
 uint8_t getDeviceIndexByMac(const uint8_t* mac_addr);
 uint8_t getMyDeviceIndex();
 void cleanupInactiveDeviceMappings();
 void printDeviceMappings();
 bool macEquals(const uint8_t* mac1, const uint8_t* mac2);
 void macToString(const uint8_t* mac, char* str);
 uint8_t calculateDeterministicIndex(const uint8_t* mac_addr);

 //================== 异步发送任务配置 ==================
 #define TX_QUEUE_SIZE 15
 #define SEND_TASK_STACK_SIZE 49152   // 调整栈大小 (48KB) - 平衡稳定性和内存使用
 #define SEND_TASK_PRIORITY 3     // 提高优先级 (原为1)
 #define SEND_TASK_CORE 1
 QueueHandle_t txQueue = NULL;  // 异步发送队列
 static TaskHandle_t sendTaskHandle = NULL;

 //================== 心跳机制配置 ==================
 #define HEARTBEAT_INTERVAL_MS 2000  // 心跳间隔
 #define MAX_STARTUP_BROADCASTS 5    // 启动时的广播次数

 typedef struct {
     uint8_t deviceId;
     uint8_t type;   // 0=心跳, 1=设备启动通知
     uint8_t channel; // 发送设备的WiFi通道
 } __attribute__((packed)) HeartbeatPacket;

 bool sendHeartbeat(uint8_t type);
 unsigned long lastHeartbeatTime = 0;
 int startupBroadcastCount = 0;

 //================== 平衡自听和网络传输的音频缓冲区 ==================
 // 这部分将被替换为彻底隔离方案

 // 定义音频块结构体 - 用于在两个任务间传递完整音频信息
 typedef struct {
     int16_t samples[BLOCK_SIZE];      // 音频样本
     bool voxActive;                   // VOX状态
     unsigned long timestamp;          // 时间戳
 } AudioBlock;

 // 网络音频处理队列和相关参数
 #define NETWORK_QUEUE_SIZE 4          // 增加队列深度以处理发送和接收

 // 网络任务数据类型
 typedef enum {
     NETWORK_DATA_SEND,    // 发送音频块
     NETWORK_DATA_RECEIVE  // 接收音频包
 } NetworkDataType;

 typedef struct {
     NetworkDataType type;
     union {
         AudioBlock sendBlock;      // 用于发送
         AudioPacket receivePacket; // 用于接收
     } data;
     uint8_t deviceIndex;  // 接收时使用：发送设备的索引（从MAC地址映射）
 } NetworkData;

 QueueHandle_t networkAudioQueue = NULL;  // 网络音频队列
 volatile bool networkTaskRunning = false;  // 网络任务状态
 TaskHandle_t networkTaskHandle = NULL;     // 网络任务句柄

 //================== VAD增强模块 ==================
 VADEnhanced vadEnhanced;  // VAD增强实例

//================== 连续噪声抑制模块 ==================
ContinuousNoiseSuppressor continuousNoiseSuppressor;  // 连续噪声抑制实例

 //================== 函数声明 ==================
 // 初始化
 bool initWiFi();
 bool initI2S();
 bool initESPNow();

 void initSendTask();
 void initVolumeEncoder();
 void scanMacAddress();
 void initRotarySwitch(); // 新增旋轉開關初始化函數聲明

 // 回调函数
 void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
 void onDataReceived(const uint8_t *mac_addr, const uint8_t *data, int len);
 void IRAM_ATTR updateEncoder();

 // 音频处理
 bool readMic(int16_t* buffer, size_t samples);
 bool playSpeaker(int16_t* buffer, size_t samples);
 void processAudio();

 void applyGain(int16_t* buffer, size_t samples, float gain);
 void checkVolumeChange();
 void toggleMute();
 void updateAudioSource(uint8_t deviceId, int16_t* buffer);
 void processReceivedAudio();
 void IRAM_ATTR handleEncoderButton(); // 新增EC11按钮中断处理函数声明



 // 网络发送
 bool sendAudioPacket(AudioPacket* packet);
 void sendTask(void *pvParameters);
 bool sendHeartbeat(uint8_t type);
 void sendStartupDetectionPackets();

 // LED和电池监测相关
 void initLEDs();           // LED初始化函数
 void initBatteryMonitor(); // 电池监测初始化
 float readBatteryVoltage();
 void updateBatteryStatus();
 void playBatteryLowBeep();



 // 平衡处理相关
 void initNetworkTask();
 void networkTask(void* parameter);

 // 工具函数
 void updateSignalQuality();



 //================== MAC地址映射函数实现 ==================
 // 将MAC地址转换为可读字符串
 void macToString(const uint8_t* mac, char* str) {
     sprintf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
 }

 // 比较两个MAC地址是否相同
 bool macEquals(const uint8_t* mac1, const uint8_t* mac2) {
     return memcmp(mac1, mac2, 6) == 0;
 }

 // 基于MAC地址计算确定性索引（所有设备计算结果一致）
 uint8_t calculateDeterministicIndex(const uint8_t* mac_addr) {
     // 使用MAC地址的最后一个字节作为基础
     // 这确保所有设备对同一个MAC地址计算出相同的索引
     uint8_t baseIndex = mac_addr[5] % MAX_SUPPORTED_DEVICES;

     // 如果是本机MAC，总是返回0
     uint8_t my_mac[6];
     WiFi.macAddress(my_mac);
     if (macEquals(mac_addr, my_mac)) {
         return 0;
     }

     // 其他设备使用1-9的索引
     if (baseIndex == 0) {
         baseIndex = 1;  // 避免与本机索引0冲突
     }

     return baseIndex;
 }

 // 根据MAC地址获取设备在audioSources数组中的索引
 // 这是核心函数，保证100%无冲突
 uint8_t getDeviceIndexByMac(const uint8_t* mac_addr) {
     uint32_t currentTime = millis();

     // 1. 首先查找现有映射
     for (int i = 0; i < activeMappingCount; i++) {
         if (macEquals(deviceMappings[i].mac_addr, mac_addr)) {
             // 找到现有设备，更新活跃时间
             deviceMappings[i].last_seen = currentTime;
             deviceMappings[i].is_active = true;
             deviceMappings[i].packet_count++;

             #ifdef DEBUG_MODE
             Serial.printf("📱 设备 %s 使用现有索引 %d\n",
                          deviceMappings[i].mac_str, deviceMappings[i].array_index);
             #endif

             return deviceMappings[i].array_index;
         }
     }

     // 2. 新设备：使用确定性索引分配
     if (activeMappingCount < MAX_SUPPORTED_DEVICES) {
         // 计算确定性索引（所有设备对同一MAC计算结果相同）
         uint8_t deterministicIndex = calculateDeterministicIndex(mac_addr);

         // 检查该索引是否已被其他设备使用
         bool indexConflict = false;
         for (int i = 0; i < activeMappingCount; i++) {
             if (deviceMappings[i].array_index == deterministicIndex) {
                 indexConflict = true;
                 break;
             }
         }

         uint8_t finalIndex = deterministicIndex;

         // 如果有冲突，查找下一个可用索引
         if (indexConflict) {
             bool indexUsed[MAX_SUPPORTED_DEVICES] = {false};

             // 标记已使用的索引
             for (int i = 0; i < activeMappingCount; i++) {
                 if (deviceMappings[i].array_index < MAX_SUPPORTED_DEVICES) {
                     indexUsed[deviceMappings[i].array_index] = true;
                 }
             }

             // 从确定性索引开始查找下一个空闲索引
             for (int offset = 1; offset < MAX_SUPPORTED_DEVICES; offset++) {
                 uint8_t candidateIndex = (deterministicIndex + offset) % MAX_SUPPORTED_DEVICES;
                 if (candidateIndex != 0 && !indexUsed[candidateIndex]) {  // 避免索引0（本机专用）
                     finalIndex = candidateIndex;
                     break;
                 }
             }
         }

         // 分配新映射
         DeviceMapping* newMapping = &deviceMappings[activeMappingCount];
         memcpy(newMapping->mac_addr, mac_addr, 6);
         newMapping->array_index = finalIndex;
         newMapping->last_seen = currentTime;
         newMapping->expected_seq = 0;
         newMapping->is_active = true;
         newMapping->packet_count = 1;
         macToString(mac_addr, newMapping->mac_str);

         Serial.printf("🆕 新设备 %s 分配索引 %d (确定性索引: %d)\n",
                      newMapping->mac_str, finalIndex, deterministicIndex);

         activeMappingCount++;
         return finalIndex;
     }

     // 3. 没有空闲位置：回收最久未使用的索引
     DeviceMapping* oldestMapping = &deviceMappings[0];
     uint32_t oldestTime = oldestMapping->last_seen;

     for (int i = 1; i < MAX_SUPPORTED_DEVICES; i++) {
         if (deviceMappings[i].last_seen < oldestTime) {
             oldestTime = deviceMappings[i].last_seen;
             oldestMapping = &deviceMappings[i];
         }
     }

     // 回收最旧的映射
     Serial.printf("♻️ 回收设备 %s 的索引 %d，分配给新设备 ",
                  oldestMapping->mac_str, oldestMapping->array_index);

     memcpy(oldestMapping->mac_addr, mac_addr, 6);
     oldestMapping->last_seen = currentTime;
     oldestMapping->expected_seq = 0;  // 重置序列号
     oldestMapping->is_active = true;
     oldestMapping->packet_count = 1;
     macToString(mac_addr, oldestMapping->mac_str);

     Serial.printf("%s\n", oldestMapping->mac_str);

     // 清理对应的音频源数据
     uint8_t recycledIndex = oldestMapping->array_index;
     memset(&audioSources[recycledIndex], 0, sizeof(AudioSource));

     return recycledIndex;
 }

 // 获取本机在映射表中的索引（用于显示和统计）
 uint8_t getMyDeviceIndex() {
     // 返回本机在映射表中的真实索引
     // 注意：发送数据包时不再使用此值（已删除deviceId字段）
     return deviceMappings[0].array_index;
 }

 // 清理非活跃设备映射
 void cleanupInactiveDeviceMappings() {
     uint32_t currentTime = millis();
     const uint32_t DEVICE_TIMEOUT_MS = 10000;  // 10秒超时

     for (int i = 0; i < activeMappingCount; i++) {
         if (currentTime - deviceMappings[i].last_seen > DEVICE_TIMEOUT_MS) {
             deviceMappings[i].is_active = false;

             #ifdef DEBUG_MODE
             Serial.printf("⏰ 设备 %s (索引 %d) 超时，标记为非活跃\n",
                          deviceMappings[i].mac_str, deviceMappings[i].array_index);
             #endif

             // 清理对应的音频源数据
             uint8_t inactiveIndex = deviceMappings[i].array_index;
             if (inactiveIndex < MAX_SUPPORTED_DEVICES) {
                 memset(&audioSources[inactiveIndex], 0, sizeof(AudioSource));
             }
         }
     }
 }

 // 初始化设备映射系统
 void initDeviceMappingSystem() {
     memset(deviceMappings, 0, sizeof(deviceMappings));
     activeMappingCount = 0;

     // 将本机添加到映射表（索引0）
     uint8_t my_mac[6];
     WiFi.macAddress(my_mac);

     memcpy(deviceMappings[0].mac_addr, my_mac, 6);
     deviceMappings[0].array_index = 0;
     deviceMappings[0].last_seen = millis();
     deviceMappings[0].expected_seq = 0;
     deviceMappings[0].is_active = true;
     deviceMappings[0].packet_count = 0;
     macToString(my_mac, deviceMappings[0].mac_str);

     activeMappingCount = 1;

     Serial.printf("🏠 本机设备 %s 使用索引 0\n", deviceMappings[0].mac_str);
 }

 // 显示当前设备映射状态
 void printDeviceMappings() {
     Serial.println("\n� 当前设备映射表:");
     Serial.println("索引 | MAC地址           | 活跃 | 包数量 | 最后活跃");
     Serial.println("-----|-------------------|------|--------|----------");

     for (int i = 0; i < activeMappingCount; i++) {
         DeviceMapping* mapping = &deviceMappings[i];
         uint32_t timeSince = millis() - mapping->last_seen;

         Serial.printf(" %2d  | %s | %s | %6lu | %lu秒前\n",
                      mapping->array_index,
                      mapping->mac_str,
                      mapping->is_active ? " ✓ " : " ✗ ",
                      mapping->packet_count,
                      timeSince / 1000);
     }

     // 验证映射表一致性
     bool indexUsed[MAX_SUPPORTED_DEVICES] = {false};
     for (int i = 0; i < activeMappingCount; i++) {
         uint8_t idx = deviceMappings[i].array_index;
         if (idx >= MAX_SUPPORTED_DEVICES) {
             Serial.printf("⚠️ 警告：映射%d使用了无效索引%d\n", i, idx);
         } else if (indexUsed[idx]) {
             Serial.printf("⚠️ 警告：索引%d被多个设备使用\n", idx);
         } else {
             indexUsed[idx] = true;
         }
     }
 }



 //================== 设置函数 ==================
 void setup() {
     // 首先初始化LED，让用户知道设备已正确通电
     Serial.println("\n[0] 初始化LED指示灯...");
     initLEDs();

     // 确保USB CDC正确初始化
     Serial.begin(115200);

     // 等待USB CDC串口准备好（最多等待1秒）
     int usbWaitStart = millis();
     while(!Serial && (millis() - usbWaitStart < 1000)) {
         delay(10); // 缩短等待间隔
     }

     // 强制刷新串口
     Serial.flush();
     // 移除不必要的延迟

     // 只发送一次初始消息，减少启动延迟
     Serial.println("\n\n==== ESP32-S3 无线对讲系统启动 ====");

     Serial.println("\n🚀 ESP32-S3 无线多人对讲系统 - 优化版 (iLBC-20ms) 🚀");

     // 初始化音频源数组
     memset(audioSources, 0, sizeof(audioSources));

     // 初始化音频源
     for (int i = 0; i < MAX_SUPPORTED_DEVICES; i++) {
         memset(&audioSources[i], 0, sizeof(AudioSource));
     }

     Serial.println("\n[1] 初始化WiFi...");
     if (!initWiFi()) { Serial.println("❌ WiFi 初始化失败! 系统停止。"); while(1) delay(1000); }

     Serial.println("\n[2] 初始化ESP-NOW...");
     if (!initESPNow()) { Serial.println("❌ ESP-NOW 初始化失败! 系统停止。"); while(1) delay(1000); }

     // 初始化MAC地址映射系统
     Serial.println("\n[2.1] 初始化MAC地址映射系统...");
     initDeviceMappingSystem();

     // 发送启动通知
     Serial.println("\n[2.2] 发送设备启动通知...");
     sendHeartbeat(1);
     startupBroadcastCount = 1;

     // 启动检测包将在主循环中发送，避免setup()中的栈溢出
     Serial.println("\n[2.3] 启动检测包将在主循环中发送...");

     // 网络初始化完成后，重新设置LED状态
     Serial.println("\n[2.3] 重新设置LED状态...");
     // 根据当前电池状态设置LED
     if (batteryHigh) {
         // 高电量 - 绿色LED亮
         digitalWrite(BATTERY_GREEN_LED_PIN, HIGH);
         digitalWrite(BATTERY_BLUE_LED_PIN, LOW);
         digitalWrite(BATTERY_RED_LED_PIN, LOW);
     } else if (batteryMedium) {
         // 中等电量 - 蓝色LED亮
         digitalWrite(BATTERY_GREEN_LED_PIN, LOW);
         digitalWrite(BATTERY_BLUE_LED_PIN, HIGH);
         digitalWrite(BATTERY_RED_LED_PIN, LOW);
     } else {
         // 低电量 - 红色LED亮
         digitalWrite(BATTERY_GREEN_LED_PIN, LOW);
         digitalWrite(BATTERY_BLUE_LED_PIN, LOW);
         digitalWrite(BATTERY_RED_LED_PIN, HIGH);
     }

     Serial.println("\n[3] 初始化I2S...");
     if (!initI2S()) { Serial.println("❌ I2S 初始化失败! 系统停止。"); while(1) delay(1000); }

     // 注释：序列号跟踪现在在DeviceMapping中管理

     Serial.println("\n[6] 初始化异步发送任务...");
     initSendTask();

     Serial.println("\n[6.1] 初始化平衡处理任务...");
     initNetworkTask();

     Serial.println("\n[6.2] 初始化VAD增强模块...");
     if (!vadEnhanced.init()) {
         Serial.println("❌ VAD增强模块初始化失败! 系统停止。");
         while(1) delay(1000);
     }
     Serial.println("✅ VAD增强模块初始化完成 (状态机VAD算法)");

    Serial.println("\n[6.3] 初始化连续噪声抑制模块...");
    if (!continuousNoiseSuppressor.init()) {
        Serial.println("❌ 连续噪声抑制模块初始化失败! 系统停止。");
        while(1) delay(1000);
    }
    Serial.println("✅ 连续噪声抑制模块初始化完成");

    Serial.println("\n[6.4] 初始化iLBC音频编解码器...");
    if (!initILBCCodecs()) {
        Serial.println("❌ iLBC编解码器初始化失败! 系统停止。");
        while(1) delay(1000);
    }

     Serial.println("\n[7] 初始化音量控制编码器...");
     initVolumeEncoder();

     Serial.println("\n[7.1] 初始化自听音控制按键 (IO12)...");
     pinMode(ENCODER_BUTTON_PIN, INPUT_PULLUP); // 设置 IO12 为输入，启用上拉电阻
     // 注释掉中断处理，使用主循环轮询检测（避免双重触发）
     // attachInterrupt(digitalPinToInterrupt(ENCODER_BUTTON_PIN), handleEncoderButton, FALLING);
     Serial.printf("  自听音功能默认: %s\n", sidetoneEnabled ? "开启" : "关闭");

     Serial.println("\n[8] 初始化16檔位絕對值編碼開關...");
     initRotarySwitch();

     Serial.println("\n[9] 初始化电池监测...");
     initBatteryMonitor();



     scanMacAddress();

     // 所有组件初始化完成，显示MAC地址映射状态
     Serial.println("\n[10] 显示设备映射状态...");
     printDeviceMappings();

     // 播放开机词 - 确认系统初始化完成
     Serial.println("\n🔊 播放开机词，确认系统就绪...");
     delay(500);  // 短暂延迟确保网络稳定
     playVoicePrompt(VOICE_STARTUP);

     Serial.println("\n🎉 系统初始化完成! 开始对讲吧!");
     // 更新数据包大小说明
     Serial.printf("📦 音频包大小: %d 字节 (原始语音 %d 字节 -> iLBC-20ms %d 字节 + %d 字节包头)\n",
                  sizeof(AudioPacket), BLOCK_SIZE * 2, 38, sizeof(uint16_t));
     Serial.printf("✨ 优化: 删除deviceId字段(节省1字节), 使用MAC地址识别设备, 支持%d个设备\n", MAX_SUPPORTED_DEVICES);
     Serial.println("✨ 特性: iLBC-20ms编解码, 异步发送, VAD增强语音检测, VOX传输控制, 自适应降噪, 远距离优化(20.5dBm+11b), 单声道优先");
     Serial.printf("📡 WiFi 配置: 信道%d, 功率20.5dBm, 协议11b (远距离模式), ESP-NOW硬件CSMA/CA冲突避免\n", WIFI_CHANNEL);
     Serial.printf("🔊 混音模式: 简单平衡混音, 最多%d人同时说话, 增益120%%\n", MAX_SIMULTANEOUS_TALKERS);

     Serial.println("\n📝 控制命令:");
     Serial.println(" c - 清屏");
     Serial.println(" s - 显示统计信息");
     Serial.println(" m - 显示MAC地址");
     Serial.println(" r - 显示资源使用情况 (Heap/Stack)");
     Serial.println(" b - 显示电池状态");
     Serial.println(" w - 显示WiFi配置 (协议/功率/信道) ⭐");
     Serial.println(" g - 会话管理");

     // 讀取初始頻道
     initRotarySwitch();
     currentChannel = readRotarySwitch();
     if (currentChannel > 15) currentChannel = 0; // 確保有效範圍
     Serial.printf("📻 初始頻道: %d\n", currentChannel);

 }

 //================== 主循环 ==================
 void loop() {
     // 發送心跳包
     unsigned long currentTime = millis();

     // 启动包发送标记（将在发送任务中处理）
     static bool startupPacketsSent = false;
     if (!startupPacketsSent && currentTime > 1000) { // 启动1秒后标记
         Serial.println("准备发送启动检测包...");
         // 设置全局标记，让发送任务处理
         shouldSendStartupPackets = true;
         startupPacketsSent = true;
     }
     if (currentTime - lastHeartbeatTime > HEARTBEAT_INTERVAL_MS) {
         sendHeartbeat(0); // 發送普通心跳
         lastHeartbeatTime = currentTime;

         // 定期清理非活跃设备映射（每次心跳时执行）
         cleanupInactiveDeviceMappings();
     }

     // 檢測旋轉開關變化
     static uint8_t lastPosition = 255; // 初始為無效值，確保第一次讀取會顯示
     static unsigned long lastSwitchCheckTime = 0;
     static unsigned long lastValidChangeTime = 0; // 上次有效變化的時間

     // 每100ms檢查一次旋轉開關狀態 (增加檢測間隔，減少頻繁觸發)
     if (currentTime - lastSwitchCheckTime > 100) {
         lastSwitchCheckTime = currentTime;
         uint8_t position = readRotarySwitch();

         // 確保讀取到有效值 (非255)且與上次位置不同
         if(position != 255 && position != lastPosition) {
             // 如果位置與上次不同且去抖時間已過
             if (currentTime - lastRotaryChangeTime > ROTARY_DEBOUNCE_TIME) {
                 // 標記檢測到的變化
                 Serial.printf("📊 檢測到旋轉開關位置變化: %d -> %d\n", lastPosition, position);

                 // 檢查新位置是否合理 (與上一個位置相鄰或差異不大)
                 bool isReasonableChange = true;

                 // 第一次讀取時，直接接受任何值
                 if (lastPosition == 255) {
                     isReasonableChange = true;
                 }
                 // 相鄰位置變化 (或從15到0、從0到15的循環變化)
                 else if (abs(position - lastPosition) == 1 ||
                         (position == 0 && lastPosition == 15) ||
                         (position == 15 && lastPosition == 0)) {
                     isReasonableChange = true;
                 }
                 // 對於快速旋轉，允許較小範圍的跳變，但控制在更嚴格的範圍內
                 else if (abs(position - lastPosition) == 2) {
                     // 允許跳過一個位置，但需要確認已經過了足夠長的時間
                     if (currentTime - lastValidChangeTime > 300) { // 最少300ms間隔
                         isReasonableChange = true;
                     }
                 }

                 if (isReasonableChange) {
                     // 更新穩定位置和狀態變量
                     lastStablePosition = position;
                     lastPosition = position;
                     lastRotaryChangeTime = currentTime;
                     lastValidChangeTime = currentTime; // 記錄有效變化時間

                     // 切換頻道 - 直接映射旋轉開關位置到頻道
                     if (currentChannel != position) {
                         currentChannel = position;
                         updateChannelDisplay();
                     }

                     Serial.printf("📊 旋轉開關位置已變更: %d (0x%X)\n", position, position);
                 } else {
                     // 檢測到不合理的跳變，可能是抖動，忽略
                     Serial.printf("⚠️ 檢測到不合理的位置變化，忽略: %d -> %d\n", lastPosition, position);
                 }
             } else {
                 // 檢測到變化，但未超過去抖時間，不輸出，避免干擾日誌
             }
         }
     }

     // 启动时多发送几次设备通知，增加识别机会
     if (startupBroadcastCount > 0 && startupBroadcastCount < MAX_STARTUP_BROADCASTS) {
         if (currentTime - lastHeartbeatTime > 500) { // 每500ms发送一次
             sendHeartbeat(1); // 发送启动通知
             startupBroadcastCount++;
             lastHeartbeatTime = currentTime;
         }
     }

     // 清空最终输出缓冲区
     memset(outputBuffer, 0, sizeof(outputBuffer));

     // 手动检查按钮状态(补充中断机制)
     static unsigned long lastButtonCheckTime = 0;
     static bool lastButtonState = HIGH;
     static unsigned long lastButtonChangeTime = 0;

     // 每10ms检查一次按钮状态
     if (millis() - lastButtonCheckTime > 10) {
         bool buttonState = digitalRead(ENCODER_BUTTON_PIN);
         lastButtonCheckTime = millis();

         // 增强的编码器活动检测和屏蔽逻辑
         unsigned long timeSinceLastEncoder = millis() - lastEncoderTime;

         // 优化的屏蔽策略：
         // 1. 基础时间屏蔽：150ms内有编码器活动（降低从200ms）
         // 2. 活动强度屏蔽：如果编码器活动频繁，适度延长屏蔽时间
         int blockTime = 150;  // 基础屏蔽时间（降低以提高按键灵敏度）
         if (encoderActivityCount > 4) {  // 提高阈值从3到4
             blockTime = 250;  // 频繁活动时延长（降低从300ms）
         }

         if (encoderActive && timeSinceLastEncoder < blockTime) {
             // 编码器正在活动，屏蔽按键检测以防误触发
             return;
         }

         // 清除编码器活动标志和计数器（如果超过屏蔽时间没有活动）
         if (timeSinceLastEncoder >= blockTime) {
             encoderActive = false;
             encoderActivityCount = 0;  // 重置活动计数器
         }

         // 如果按钮状态发生变化且防抖时间已过（降低防抖时间提高响应性）
         if (buttonState != lastButtonState && (millis() - lastButtonChangeTime > 50)) {
             lastButtonState = buttonState;
             lastButtonChangeTime = millis();

             // 检测到按钮被按下
             if (buttonState == LOW) {
                 sidetoneEnabled = !sidetoneEnabled;
                 sidetoneStatusChanged = true;

                 // 关闭自听音时，确保清除自听音准备标志
                 if (!sidetoneEnabled) {
                     sidetoneReady = false;
                 }
             }
         }
     }

     // 处理本地音频（可能添加自听音到sidetoneBuffer）
     processAudio();

     // 处理远程音频（独立处理到remoteBuffer）
     processReceivedAudio();

     // 检查自听音状态变化
     if (sidetoneStatusChanged) {
         Serial.printf("🔊 自听音功能已%s\n", sidetoneEnabled ? "开启" : "关闭");

         // 播放相应的语音提示
         if (sidetoneEnabled) {
             playVoicePrompt(VOICE_SIDETONE_ON);
         } else {
             playVoicePrompt(VOICE_SIDETONE_OFF);
         }

         sidetoneStatusChanged = false;

         // 确保状态变化后的稳定性
         if (!sidetoneEnabled) {
             // 如果自听音关闭，确保清空输出缓冲区中的自听音数据
             memset(sidetoneBuffer, 0, sizeof(sidetoneBuffer));
             sidetoneReady = false;
         }
     }

     // 处理音量调节提示音
     if ((volumeUpPrompt || volumeDownPrompt) &&
         (millis() - lastVolumePromptTime > VOLUME_PROMPT_INTERVAL)) {

         if (volumeUpPrompt) {
             Serial.println("🔊 播放音量增加提示音");
             playVoicePrompt(VOICE_VOLUME_UP);
             volumeUpPrompt = false;
         } else if (volumeDownPrompt) {
             Serial.println("🔊 播放音量减少提示音");
             playVoicePrompt(VOICE_VOLUME_DOWN);
             volumeDownPrompt = false;
         }

         lastVolumePromptTime = millis();
     }

     // 检查音量变化
     checkVolumeChange();

     // 最终混合处理
     // 1. 首先添加自听音（如果就绪且自听音功能开启）
     if (sidetoneReady && sidetoneEnabled) {
         memcpy(outputBuffer, sidetoneBuffer, sizeof(sidetoneBuffer));
         sidetoneReady = false; // 重置标志
     }

     // 2. 混合远程音频（如果就绪）
     if (remoteReady) {
         for (int j = 0; j < BLOCK_SIZE; j++) {
             // 混合远程音频到可能已包含自听音的输出缓冲区
             int32_t mixed = outputBuffer[j] + remoteBuffer[j];
             outputBuffer[j] = clamp16(mixed);
         }
         remoteReady = false; // 重置标志
     }

     // 3. 应用音量并播放最终混合的音频
     playSpeaker(outputBuffer, BLOCK_SIZE);

     // 更新电池状态
     updateBatteryStatus();


     if (Serial.available()) {
         char cmd = Serial.read();
         switch (cmd) {
             case 'c': {
                 for(int i=0; i<20; i++) Serial.println();
                 Serial.println("--- 屏幕已清除 ---");
                 break;
             }

             case 's': {
                 Serial.println("\n📊 统计信息:");
                 Serial.printf(" - 设备索引: %d\n", getMyDeviceIndex());
                 Serial.printf(" - 运行时间: %lu 秒\n", millis() / 1000);

                 Serial.printf(" - 发送队列: %u/%u | TX Seq: %u\n",
                              txQueue ? (TX_QUEUE_SIZE - uxQueueSpacesAvailable(txQueue)) : 0,
                              TX_QUEUE_SIZE, txSequence);
                 Serial.printf(" - iLBC编码器状态: 20ms模式 (160样本->38字节)\n"); // iLBC encoder status
                 Serial.printf(" - 已发送 (异步任务): %u 包\n", sentPackets);
                 Serial.printf(" - 已接收: %u 包\n", receivedPackets);
                 Serial.printf(" - 信号质量 (估算): %.1f%%\n", signalQuality);

                 Serial.printf(" - 当前音量: %d%% %s\n", volumeLevel, isMuted ? "[静音]" : "");
                 Serial.printf(" - 自听音状态: %s\n", sidetoneEnabled ? "开启" : "关闭");
                 Serial.printf(" - 旋轉開關位置: %d\n", readRotarySwitch());
                 Serial.printf(" - 电池电压: %.2fV %s\n", batteryVoltage,
                             batteryCritical ? "[超低]" :
                             batteryLow ? "[低]" :
                             batteryMedium ? "[中]" :
                             "[高]");

                 // 显示活跃音频源信息
                 int activeSources = 0;
                 Serial.println(" - 活跃音频源详情:");
                 for(int i=0; i<MAX_SUPPORTED_DEVICES; i++) {
                     if(audioSources[i].active && (millis() - audioSources[i].timestamp < SOURCE_TIMEOUT_MS)) {
                         activeSources++;
                         Serial.printf("   设备%d: 活跃\n", i);
                     }
                 }
                 Serial.printf(" - 当前活跃音频源: %d", activeSources);
                 if(activeSources >= 2) {
                     Serial.printf(" [混音已启用]");
                 }
                 Serial.println();

                 // 显示网络处理状态
                 if (networkAudioQueue) {
                     uint8_t queueUsed = NETWORK_QUEUE_SIZE - uxQueueSpacesAvailable(networkAudioQueue);
                     Serial.printf(" - 网络队列状态: %u/%u [彻底隔离模式]\n", queueUsed, NETWORK_QUEUE_SIZE);
                 } else {
                     Serial.println(" - 网络队列未初始化");
                 }
                 break;
             }

             case 'm': {
                 scanMacAddress();
                 break;
             }

             case 'r': {
                 Serial.println("\n💻 资源使用情况:");
                 Serial.printf(" - 堆内存剩余 (Heap Free): %u 字节\n", ESP.getFreeHeap());
                 if (sendTaskHandle) Serial.printf(" - 发送任务堆栈高水位: %u 字节\n", uxTaskGetStackHighWaterMark(sendTaskHandle));
                 if (networkTaskHandle) Serial.printf(" - 网络处理任务堆栈高水位: %u 字节\n", uxTaskGetStackHighWaterMark(networkTaskHandle));
                 Serial.printf(" - Loop任务堆栈高水位: %u 字节\n", uxTaskGetStackHighWaterMark(NULL));

                 // 显示队列内存使用
                 if (networkAudioQueue) {
                     uint8_t queueUsed = NETWORK_QUEUE_SIZE - uxQueueSpacesAvailable(networkAudioQueue);
                     Serial.printf(" - 网络隔离队列: %u/%u 已用 (约 %u 字节内存)\n",
                                  queueUsed, NETWORK_QUEUE_SIZE,
                                  sizeof(AudioBlock) * NETWORK_QUEUE_SIZE);
                 }
                 break;
             }



             case 'b': {
                 // 显示电池状态信息
                 Serial.println("\n🔋 电池状态信息");
                 Serial.printf("  电池电压: %.2fV\n", batteryVoltage);
                 Serial.printf("  电池状态: %s\n",
                             batteryHigh ? "高电量" :
                             batteryMedium ? "中等电量" :
                             batteryLow ? "低电量" : "超低电量");
                 Serial.printf("  电池电压阈值: 高>%.1fV, 中>%.1fV, 低>%.1fV, 超低<%.1fV\n",
                             BATTERY_HIGH_VOLTAGE, BATTERY_MEDIUM_VOLTAGE, BATTERY_LOW_VOLTAGE, BATTERY_CRITICAL_VOLTAGE);
                 break;
             }

             case 'w': {
                 // 显示WiFi配置信息
                 Serial.println("\n📡 WiFi配置信息");

                 // 获取当前协议（详细检查）
                 uint8_t protocol = 0;
                 esp_wifi_get_protocol(WIFI_IF_STA, &protocol);
                 Serial.println("  当前协议:");
                 Serial.printf("    - 原始值: 0x%02X\n", protocol);
                 Serial.printf("    - 11b: %s\n", (protocol & WIFI_PROTOCOL_11B) ? "✅" : "❌");
                 Serial.printf("    - 11g: %s\n", (protocol & WIFI_PROTOCOL_11G) ? "✅" : "❌");
                 Serial.printf("    - 11n: %s\n", (protocol & WIFI_PROTOCOL_11N) ? "✅" : "❌");
                 Serial.printf("    - LR:  %s", (protocol & WIFI_PROTOCOL_LR) ? "✅" : "❌");
                 if (protocol & WIFI_PROTOCOL_LR) {
                     Serial.println(" (Long Range模式已启用)");
                 } else {
                     Serial.println(" ⚠️ (LR模式未启用！)");
                 }

                 // 获取当前功率
                 int8_t power = 0;
                 esp_wifi_get_max_tx_power(&power);
                 Serial.printf("  发射功率: %.1fdBm (%.0fmW)\n", power * 0.25, pow(10, power * 0.25 / 10));

                 // 获取当前信道
                 uint8_t primary = 0;
                 wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
                 esp_wifi_get_channel(&primary, &second);
                 Serial.printf("  当前信道: %d\n", primary);

                 // WiFi模式
                 Serial.printf("  WiFi模式: %s\n", WiFi.getMode() == WIFI_STA ? "STA" : "其他");

                 // MAC地址
                 Serial.printf("  MAC地址: %s\n", WiFi.macAddress().c_str());

                 // 省电模式
                 wifi_ps_type_t ps_type;
                 esp_wifi_get_ps(&ps_type);
                 Serial.printf("  省电模式: %s\n", ps_type == WIFI_PS_NONE ? "禁用 (低延迟)" : "启用");

                 // 天线状态提示
                 Serial.println("\n  📡 天线检查:");
                 Serial.println("    ⚠️ 如果使用外置天线，请确认:");
                 Serial.println("    1. 模块型号是否为ESP32-S3-WROOM-1U");
                 Serial.println("    2. 是否需要GPIO切换天线");
                 Serial.println("    3. 外置天线是否正确连接");

                 Serial.println("\n  ⚠️ 如果LR模式未启用或距离不理想，请重启设备！");
                 break;
             }



             case 'g': {
                 // 会话管理菜单
                 Serial.println("\n🔊 会话管理:");
                 Serial.println(" 1 - 显示当前活跃设备");
                 Serial.println(" 2 - 显示能量阈值设置");
                 Serial.println(" 3 - 显示混音参数");
                 Serial.println(" 4 - 返回主菜单");

                 // 等待用户输入子命令
                 while (!Serial.available()) delay(10);
                 char subCmd = Serial.read();

                 switch (subCmd) {
                     case '1': {
                         // 显示当前活跃设备
                         Serial.println("\n📊 当前活跃设备:");
                         int activeCount = 0;
                         for (int i = 0; i < MAX_SUPPORTED_DEVICES; i++) {
                             if (audioSources[i].active &&
                                (millis() - audioSources[i].timestamp < SOURCE_TIMEOUT_MS)) {
                                 Serial.printf(" - 设备 %d: 活跃\n", i);
                                 activeCount++;
                             }
                         }
                         if (activeCount == 0) {
                             Serial.println(" - 当前没有活跃设备");
                         }
                         break;
                     }

                     case '2': {
                         // 显示VAD能量阈值设置
                         Serial.println("\n📊 VAD能量阈值设置:");
                         Serial.printf(" - VAD能量阈值: %d\n", VAD_ENERGY_THRESHOLD);
                         break;
                     }

                     case '3': {
                         // 显示混音参数
                         Serial.println("\n🔊 混音参数:");
                         Serial.printf(" - 最大同时说话人数: %d人\n", MAX_SIMULTANEOUS_TALKERS);
                         Serial.printf(" - 音频源超时时间: %dms\n", SOURCE_TIMEOUT_MS);
                         Serial.printf(" - 混音增益: 120%% (1.2x)\n");
                         Serial.println(" - 混音模式: 简单平衡混音（所有人音量相同）");
                         break;
                     }

                     case '4':
                     default:
                         Serial.println("返回主菜单");
                         break;
                 }
                 break;
             }



             default: {
                 if (cmd >= ' ' && cmd != '\n' && cmd != '\r') {
                     Serial.printf("❓ 未知命令: %c\n", cmd);
                     Serial.println("可用命令: c=清屏, s=統計, m=MAC, r=資源, b=電池狀態, g=会话管理");
                 }
                 break;
             }
         }
     }
     delay(5);
 }

 //================== WiFi, MAC, ESP-NOW, I2S 初始化  ==================
 bool initWiFi() {
     Serial.println("  初始化WiFi (STA模式, 信道13, 最大功率, 11b远距离模式)...");

     // 步骤1: 断开并清除之前的WiFi配置
     WiFi.disconnect(true, true);
     delay(100);

     // 步骤2: 设置WiFi模式为STA
     WiFi.mode(WIFI_STA);
     delay(100);

     // 步骤3: 等待WiFi完全启动（重要！）
     // WiFi.mode()是异步的，需要等待WiFi驱动完全初始化
     int retry = 0;
     while (WiFi.status() == WL_NO_SHIELD && retry < 10) {
         delay(100);
         retry++;
     }
     Serial.printf("  WiFi驱动启动完成 (耗时%dms)\n", retry * 100);

     // 步骤4: 设置WiFi协议为11b+LR模式（超远距离传输优化）
     // ⚠️ 必须在WiFi启动后设置才能生效
     // 11b协议：1-11Mbps，传输距离最远，穿墙能力强
     // LR模式：Espressif专利Long Range模式，接收灵敏度-102dBm，距离可达1km
     // 对于iLBC-20ms (15.2kbps) 完全够用
     esp_err_t protocol_err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_LR);
     if (protocol_err == ESP_OK) {
         Serial.println("  ✅ WiFi协议设置为 11b+LR (超远距离模式)");
     } else {
         Serial.printf("  ⚠️ WiFi协议设置失败，错误码: %d (0x%X)\n", protocol_err, protocol_err);
         Serial.println("  ⚠️ 将使用默认协议 (11b/g/n混合)");
     }

     // 步骤5: 使用ESP-IDF底层API设置最大发射功率
     // 单位：0.25dBm，82 = 20.5dBm (112mW)
     // 旧版Arduino ESP32不支持WIFI_POWER_20_5dBm枚举，直接用底层API
     int8_t power = 82;  // 20.5dBm
     esp_err_t power_err = esp_wifi_set_max_tx_power(power);
     if (power_err == ESP_OK) {
         Serial.printf("  ✅ 发射功率设置为 %.1fdBm (%.0fmW)\n", power * 0.25, pow(10, power * 0.25 / 10));
     } else {
         Serial.printf("  ⚠️ 发射功率设置失败，错误码: %d (0x%X)\n", power_err, power_err);
     }

     // 步骤6: 验证实际功率设置
     int8_t actual_power = 0;
     esp_wifi_get_max_tx_power(&actual_power);
     Serial.printf("  📡 实际发射功率: %.1fdBm\n", actual_power * 0.25);

     // 步骤7: 设置固定信道 (ESP-NOW对等设备配置中会使用信道13)
     esp_err_t channel_err = esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
     if (channel_err == ESP_OK) {
         Serial.printf("  ✅ WiFi信道设置为 %d\n", WIFI_CHANNEL);
     } else {
         Serial.printf("  ⚠️ WiFi信道设置失败，错误码: %d\n", channel_err);
     }

     // 步骤8: 设置ESP-NOW速率为1Mbps（最远距离模式）
     // ⚠️ 必须在WiFi协议设置后、ESP-NOW初始化前设置
     // WIFI_PHY_RATE_1M_L：11b协议的1Mbps长前导码，传输距离最远
     // 对于iLBC-20ms (15.2kbps) 完全够用，1Mbps = 1000kbps >> 15.2kbps
     esp_err_t rate_err = esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);
     if (rate_err == ESP_OK) {
         Serial.println("  ✅ ESP-NOW速率设置为 1Mbps (最远距离模式)");
     } else {
         Serial.printf("  ⚠️ ESP-NOW速率设置失败，错误码: %d (0x%X)\n", rate_err, rate_err);
         Serial.println("  ⚠️ 将使用默认速率 (可能是2Mbps或5.5Mbps)");
     }

     // 步骤9: 低延迟优化
     esp_wifi_set_connectionless_wake_interval(1);
     esp_wifi_set_ps(WIFI_PS_NONE);
     Serial.println("  ✅ 低延迟优化已启用");

     Serial.println("✅ WiFi 初始化完成");
     Serial.println("  配置摘要:");
     Serial.printf("    - 协议: 11b (远距离模式)\n");
     Serial.printf("    - ESP-NOW速率: 1Mbps (最远距离)\n");
     Serial.printf("    - 功率: %.1fdBm\n", actual_power * 0.25);
     Serial.printf("    - 信道: %d\n", WIFI_CHANNEL);
     Serial.printf("    - 省电: 禁用 (低延迟优化)\n");

     return true;
 }

 void scanMacAddress() {
     Serial.println("\n📱 MAC地址信息:");
     Serial.print("  本机MAC地址: "); Serial.println(WiFi.macAddress());
     uint8_t macBytes[6]; WiFi.macAddress(macBytes);
     Serial.print("  字节格式: ");
     for (int i = 0; i < 6; i++) { Serial.printf("%02X", macBytes[i]); if (i < 5) Serial.print(":"); }
     Serial.println("\n");
 }

 bool initESPNow() {
     Serial.println("  初始化ESP-NOW核心...");
     if (esp_now_init() != ESP_OK) { Serial.println("  ❌ ESP-NOW 初始化失败"); return false; }
     Serial.println("  注册发送和接收回调...");
     esp_now_register_send_cb(onDataSent);
     esp_now_register_recv_cb(onDataReceived);
     Serial.println("  添加广播对等设备...");
     esp_now_peer_info_t peerInfo = {};
     memcpy(peerInfo.peer_addr, broadcastAddress, 6);
     peerInfo.channel = WIFI_CHANNEL; // 明确使用WIFI_CHANNEL而不是当前通道
     peerInfo.encrypt = false;
     peerInfo.ifidx = WIFI_IF_STA;

     // 尝试移除之前可能存在的对等设备，确保干净的重新配对
     esp_now_del_peer(broadcastAddress);
     delay(10);

     // 现在添加广播对等设备
     if (esp_now_add_peer(&peerInfo) != ESP_OK) {
         Serial.println("  ⚠️ 添加广播对等设备失败! 重试...");
         esp_now_del_peer(broadcastAddress);
         delay(50);
         // 再次尝试添加
         if (esp_now_add_peer(&peerInfo) != ESP_OK) {
             Serial.println("  ⚠️ 添加广播对等设备再次失败!");
         }
     }

     Serial.println("✅ ESP-NOW 初始化完成");
     return true;
 }

 bool initI2S() {
     Serial.println("  卸载可能存在的旧I2S驱动...");
     i2s_driver_uninstall(I2S_NUM_0); // Mic
     i2s_driver_uninstall(I2S_NUM_1); // Speaker
     delay(100);
     Serial.println("  配置麦克风 PDM (I2S_NUM_0)...");
     i2s_config_t i2s_mic_config = {
         .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),  // 添加PDM模式
         .sample_rate = SAMPLE_RATE,
         .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,  // PDM模式使用16位
         .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
         .communication_format = I2S_COMM_FORMAT_STAND_I2S,
         .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
         .dma_buf_count = I2S_DMA_BUF_COUNT,
         .dma_buf_len = I2S_DMA_BUF_LEN, // Samples per buffer
         .use_apll = false,
         .tx_desc_auto_clear = false, // Not used for RX
         .fixed_mclk = 0
     };
     i2s_pin_config_t i2s_mic_pins = {
         .bck_io_num = I2S_PIN_NO_CHANGE, // PDM模式不需要BCLK
         .ws_io_num = I2S_MIC_CLK,        // PDM时钟线连接到IO16
         .data_out_num = I2S_PIN_NO_CHANGE,
         .data_in_num = I2S_MIC_DATA      // PDM数据线连接到IO18
     };
     if (i2s_driver_install(I2S_NUM_0, &i2s_mic_config, 0, NULL) != ESP_OK) {
         Serial.println("  ❌ PDM麦克风驱动安装失败");
         return false;
     }

     if (i2s_set_pin(I2S_NUM_0, &i2s_mic_pins) != ESP_OK) {
         Serial.println("  ❌ PDM麦克风引脚配置失败");
         return false;
     }

     // 设置PDM时钟
     if (i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO) != ESP_OK) {
         Serial.println("  ❌ PDM时钟设置失败");
         return false;
     }

     i2s_zero_dma_buffer(I2S_NUM_0);

     Serial.println("  配置扬声器 I2S (I2S_NUM_1)...");
     i2s_config_t i2s_spk_config = {
         .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
         .sample_rate = SAMPLE_RATE,
         .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // Output 16-bit samples
         .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
         .communication_format = I2S_COMM_FORMAT_STAND_I2S,
         .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
         .dma_buf_count = I2S_DMA_BUF_COUNT,
         .dma_buf_len = I2S_DMA_BUF_LEN, // Samples per buffer
         .use_apll = false,
         .tx_desc_auto_clear = true, // Auto clear for TX
         .fixed_mclk = 0
     };
     i2s_pin_config_t i2s_spk_pins = {
         .bck_io_num = I2S_SPK_BCLK,
         .ws_io_num = I2S_SPK_LRCK,
         .data_out_num = I2S_SPK_DATA,
         .data_in_num = I2S_PIN_NO_CHANGE
     };
      if (i2s_driver_install(I2S_NUM_1, &i2s_spk_config, 0, NULL) != ESP_OK) { Serial.println("  ❌ 扬声器I2S驱动安装失败"); return false; }
     if (i2s_set_pin(I2S_NUM_1, &i2s_spk_pins) != ESP_OK) { Serial.println("  ❌ 扬声器I2S引脚配置失败"); return false; }
     i2s_zero_dma_buffer(I2S_NUM_1);
     Serial.println("✅ I2S 初始化完成");
     return true;
 }



 void initSendTask() {
     Serial.println("  创建发送队列...");
     txQueue = xQueueCreate(TX_QUEUE_SIZE, sizeof(AudioPacket)); // Use new packet size
     if (txQueue == NULL) { Serial.println("  ❌ 发送队列创建失败!"); return; }

     Serial.println("  创建异步发送任务...");
     BaseType_t taskCreated = xTaskCreatePinnedToCore(
         sendTask,               // Task function
         "SendTask",             // Name of the task
         SEND_TASK_STACK_SIZE,   // Stack size in words
         NULL,                   // Task input parameter
         SEND_TASK_PRIORITY,     // Priority of the task
         &sendTaskHandle,        // Task handle
         SEND_TASK_CORE          // Core where the task should run
     );

     if (taskCreated != pdPASS || sendTaskHandle == NULL) {
         Serial.println("  ❌ 发送任务创建失败!");
         vQueueDelete(txQueue); // Clean up queue
         txQueue = NULL;
     } else {
         Serial.printf("✅ 异步发送任务已在核心 %d 上启动\n", SEND_TASK_CORE);
     }
 }

 // 初始化EC11旋转编码器用于音量控制
 void initVolumeEncoder() {
     // 设置引脚为输入模式并启用内部上拉电阻
     pinMode(ENCODER_PIN_A, INPUT_PULLUP);
     pinMode(ENCODER_PIN_B, INPUT_PULLUP);

     // 等待一小段时间确保引脚状态稳定
     delay(50);

     // 读取初始状态
     lastEncoded = (digitalRead(ENCODER_PIN_A) << 1) | digitalRead(ENCODER_PIN_B);

     // 验证引脚读取状态
     Serial.printf("  初始编码器状态: A(IO%d)=%d, B(IO%d)=%d\n",
                   ENCODER_PIN_A, digitalRead(ENCODER_PIN_A),
                   ENCODER_PIN_B, digitalRead(ENCODER_PIN_B));

     // 添加中断处理 - 修改为CHANGE增加灵敏度
     attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_A), updateEncoder, CHANGE);
     attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_B), updateEncoder, CHANGE);

     Serial.printf("✅ 音量控制已初始化，当前音量: %d%%\n", volumeLevel);
     Serial.println("  旋转方向：顺时针增加音量，逆时针减小音量");
 }

 // EC11编码器中断处理函数 - 放在IRAM中以确保快速响应
 void IRAM_ATTR updateEncoder() {
     // 添加简单的时间去抖动 (忽略5ms内的重复中断)
     unsigned long currentTime = millis();
     if(currentTime - lastEncoderTime < 5) {
         return;
     }

     // 读取当前引脚状态
     int a = digitalRead(ENCODER_PIN_A);
     int b = digitalRead(ENCODER_PIN_B);

     // 简化的编码器逻辑 - 基于A/B相的直接比较
     static int lastA = 1, lastB = 1;

     // 检测A相的变化来判断旋转方向
     if (a != lastA) {
         if (a == b) {
             // 顺时针旋转 - 增加音量
             int oldVolume = volumeLevel;
             volumeLevel += VOLUME_STEP;
             if(volumeLevel > 100) volumeLevel = 100;
             if(volumeLevel > 0) isMuted = false;
             // 只有在音量实际发生变化时才触发提示音
             if(volumeLevel != oldVolume) {
                 volumeUpPrompt = true;
             }
         } else {
             // 逆时针旋转 - 减小音量
             int oldVolume = volumeLevel;
             volumeLevel -= VOLUME_STEP;
             if(volumeLevel < 0) volumeLevel = 0;
             if(volumeLevel == 0) isMuted = true;
             // 只有在音量实际发生变化时才触发提示音
             if(volumeLevel != oldVolume) {
                 volumeDownPrompt = true;
             }
         }

         // 更新状态和时间戳
         lastEncoderTime = currentTime;
         encoderActive = true;
         encoderActivityCount++;  // 增加活动计数
     }

     // 更新上次状态
     lastA = a;
     lastB = b;
 }

 // EC11 按键中断服务程序 (已禁用，避免与轮询检测双重触发)
 // 注意：此函数已被禁用，按键检测现在完全由主循环轮询处理
 void IRAM_ATTR handleEncoderButton() {
     // 函数已禁用，立即返回
     return;

     // 以下代码已禁用 - 防抖逻辑，过滤EC11旋转时的误触发（100ms以下）
     static unsigned long lastDebounceTime = 0;
     unsigned long currentTime = millis();

     // 增强的智能屏蔽：根据编码器活动强度动态调整屏蔽时间
     unsigned long timeSinceLastEncoder = currentTime - lastEncoderTime;
     int blockTime = 200;  // 基础屏蔽时间
     if (encoderActivityCount > 3) {
         blockTime = 300;  // 频繁活动时延长屏蔽
     }

     if (encoderActive && timeSinceLastEncoder < blockTime) {
         return; // 编码器正在活动，忽略按键中断以防误触发
     }

     // 80ms防抖，过滤旋转时的误触发
     if (currentTime - lastDebounceTime < 80) {
         return; // 如果距离上次触发不到80ms，忽略此次中断
     }

     // 由于使用FALLING中断，这里不需要检查按钮状态
     // 直接切换自听音状态
     sidetoneEnabled = !sidetoneEnabled; // 翻转自听音使能标志
     sidetoneStatusChanged = true; // 设置状态变化标志，供主循环检查

     // 关闭自听音时，确保清除自听音准备标志
     if (!sidetoneEnabled) {
         sidetoneReady = false;
     }

     lastDebounceTime = currentTime; // 更新去抖时间戳
 }

 // 检查并显示音量变化
 void checkVolumeChange() {
     static int lastReportedVolume = -1;
     static bool lastMuteState = false;

     // 只在音量或静音状态发生变化时更新
     if(volumeLevel != lastReportedVolume || isMuted != lastMuteState) {
         Serial.printf("🔊 音量: %d%% %s\n", volumeLevel, isMuted ? "[静音]" : "");
         lastReportedVolume = volumeLevel;
         lastMuteState = isMuted;
     }
 }

 // 切换静音状态
 void toggleMute() {
     isMuted = !isMuted;

     // 如果解除静音但音量为0，设置音量为最小非零值
     if(!isMuted && volumeLevel == 0) {
         volumeLevel = VOLUME_STEP;
     }

     Serial.printf("🔊 %s: 音量 %d%%\n", isMuted ? "已静音" : "已取消静音", volumeLevel);
 }

 //================== ESP-NOW 回调函数 ==================
 void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
     // This callback confirms the transmission attempt, not necessarily successful reception by peer.
     if (status != ESP_NOW_SEND_SUCCESS) {
         #ifdef DEBUG_MODE
         static unsigned long lastSendErrorTime = 0;
         // Avoid spamming the serial port with send failures
         if (millis() - lastSendErrorTime > 1000) {
              Serial.printf(" DBUG: Send CB Status Fail for %02X:%02X:%02X:%02X:%02X:%02X, Status: %d\n",
                 mac_addr[0],mac_addr[1],mac_addr[2],mac_addr[3],mac_addr[4],mac_addr[5], status);
              lastSendErrorTime = millis();
         }
         #endif
         // Potential place to implement retry logic if needed, but NACK handles peer reception failure.
     }
 }

 void onDataReceived(const uint8_t *mac_addr, const uint8_t *data, int len) {
     // 获取本机MAC地址
     uint8_t my_mac[6];
     WiFi.macAddress(my_mac);

     // 跳过自己发送的包（基于MAC地址比较，100%准确）
     if (macEquals(mac_addr, my_mac)) {
         return; // 忽略自己的包
     }

     // 处理心跳包
     if (len == sizeof(HeartbeatPacket)) {
         HeartbeatPacket* heartbeat = (HeartbeatPacket*)data;

         // 不再需要ID冲突检测！直接更新设备映射
         uint8_t deviceIndex = getDeviceIndexByMac(mac_addr);

         #ifdef DEBUG_MODE
         if (heartbeat->type == 1) {
             char mac_str[18];
             macToString(mac_addr, mac_str);
             Serial.printf("� 设备 %s (索引 %d) 启动通知，通道: %d\n",
                          mac_str, deviceIndex, heartbeat->channel);
         }
         #endif

         // 重置该设备的序列号跟踪
         if (heartbeat->type == 1) {
             for (int i = 0; i < activeMappingCount; i++) {
                 if (deviceMappings[i].array_index == deviceIndex) {
                     deviceMappings[i].expected_seq = 0;
                     Serial.printf("📢 设备索引 %d 重新启动通知，已重置序列号跟踪\n", deviceIndex);
                     break;
                 }
             }
         }

         return;
     }

     // 忽略其他类型的包
     if (len != sizeof(AudioPacket) && len != sizeof(HeartbeatPacket)) {
         #ifdef DEBUG_MODE
         Serial.println(" DBUG: 收到未知类型的包，已忽略");
         #endif
         return;
     }

     // 音频包处理
     if (len == sizeof(AudioPacket)) {
         AudioPacket* packet = (AudioPacket*)data;

         // 使用MAC地址获取设备索引（100%无冲突）
         uint8_t deviceIndex = getDeviceIndexByMac(mac_addr);

         // 检查频道是否匹配
         uint8_t packetChannel = getChannelFromPacket(packet);
         if (packetChannel != currentChannel) {
             // 频道不匹配，忽略此数据包
             #ifdef DEBUG_MODE
             Serial.printf(" DBUG: 忽略不匹配频道的数据包（接收频道: %d, 当前频道: %d）\n",
                          packetChannel, currentChannel);
             #endif
             return;
         }

         // 获取实际序列号（不含频道信息）
         uint16_t receivedSeq = getSequenceFromPacket(packet);

         // 序列号检查（直接通过索引访问映射表）
         // deviceIndex对应deviceMappings中array_index相同的条目
         DeviceMapping* mapping = nullptr;

         // 优化：直接通过索引查找映射
         for (int i = 0; i < activeMappingCount; i++) {
             if (deviceMappings[i].array_index == deviceIndex) {
                 mapping = &deviceMappings[i];
                 break;
             }
         }

         if (mapping) {
             uint16_t expected = mapping->expected_seq;

             if (expected == 0) {
                 // 第一个包，直接接受
                 mapping->expected_seq = receivedSeq + 1;
             } else if (receivedSeq >= expected) {
                 // 正常或跳跃的包
                 if (receivedSeq > expected) {
                     #ifdef DEBUG_MODE
                     Serial.printf(" DBUG: 检测到丢包：设备索引%d，期望%u，收到%u，丢失%u个包\n",
                                  deviceIndex, expected, receivedSeq, receivedSeq - expected);
                     #endif
                 }
                 mapping->expected_seq = receivedSeq + 1;
             } else if ((expected > SEQUENCE_WRAP_THRESHOLD) && (receivedSeq < (UINT16_MAX - SEQUENCE_WRAP_THRESHOLD))) {
                 // 序列号回绕
                 mapping->expected_seq = receivedSeq + 1;
             }
             // 旧包或重复包，静默忽略
         } else {
             #ifdef DEBUG_MODE
             Serial.printf(" DBUG: 警告：找不到设备索引%d的映射\n", deviceIndex);
             #endif
         }

         // 传递给网络任务处理
         if (networkAudioQueue) {
             NetworkData networkData;
             networkData.type = NETWORK_DATA_RECEIVE;
             networkData.data.receivePacket = *packet;
             networkData.deviceIndex = deviceIndex;  // 传递从MAC地址映射的设备索引

             if (xQueueSend(networkAudioQueue, &networkData, 0) != pdTRUE) {
                 // 队列满，丢弃包
             }
         }

         receivedPackets++;
         updateSignalQuality(); // Update signal quality estimate based on NACK rate
         return;
     }

     #ifdef DEBUG_MODE
     // Optional: Log packets with unexpected sizes
     // Serial.printf(" DBUG: Received packet with unexpected size %d from %02X:%02X:%02X:%02X:%02X:%02X\n",
     //               len, mac_addr[0],mac_addr[1],mac_addr[2],mac_addr[3],mac_addr[4],mac_addr[5]);
     #endif
 }

 //================== 音频处理函数 ==================
 bool readMic(int16_t* buffer, size_t samples) {
     size_t bytes_read = 0;
     // PDM模式直接输出16位数据
     int16_t temp_buffer[samples];
     // 从I2S接口读取数据
     esp_err_t result = i2s_read(I2S_NUM_0, temp_buffer, samples * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(30)); // Wait max 30ms

     if (result != ESP_OK || bytes_read == 0) {
         #ifdef DEBUG_MODE
         Serial.printf(" DBUG: PDM Read Error: %s, bytes read: %d\n", esp_err_to_name(result), bytes_read);
         #endif
         return false; // Error or no data read
     }



     // 复制数据到输出缓冲区
     memcpy(buffer, temp_buffer, samples * sizeof(int16_t));

     // Apply gain (noise reduction now handled in processAudio after VAD)
     applyGain(buffer, samples, AUDIO_GAIN);

     return true;
 }

 void applyGain(int16_t* buffer, size_t samples, float gain) {
     if (gain == 1.0f) return; // Skip if gain is 1
     for(size_t i = 0; i < samples; ++i) {
         int32_t amplified = (int32_t)(buffer[i] * gain);
         buffer[i] = clamp16(amplified); // Clamp to 16-bit range
     }
 }

 bool playSpeaker(int16_t* buffer, size_t samples) {
     // 应用音量控制 (只有在非100%音量时才需要处理)
     if(volumeLevel < 100) {
         float volumeFactor = volumeLevel / 100.0f;
         for(size_t i = 0; i < samples; i++) {
             buffer[i] = (int16_t)(buffer[i] * volumeFactor);
         }
     }

     size_t bytes_written = 0;
     // 将处理过的音频数据写入I2S接口
     esp_err_t result = i2s_write(I2S_NUM_1, buffer, samples * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(30)); // Wait max 30ms

     if (result != ESP_OK || bytes_written != samples * sizeof(int16_t)) {
          #ifdef DEBUG_MODE
          Serial.printf(" DBUG: I2S Write Error: %s, bytes written: %d/%d\n", esp_err_to_name(result), bytes_written, samples * sizeof(int16_t));
          #endif
         return false; // Error or not all data written
     }
     return true;
 }

 // Function to queue an audio packet for sending via the dedicated task
 bool sendAudioPacket(AudioPacket* packet) {
     if (txQueue == NULL) return false; // Queue not initialized

     // 設置頻道信息到序列號高4位
     setChannelToPacket(packet, currentChannel);

     // Try to send the packet to the queue, wait max 10ms if full
     if (xQueueSend(txQueue, packet, pdMS_TO_TICKS(10)) != pdTRUE) {
         static uint32_t lastQueueFullWarn = 0;
         // Print warning only once per second to avoid spamming
         if (millis() - lastQueueFullWarn > 1000) {
             Serial.println("⚠️ 发送队列已满!");
             lastQueueFullWarn = millis();
         }
         return false; // Queue is full
     }
     return true; // Packet successfully queued
 }

 // Main audio processing loop: Read mic -> VAD检测 -> VOX传输控制 -> Encode -> Send
 void processAudio() {
     // 如果处于静音状态，跳过麦克风处理
     if(isMuted) {
         return;
     }

     // Attempt to read a block of audio from the microphone
     if (readMic(micBuffer, BLOCK_SIZE)) {

         // --- VAD增强检测 ---
         bool voiceDetected = vadEnhanced.processBlock(micBuffer, BLOCK_SIZE);

         // --- 保存原始信号用于自听音 ---
         int16_t originalMicBuffer[BLOCK_SIZE];
         memcpy(originalMicBuffer, micBuffer, BLOCK_SIZE * sizeof(int16_t));

         // --- 连续噪声抑制处理 ---
         // 对micBuffer进行降噪处理，用于网络传输
         continuousNoiseSuppressor.processVoiceWithNoiseSuppression(micBuffer, BLOCK_SIZE, voiceDetected);

         unsigned long currentTime = millis();

         // --- 传输控制逻辑 (基于VAD检测结果 + VOX时序控制) ---
         if (voiceDetected) {
             // VAD检测到语音，启动或继续VOX传输
             if (!voxActive) {
                 voxActive = true;
                 voxStartTime = currentTime; // Mark start time for min duration check
             }
             lastVoiceTime = currentTime; // Update time of last voice activity


             // 路径2: 网络发送 - 完全独立的处理路径
             if (networkTaskRunning && networkAudioQueue) {
                 // 创建音频块
                 AudioBlock newBlock;
                 memcpy(newBlock.samples, micBuffer, BLOCK_SIZE * sizeof(int16_t));
                 newBlock.voxActive = true;
                 newBlock.timestamp = currentTime;

                 // 非阻塞发送到队列，如果队列满则丢弃
                 NetworkData networkData;
                 networkData.type = NETWORK_DATA_SEND;
                 networkData.data.sendBlock = newBlock;
                 xQueueSend(networkAudioQueue, &networkData, 0);
             }

             // === 自听音独立处理逻辑 ===
             // 只有当sidetoneEnabled为true时才处理自听音
             if (sidetoneEnabled && !isMuted) {
                 // 清空自听音缓冲区
                 memset(sidetoneBuffer, 0, sizeof(sidetoneBuffer));

                 // 添加自听音到独立缓冲区（使用原始信号，不受降噪影响）
                 for (int j = 0; j < BLOCK_SIZE; j++) {
                     int32_t sidetoneSample = (int32_t)(originalMicBuffer[j] * SIDETONE_VOLUME);
                     sidetoneBuffer[j] = clamp16(sidetoneSample);
                 }

                 // 标记自听音就绪
                 sidetoneReady = true;
             } else {
                 // 如果自听音被禁用，确保不会使用旧的自听音缓冲区
                 sidetoneReady = false;
             }

         } else if (voxActive) {
             // VAD未检测到语音，但VOX仍处于活跃状态 (尾部时间控制)
             bool tooShort = (currentTime - voxStartTime < VOX_MIN_DURATION_MS); // Check minimum transmission duration
             bool inTail = (currentTime - lastVoiceTime < VOX_TAIL_MS); // Check if within tail-off period

             if (tooShort || inTail) {
                 // VOX尾部控制：保持传输直到满足最小持续时间或尾部时间
                 // 连续噪声抑制器已处理降噪，VOX只负责传输时序
                 voxEndingPhase++; // 仅更新阶段计数


                 // 路径2: 网络发送 - 完全独立的处理路径
                 if (networkTaskRunning && networkAudioQueue) {
                     // 创建音频块
                     AudioBlock newBlock;
                     memcpy(newBlock.samples, micBuffer, BLOCK_SIZE * sizeof(int16_t));
                     newBlock.voxActive = true;  // 仍在VOX活动期内
                     newBlock.timestamp = currentTime;

                     // 非阻塞发送到队列，如果队列满则丢弃
                     NetworkData networkData;
                     networkData.type = NETWORK_DATA_SEND;
                     networkData.data.sendBlock = newBlock;
                     xQueueSend(networkAudioQueue, &networkData, 0);
                 }

                 // === 拖尾期间也添加自听音 ===
                 // 同样，只有当sidetoneEnabled为true时才处理
                 if (sidetoneEnabled && !isMuted) {
                     // 清空自听音缓冲区
                     memset(sidetoneBuffer, 0, sizeof(sidetoneBuffer));

                     // 添加自听音到独立缓冲区（使用原始信号，不受降噪影响）
                     for (int j = 0; j < BLOCK_SIZE; j++) {
                         int32_t sidetoneSample = (int32_t)(originalMicBuffer[j] * SIDETONE_VOLUME);
                         sidetoneBuffer[j] = clamp16(sidetoneSample);
                     }

                     // 标记自听音就绪
                     sidetoneReady = true;
                 } else {
                     // 如果自听音被禁用，确保不会使用旧的自听音缓冲区
                     sidetoneReady = false;
                 }
             } else {
                 // End of transmission (min duration and tail met)
                 // 开始发送渐进式静音包
                 fadeOutPacketsRemaining = FADE_OUT_PACKETS;

                 #ifdef DEBUG_MODE
                 Serial.printf("VOX结束，开始发送%d个渐进式静音包\n", FADE_OUT_PACKETS);
                 #endif

                 // 不立即停止VOX，而是进入静音包发送阶段
                 voxActive = false;
                 voxEndingPhase = 0;
                 sidetoneReady = false; // 确保结束VOX时也清除自听音准备标志
             }

             previousVoxActive = true;
         } else if (fadeOutPacketsRemaining > 0) {
             // VOX已结束，但仍需发送渐进式静音包

             // 创建渐进式静音包 - 音量逐渐降低
             float fadeOutFactor = (float)fadeOutPacketsRemaining / FADE_OUT_PACKETS;

             // 创建一个静音缓冲区，但保留一些原始信号以实现平滑过渡
             int16_t fadeOutBuffer[BLOCK_SIZE];
             memset(fadeOutBuffer, 0, BLOCK_SIZE * sizeof(int16_t));

             // 如果有上一个音频块，使用它作为基础，但大幅降低音量
             if (previousVoxActive) {
                 // 复制上一个音频块，但应用渐进式音量衰减
                 for (int i = 0; i < BLOCK_SIZE; i++) {
                     fadeOutBuffer[i] = (int16_t)(micBuffer[i] * fadeOutFactor * 0.5f);
                 }
             }

             // 发送渐进式静音包
             if (networkTaskRunning && networkAudioQueue) {
                 // 创建音频块
                 AudioBlock newBlock;
                 memcpy(newBlock.samples, fadeOutBuffer, BLOCK_SIZE * sizeof(int16_t));
                 newBlock.voxActive = true;  // 标记为活跃，确保接收端处理
                 newBlock.timestamp = millis();

                 // 非阻塞发送到队列
                 NetworkData networkData;
                 networkData.type = NETWORK_DATA_SEND;
                 networkData.data.sendBlock = newBlock;
                 xQueueSend(networkAudioQueue, &networkData, 0);

                 #ifdef DEBUG_MODE
                 Serial.printf("发送渐进式静音包 %d/%d，音量因子: %.2f\n",
                              FADE_OUT_PACKETS - fadeOutPacketsRemaining + 1,
                              FADE_OUT_PACKETS, fadeOutFactor);
                 #endif
             }

             // 递减剩余包计数
             fadeOutPacketsRemaining--;

             // 保存当前缓冲区供下一个静音包使用
             memcpy(micBuffer, fadeOutBuffer, BLOCK_SIZE * sizeof(int16_t));

         } else {
             // VAD未检测到语音且VOX不活跃，无需传输
             if (previousVoxActive) {
                 // VOX刚刚结束，应用最后的噪声清理
                 memset(micBuffer, 0, BLOCK_SIZE * sizeof(int16_t));
                 previousVoxActive = false;
             }

             voxEndingPhase = 0;

             // 静默状态，无需传输处理
         }
     }
 }



 //================== 异步发送任务  ==================
 // FreeRTOS task dedicated to sending packets from the queue
 void sendTask(void *pvParameters) {
     AudioPacket packetToSend; // Structure to hold the packet received from the queue
     Serial.println("  Async Send Task Started.");

     while(1) {
         // 检查是否需要发送启动包
         if (shouldSendStartupPackets) {
             Serial.println("发送启动检测包...");
             sendStartupDetectionPackets();
             shouldSendStartupPackets = false;
         }

         // 缩短等待时间，避免长时间阻塞 (从无限等待改为最多等待5ms)
         if(xQueueReceive(txQueue, &packetToSend, pdMS_TO_TICKS(5)) == pdTRUE) {
             // Attempt to send the packet via ESP-NOW broadcast
             esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&packetToSend, sizeof(AudioPacket));

             if(result == ESP_OK) {
                 sentPackets++; // Increment counter for successfully initiated sends
             }
             #ifdef DEBUG_MODE
             else {
                 // Log send errors (potentially rate-limited)
                 static uint32_t failCount = 0;
                 failCount++;
                 if (failCount % 10 == 0) { // Log every 10th failure
                     Serial.printf(" DBUG: SendTask ESP-NOW Send Error: %s\n", esp_err_to_name(result));
                 }
             }
             #endif
             // Brief yield to allow other tasks (optional, but good practice)
             // vTaskDelay(pdMS_TO_TICKS(1));
         }
     }
 }

 //================== 信号质量监控  ==================
 // Estimate signal quality based on the ratio of NACK requests to received packets
 void updateSignalQuality() {
     // 已禁用的NACK统计，保持100%信号质量
     signalQuality = 100.0f;

 }

 // 更新设备的音频数据
 void updateAudioSource(uint8_t deviceId, int16_t* buffer) {
     if(deviceId >= MAX_SUPPORTED_DEVICES) return;

     // 复制音频数据到对应设备的缓冲区
     memcpy(audioSources[deviceId].buffer, buffer, BLOCK_SIZE * sizeof(int16_t));
     audioSources[deviceId].timestamp = millis();
     audioSources[deviceId].active = true;
 }

 // 处理接收到的音频，实现简单平衡混音
 void processReceivedAudio() {
     uint32_t currentTime = millis();

     // 1. 收集活跃音频源（最多8人）
     int activeSources[MAX_SIMULTANEOUS_TALKERS];
     int activeCount = 0;

     for(int i = 0; i < MAX_SUPPORTED_DEVICES && activeCount < MAX_SIMULTANEOUS_TALKERS; i++) {
         if(audioSources[i].active &&
            (currentTime - audioSources[i].timestamp < SOURCE_TIMEOUT_MS)) {
             activeSources[activeCount++] = i;
         }
     }

     if(activeCount == 0) {
         remoteReady = false;
         return;
     }

     // 2. 清空输出缓冲区
     memset(remoteBuffer, 0, sizeof(remoteBuffer));

     // 3. 简单平均混音 + 全局增益20%
     float mixVolume = 1.2f / activeCount;

     for(int i = 0; i < activeCount; i++) {
         int srcIdx = activeSources[i];
         for(int j = 0; j < BLOCK_SIZE; j++) {
             int32_t mixed = remoteBuffer[j] +
                            (int32_t)(audioSources[srcIdx].buffer[j] * mixVolume);
             remoteBuffer[j] = clamp16(mixed);
         }
     }

     remoteReady = true;
 }

 //================== 平衡网络处理任务 ==================
 // 初始化网络处理任务
 void initNetworkTask() {
     // 创建网络音频队列
     networkAudioQueue = xQueueCreate(NETWORK_QUEUE_SIZE, sizeof(NetworkData));
     if (networkAudioQueue == NULL) {
         Serial.println("❌ 网络音频队列创建失败!");
         return;
     }

     Serial.printf("✅ 网络音频队列已创建 (深度: %d)\n", NETWORK_QUEUE_SIZE);

     // 创建网络处理任务（在核心1上运行，优先级适中）
     BaseType_t taskCreated = xTaskCreatePinnedToCore(
         networkTask,             // 任务函数
         "NetworkTask",           // 任务名称
         49152,                   // 堆栈大小 (48KB) - 平衡稳定性和内存使用
         NULL,                    // 参数
         5,                       // 优先级提高到5（原为4）
         &networkTaskHandle,      // 任务句柄
         1                        // 在核心1上运行
     );

     if (taskCreated != pdPASS || networkTaskHandle == NULL) {
         Serial.println("❌ 网络处理任务创建失败!");
         return;
     }

     networkTaskRunning = true;  // 标记任务已启动
     Serial.printf("✅ 彻底隔离的网络处理任务已在核心 1 上启动（优先级：5）\n");
 }

 // 完全独立的网络处理任务
 void networkTask(void* parameter) {
     Serial.println("  彻底隔离的网络处理任务已启动");

     NetworkData networkData;
     static AudioPacket packet;  // 重用包结构

     // iLBC编码器无需本地状态管理

     while(true) {
         // 等待队列中的网络数据
         if(xQueueReceive(networkAudioQueue, &networkData, portMAX_DELAY) == pdTRUE) {

             if (networkData.type == NETWORK_DATA_SEND) {
                 // 处理发送音频块
                 AudioBlock& audioBlock = networkData.data.sendBlock;

                 // 准备发送包
                 packet.sequence = txSequence++;

                 // 使用iLBC编码音频
                 size_t encodedSize = encodeILBC(audioBlock.samples, packet.ilbc_data);
                 if (encodedSize != 38) {
                     continue; // 跳过这一帧，减少串口输出
                 }

                 // 发送音频包
                 sendAudioPacket(&packet);

             } else if (networkData.type == NETWORK_DATA_RECEIVE) {
                 // 处理接收到的音频包
                 AudioPacket& receivedPacket = networkData.data.receivePacket;
                 uint8_t deviceIndex = networkData.deviceIndex;  // 从MAC地址映射的设备索引

                 // 使用iLBC解码音频数据
                 int16_t decompressed[BLOCK_SIZE];
                 size_t decodedSamples = decodeILBC(receivedPacket.ilbc_data, decompressed);
                 if (decodedSamples == BLOCK_SIZE) {
                     // 更新该设备的音频源数据
                     updateAudioSource(deviceIndex, decompressed);

                     // 触发音频处理（播放或混音）
                     processReceivedAudio();

                     receivedPackets++;
                     updateSignalQuality();
                 }
                 // 解码失败时静默跳过，不输出错误信息
             }
         }
     }
 }

 bool sendHeartbeat(uint8_t type) {
     HeartbeatPacket packet;
     packet.deviceId = getMyDeviceIndex();  // 使用MAC地址映射获取索引
     packet.type = type;
     packet.channel = WIFI_CHANNEL;

     esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&packet, sizeof(HeartbeatPacket));
     if (result == ESP_OK) {
         return true;
     } else {
         Serial.printf(" DBUG: Failed to send heartbeat. Error: %s\n", esp_err_to_name(result));
         return false;
     }
 }

 //================== 啟動檢測函數 ==================
 void sendStartupDetectionPackets() {
     Serial.println("  发送启动检测包...");

     // 发送5个间隔100ms的音频包，帮助重建连接
     for (int i = 0; i < 5; i++) {
         AudioPacket packet;
         packet.sequence = i;  // 从0开始的序列号，表明这是新启动的设备

         // iLBC编码器无需状态初始化

         // 填充检测音频数据
         int16_t tempBuffer[BLOCK_SIZE];
         for (size_t j = 0; j < BLOCK_SIZE; j++) {
             tempBuffer[j] = 5000 * ((j % 2) ? 1 : -1); // 交替正负值产生检测音频
         }

         // 使用iLBC编码音频数据
         size_t encodedSize = encodeILBC(tempBuffer, packet.ilbc_data);
         if (encodedSize != 38) {
             Serial.printf("⚠️ iLBC编码失败，跳过此帧 (返回%d字节)\n", encodedSize);
             continue; // 跳过这一帧
         }

         // 直接发送，不经过队列
         esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&packet, sizeof(AudioPacket));
         if (result != ESP_OK) {
             Serial.printf("  ⚠️ 启动检测包 %d 发送失败: %s\n", i, esp_err_to_name(result));
         } else {
             Serial.printf("  ✓ 启动检测包 %d 已发送\n", i);
         }

         delay(100);
     }

     // 重置发送序列号计数器为一个较小的值
     txSequence = 10;
     Serial.println("  发送启动检测包完成，重置序列号为 10");
 }

 // 添加讀取旋轉開關的函數
 uint8_t readRotarySwitch() {
     // 連續讀取多次，確保數據穩定
     uint8_t readings[ROTARY_CONFIRM_READS];

     // 進行多次讀取
     for(int i = 0; i < ROTARY_CONFIRM_READS; i++) {
         readings[i] = 0;
         readings[i] |= (digitalRead(ROTARY_PIN_1) == LOW) ? 1 : 0;
         readings[i] |= (digitalRead(ROTARY_PIN_2) == LOW) ? 2 : 0;
         readings[i] |= (digitalRead(ROTARY_PIN_4) == LOW) ? 4 : 0;
         readings[i] |= (digitalRead(ROTARY_PIN_8) == LOW) ? 8 : 0;

         // 每次讀取之間等待較長時間
         if(i < ROTARY_CONFIRM_READS - 1) {
             delay(2); // 增加到2ms
         }
     }

     // 檢查多次讀取的一致性
     bool allSame = true;
     for(int i = 1; i < ROTARY_CONFIRM_READS; i++) {
         if(readings[i] != readings[0]) {
             allSame = false;
             break;
         }
     }

     // 如果多次讀取結果一致，將結果加入歷史緩衝區
     if(allSame) {
         uint8_t currentReading = readings[0];

         // 更新歷史緩衝區
         positionHistory[historyIndex] = currentReading;
         historyIndex = (historyIndex + 1) % ROTARY_HISTORY_SIZE;

         // 檢查歷史緩衝區中的值是否一致
         bool historyConsistent = true;
         for(int i = 0; i < ROTARY_HISTORY_SIZE; i++) {
             if(positionHistory[i] != currentReading && positionHistory[i] != 255) {
                 historyConsistent = false;
                 break;
             }
         }

         // 只有在歷史緩衝區一致或者當前讀取與上次穩定位置相同時才返回當前讀取值
         if(historyConsistent || currentReading == lastStablePosition) {
             return currentReading;
         }
     }

     // 讀取不一致或歷史緩衝區不一致，返回上次穩定的值
     return lastStablePosition;
 }

 // 初始化旋轉開關函數聲明
 void initRotarySwitch();

 // 添加初始化旋轉開關的函數實現
 void initRotarySwitch() {
     // 設置引腳為輸入模式並啟用內部上拉電阻
     pinMode(ROTARY_PIN_1, INPUT_PULLUP);
     pinMode(ROTARY_PIN_2, INPUT_PULLUP);
     pinMode(ROTARY_PIN_4, INPUT_PULLUP);
     pinMode(ROTARY_PIN_8, INPUT_PULLUP);

     // 等待一小段時間確保引腳狀態穩定
     delay(50);

     // 讀取並顯示當前旋轉開關位置
     uint8_t position = readRotarySwitch();
     Serial.printf("✅ 旋轉開關已初始化，當前位置: %d (0x%X)\n", position, position);
 }

 // 定義提示音函數 - 使用I2S播放一個簡短的蜂鳴聲
 void playRotarySwitchBeep() {
     static int16_t beepBuffer[SAMPLE_RATE/10]; // 100ms的緩衝區
     static bool beepInitialized = false;

     // 首次運行時初始化提示音緩衝區
     if (!beepInitialized) {
         // 生成正弦波提示音
         for (int i = 0; i < sizeof(beepBuffer)/sizeof(beepBuffer[0]); i++) {
             float angle = (2.0f * PI * SWITCH_BEEP_FREQ * i) / SAMPLE_RATE;
             beepBuffer[i] = (int16_t)(sinf(angle) * 32767.0f * SWITCH_BEEP_VOLUME);
         }
         beepInitialized = true;
     }

     // 計算要播放的樣本數量
     size_t sampleCount = (SWITCH_BEEP_DURATION * SAMPLE_RATE) / 1000;
     if (sampleCount > sizeof(beepBuffer)/sizeof(beepBuffer[0])) {
         sampleCount = sizeof(beepBuffer)/sizeof(beepBuffer[0]);
     }

     // 播放提示音
     size_t bytesWritten = 0;
     i2s_write(I2S_NUM_1, beepBuffer, sampleCount * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
 }

 // 向文件最後添加頻道處理函數定義

 //================== 頻道處理函數 ==================
 // 在packet中設置頻道信息
 void setChannelToPacket(AudioPacket* packet, uint8_t channel) {
     packet->sequence = (packet->sequence & SEQUENCE_MASK) | ((channel & 0x0F) << 12);
 }

 // 從packet中獲取頻道信息
 uint8_t getChannelFromPacket(const AudioPacket* packet) {
     return (packet->sequence & CHANNEL_MASK) >> 12;
 }

 // 從packet中獲取實際序列號(不含頻道信息)
 uint16_t getSequenceFromPacket(const AudioPacket* packet) {
     return packet->sequence & SEQUENCE_MASK;
 }

 // 更新頻道顯示
 void updateChannelDisplay() {
     Serial.printf("📻 當前頻道: %d\n", currentChannel);

     // 播放频道语音提示
     if (currentChannel >= 0 && currentChannel <= 15) {
         // 所有频道都使用语音提示
         playChannelVoice(currentChannel);
     } else {
         // 如果频道超出范围，播放普通提示音
         playRotarySwitchBeep();
     }
 }

 // 播放频道语音提示
 void playChannelVoice(uint8_t channel) {
     // 确保频道在有效范围内
     if (channel < 0 || channel > 15) {
         return;
     }

     // 选择对应的语音数据
     const int16_t* voiceData = nullptr;
     size_t voiceLength = 0;

     // 注意：频道0对应channel1_audio，频道1对应channel2_audio，以此类推
     switch (channel) {
         case 0:
             voiceData = channel1_audio;
             voiceLength = sizeof(channel1_audio) / sizeof(channel1_audio[0]);
             break;
         case 1:
             voiceData = channel2_audio;
             voiceLength = sizeof(channel2_audio) / sizeof(channel2_audio[0]);
             break;
         case 2:
             voiceData = channel3_audio;
             voiceLength = sizeof(channel3_audio) / sizeof(channel3_audio[0]);
             break;
         case 3:
             voiceData = channel4_audio;
             voiceLength = sizeof(channel4_audio) / sizeof(channel4_audio[0]);
             break;
         case 4:
             voiceData = channel5_audio;
             voiceLength = sizeof(channel5_audio) / sizeof(channel5_audio[0]);
             break;
         case 5:
             voiceData = channel6_audio;
             voiceLength = sizeof(channel6_audio) / sizeof(channel6_audio[0]);
             break;
         case 6:
             voiceData = channel7_audio;
             voiceLength = sizeof(channel7_audio) / sizeof(channel7_audio[0]);
             break;
         case 7:
             voiceData = channel8_audio;
             voiceLength = sizeof(channel8_audio) / sizeof(channel8_audio[0]);
             break;
         case 8:
             voiceData = channel9_audio;
             voiceLength = sizeof(channel9_audio) / sizeof(channel9_audio[0]);
             break;
         case 9:
             voiceData = channel10_audio;
             voiceLength = sizeof(channel10_audio) / sizeof(channel10_audio[0]);
             break;
         case 10:
             voiceData = channel11_audio;
             voiceLength = sizeof(channel11_audio) / sizeof(channel11_audio[0]);
             break;
         case 11:
             voiceData = channel12_audio;
             voiceLength = sizeof(channel12_audio) / sizeof(channel12_audio[0]);
             break;
         case 12:
             voiceData = channel13_audio;
             voiceLength = sizeof(channel13_audio) / sizeof(channel13_audio[0]);
             break;
         case 13:
             voiceData = channel14_audio;
             voiceLength = sizeof(channel14_audio) / sizeof(channel14_audio[0]);
             break;
         case 14:
             voiceData = channel15_audio;
             voiceLength = sizeof(channel15_audio) / sizeof(channel15_audio[0]);
             break;
         case 15:
             voiceData = channel16_audio;
             voiceLength = sizeof(channel16_audio) / sizeof(channel16_audio[0]);
             break;
         default:
             // 如果没有对应频道的语音数据，播放普通提示音
             playRotarySwitchBeep();
             return;
     }

     // 播放语音数据
     if (voiceData && voiceLength > 0) {
         // 创建临时缓冲区并调整音量
         int16_t* tempBuffer = (int16_t*)malloc(voiceLength * sizeof(int16_t));
         if (tempBuffer) {
             // 复制数据到临时缓冲区并调整音量
             for (size_t i = 0; i < voiceLength; i++) {
                 // 将音量降低到原来的10%
                 tempBuffer[i] = (int16_t)(voiceData[i] * 0.1f);
             }

             // 播放调整后的音频
             size_t bytesWritten = 0;
             i2s_write(I2S_NUM_1, tempBuffer, voiceLength * sizeof(int16_t), &bytesWritten, portMAX_DELAY);

             // 释放临时缓冲区
             free(tempBuffer);
         } else {
             // 如果内存分配失败，直接播放原始音频
             Serial.println("内存分配失败，使用原始音量播放");
             size_t bytesWritten = 0;
             i2s_write(I2S_NUM_1, voiceData, voiceLength * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
         }
     }
 }

 //================== 电池电量监测配置 ==================

 // 电池检测周期（毫秒）
 #define BATTERY_CHECK_INTERVAL 120000  // 每2分钟检测一次

 // 电池监测相关变量
 unsigned long lastBatteryCheck = 0;    // 上次电池检测时间
 unsigned long lastBatteryBeep = 0;     // 上次电池提示音时间

 // LED和电池监测
 void initLEDs();           // LED初始化函数
 void initBatteryMonitor(); // 电池监测初始化
 float readBatteryVoltage();
 void updateBatteryStatus();
 void playBatteryLowBeep();

 //================== LED和电池监测功能实现 ==================
 // 初始化LED指示灯 - 在系统启动时立即调用
 void initLEDs() {
     Serial.printf("  配置LED引脚: 绿色=IO%d, 蓝色=IO%d, 红色=IO%d\n",
                 BATTERY_GREEN_LED_PIN, BATTERY_BLUE_LED_PIN, BATTERY_RED_LED_PIN);

     // 将LED引脚设置为输出模式
     pinMode(BATTERY_GREEN_LED_PIN, OUTPUT);
     pinMode(BATTERY_BLUE_LED_PIN, OUTPUT);
     pinMode(BATTERY_RED_LED_PIN, OUTPUT);

     // 默认关闭所有LED
     digitalWrite(BATTERY_GREEN_LED_PIN, LOW);
     digitalWrite(BATTERY_BLUE_LED_PIN, LOW);
     digitalWrite(BATTERY_RED_LED_PIN, LOW);
     Serial.println("  所有LED初始化为关闭状态");

     // 初始化LED指示 - 依次闪烁一次，确认设备正常启动
     Serial.println("  初始化绿色LED (IO45)...");
     digitalWrite(BATTERY_GREEN_LED_PIN, HIGH);
     delay(200);
     digitalWrite(BATTERY_GREEN_LED_PIN, LOW);

     Serial.println("  初始化蓝色LED (IO47)...");
     digitalWrite(BATTERY_BLUE_LED_PIN, HIGH);
     delay(200);
     digitalWrite(BATTERY_BLUE_LED_PIN, LOW);

     Serial.println("  初始化红色LED (IO48)...");
     digitalWrite(BATTERY_RED_LED_PIN, HIGH);
     delay(200);
     digitalWrite(BATTERY_RED_LED_PIN, LOW);

     Serial.println("  LED指示灯初始化完成");
 }

 // 初始化电池监测 - 使用IO9的ADC读取
 void initBatteryMonitor() {
     Serial.printf("  配置电池检测引脚: IO%d\n", BATTERY_ADC_PIN);

     // 先重置引脚状态
     gpio_reset_pin((gpio_num_t)BATTERY_ADC_PIN);
     delay(10); // 等待引脚重置

     // 设置为输入模式
     gpio_set_direction((gpio_num_t)BATTERY_ADC_PIN, GPIO_MODE_INPUT);

     // 禁用上拉和下拉电阻
     gpio_pullup_dis((gpio_num_t)BATTERY_ADC_PIN);
     gpio_pulldown_dis((gpio_num_t)BATTERY_ADC_PIN);

     // 配置ADC
     pinMode(BATTERY_ADC_PIN, INPUT);
     adcAttachPin(BATTERY_ADC_PIN);
     analogReadResolution(12);
     analogSetAttenuation(ADC_11db);

     // 空读取几次，稳定ADC
     for (int i = 0; i < 3; i++) {
         analogRead(BATTERY_ADC_PIN);
         delay(10);
     }

     // 读取数字电平
     int digitalValue = gpio_get_level((gpio_num_t)BATTERY_ADC_PIN);
     Serial.printf("  数字读取IO%d电平: %d\n", BATTERY_ADC_PIN, digitalValue);

     // 读取ADC值
     int rawValue = analogRead(BATTERY_ADC_PIN);
     Serial.printf("  ADC读取IO%d值: %d\n", BATTERY_ADC_PIN, rawValue);

     // 初始化时间戳变量，确保不会立即触发检测
     lastBatteryCheck = millis();
     lastBatteryBeep = millis();

     // 首次读取电池电压
     batteryVoltage = readBatteryVoltage();
     Serial.printf("  电池电压: %.2fV\n", batteryVoltage);

     // 根据电池电压设置初始状态
     batteryHigh = (batteryVoltage >= BATTERY_HIGH_VOLTAGE);
     batteryMedium = (batteryVoltage < BATTERY_HIGH_VOLTAGE && batteryVoltage >= BATTERY_MEDIUM_VOLTAGE);
     batteryLow = (!batteryHigh && !batteryMedium && !batteryCritical);
     batteryCritical = (batteryVoltage <= BATTERY_CRITICAL_VOLTAGE);

     // 输出电池状态
     Serial.printf("  电池状态: %s\n",
                 batteryHigh ? "高电量" :
                 batteryMedium ? "中等电量" :
                 batteryLow ? "低电量" : "超低电量");

     // 设置初始 LED 状态
     if (batteryHigh) {
         // 高电量 - 绿色LED亮
         digitalWrite(BATTERY_GREEN_LED_PIN, HIGH);
         digitalWrite(BATTERY_BLUE_LED_PIN, LOW);
         digitalWrite(BATTERY_RED_LED_PIN, LOW);
         Serial.println("  LED状态: 绿色LED亮");
     } else if (batteryMedium) {
         // 中等电量 - 蓝色LED亮
         digitalWrite(BATTERY_GREEN_LED_PIN, LOW);
         digitalWrite(BATTERY_BLUE_LED_PIN, HIGH);
         digitalWrite(BATTERY_RED_LED_PIN, LOW);
         Serial.println("  LED状态: 蓝色LED亮");
     } else {
         // 低电量 - 红色LED亮
         digitalWrite(BATTERY_GREEN_LED_PIN, LOW);
         digitalWrite(BATTERY_BLUE_LED_PIN, LOW);
         digitalWrite(BATTERY_RED_LED_PIN, HIGH);
         Serial.println("  LED状态: 红色LED亮");
     }

     Serial.printf("✅ 电池监测初始化完成，当前电压: %.2fV\n", batteryVoltage);
 }

 // 读取电池电压 - 使用ADC读取
 float readBatteryVoltage() {
     // 重置引脚配置，确保正确设置
     pinMode(BATTERY_ADC_PIN, INPUT);
     adcAttachPin(BATTERY_ADC_PIN);
     analogReadResolution(12);
     analogSetAttenuation(ADC_11db);

     // 使用多次采样并取平均值，提高稳定性
     const int samples = 8; // 采样次数
     int adcSum = 0;
     int validSamples = 0;

     // 首先执行一次空读取，丢弃第一次读数
     analogRead(BATTERY_ADC_PIN);
     delay(10); // 短暂延迟，稳定ADC

     // 多次采样
     for (int i = 0; i < samples; i++) {
         int rawValue = analogRead(BATTERY_ADC_PIN);
         if (rawValue > 0) { // 只计算非零读数
             adcSum += rawValue;
             validSamples++;
         }
         delay(5); // 小延迟，提高稳定性
     }

     // 检查是否有有效样本
     if (validSamples == 0) {
         // 如果ADC读取失败，使用数字电平判断
         int digitalValue = gpio_get_level((gpio_num_t)BATTERY_ADC_PIN);
         if (digitalValue == 1) {
             return 4.05f; // 如果数字电平为1，返回高电量值
         } else {
             return 3.45f; // 如果数字电平为0，返回低电量值
         }
     }

     // 计算平均值
     float adcAverage = (float)adcSum / validSamples;

     // 计算ADC输入电压
     float adcVoltage = (adcAverage / BATTERY_ADC_RESOLUTION) * BATTERY_ADC_REFERENCE;

     // 计算实际电池电压
     float actualVoltage = adcVoltage * BATTERY_DIVIDER_RATIO;

     // 应用校准系数
     float calibratedVoltage = actualVoltage * BATTERY_CALIBRATION_FACTOR;

     return calibratedVoltage;
 }

 // 更新电池状态和LED显示 - 简化版本
 void updateBatteryStatus() {
     unsigned long currentTime = millis();
     static float smoothedVoltage = 0.0f; // 平滑处理后的电压值

     // 定期检测电池电量，每2分钟检测一次
     if (currentTime - lastBatteryCheck >= 120000) { // 2分钟检测一次，平衡检测频率和功耗
         // 读取电池电压
         float newVoltage = readBatteryVoltage();
         lastBatteryCheck = currentTime;

         // 平滑处理电压值，减少波动
         if (smoothedVoltage == 0.0f) {
             // 首次读取，直接使用新值
             smoothedVoltage = newVoltage;
         } else {
             // 使用指数移动平均，平滑电压值
             smoothedVoltage = smoothedVoltage * 0.7f + newVoltage * 0.3f;
         }

         // 保存之前的状态
         bool prevHigh = batteryHigh;
         bool prevMedium = batteryMedium;
         bool prevLow = batteryLow;
         bool prevCritical = batteryCritical;

         // 更新电池状态标志，使用更精确的判断逻辑

         // 超低电量状态判断 - 先判断超低电量
         batteryCritical = (smoothedVoltage < BATTERY_CRITICAL_VOLTAGE);

         // 高电量状态判断 - 应用滑动窗口
         if (batteryHigh) {
             // 如果当前是高电量，只有电压低于(BATTERY_HIGH_VOLTAGE-BATTERY_HYSTERESIS)才会切换
             batteryHigh = (smoothedVoltage >= (BATTERY_HIGH_VOLTAGE - BATTERY_HYSTERESIS));
         } else {
             // 如果当前不是高电量，需要电压高于BATTERY_HIGH_VOLTAGE才会切换
             batteryHigh = (smoothedVoltage >= BATTERY_HIGH_VOLTAGE);
         }

         // 中等电量状态判断 - 应用滑动窗口
         if (batteryMedium) {
             // 如果当前是中等电量，需要电压低于(BATTERY_MEDIUM_VOLTAGE-BATTERY_HYSTERESIS)或高于(BATTERY_HIGH_VOLTAGE)才会切换
             batteryMedium = (!batteryHigh && !batteryCritical &&
                           smoothedVoltage >= (BATTERY_MEDIUM_VOLTAGE - BATTERY_HYSTERESIS));
         } else {
             // 如果当前不是中等电量，需要电压在区间内才会切换
             batteryMedium = (!batteryHigh && !batteryCritical &&
                           smoothedVoltage >= BATTERY_MEDIUM_VOLTAGE);
         }

         // 低电量状态判断 - 应用滑动窗口
         if (batteryLow) {
             // 如果当前是低电量，需要电压低于(BATTERY_LOW_VOLTAGE-BATTERY_HYSTERESIS)或高于(BATTERY_MEDIUM_VOLTAGE)才会切换
             batteryLow = (!batteryHigh && !batteryMedium && !batteryCritical &&
                        smoothedVoltage >= (BATTERY_LOW_VOLTAGE - BATTERY_HYSTERESIS));
         } else {
             // 如果当前不是低电量，需要电压在区间内才会切换
             batteryLow = (!batteryHigh && !batteryMedium && !batteryCritical &&
                        smoothedVoltage >= BATTERY_LOW_VOLTAGE);
         }

         // 仅在电池状态变化时才更新LED，减少IO操作
         if (prevHigh != batteryHigh || prevMedium != batteryMedium || prevLow != batteryLow) {
             // 状态变化，更新LED
             if (batteryHigh) {
                 // 高电量 - 绿色LED亮
                 digitalWrite(BATTERY_GREEN_LED_PIN, HIGH);
                 digitalWrite(BATTERY_BLUE_LED_PIN, LOW);
                 digitalWrite(BATTERY_RED_LED_PIN, LOW);
             } else if (batteryMedium) {
                 // 中等电量 - 蓝色LED亮
                 digitalWrite(BATTERY_GREEN_LED_PIN, LOW);
                 digitalWrite(BATTERY_BLUE_LED_PIN, HIGH);
                 digitalWrite(BATTERY_RED_LED_PIN, LOW);
             } else {
                 // 低电量 - 红色LED亮
                 digitalWrite(BATTERY_GREEN_LED_PIN, LOW);
                 digitalWrite(BATTERY_BLUE_LED_PIN, LOW);
                 digitalWrite(BATTERY_RED_LED_PIN, HIGH);
             }
         }

         // 如果进入超低电量状态，播放提示音
         if (!prevCritical && batteryCritical) {
             playBatteryLowBeep();
             lastBatteryBeep = currentTime; // 更新上次提示时间
         }

         // 更新显示电压值为平滑后的值
         batteryVoltage = smoothedVoltage;
     }

     // 在超低电量状态下，每3分钟提示一次
     if (batteryCritical && (currentTime - lastBatteryBeep >= 180000)) {  // 3分钟提示一次
         playBatteryLowBeep();
         lastBatteryBeep = currentTime;
     }
 }

 // 播放电池低电量提示音
 void playBatteryLowBeep() {
     // 使用语音提示"电量低"
     static bool initialized = false;

     // 播放低电量语音提示
     size_t bytesWritten = 0;

     // 创建临时缓冲区用于调整音量
     const int16_t* lowBatteryAudio = low_battery_audio;
     size_t lowBatteryAudioSize = low_battery_audio_size;

     // 创建临时缓冲区并调整音量
     int16_t* tempBuffer = (int16_t*)malloc(lowBatteryAudioSize * sizeof(int16_t));
     if (tempBuffer) {
         // 复制数据到临时缓冲区
         for (size_t i = 0; i < lowBatteryAudioSize; i++) {
             // 将音量降低到10% - 统一语音提示音量
             tempBuffer[i] = (int16_t)(lowBatteryAudio[i] * 0.1f);
         }

         // 播放调整后的音频
         i2s_write(I2S_NUM_1, tempBuffer, lowBatteryAudioSize * sizeof(int16_t), &bytesWritten, portMAX_DELAY);

         // 释放临时缓冲区
         free(tempBuffer);
     } else {
         // 如果内存分配失败，直接播放原始音频
         Serial.println("内存分配失败，使用原始音量播放");
         i2s_write(I2S_NUM_1, lowBatteryAudio, lowBatteryAudioSize * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
     }
 }

