/**
 * ESP32-S3 无线音频传输系统 - 主程序 (main.cpp)
 * 
 * 【文件说明】
 * 本文件是ESP32-S3无线对讲系统的主入口，负责系统初始化、音频处理及模式控制。
 * 实现了基于ESP-NOW的多用户对讲系统，支持多达8个频道切换和多人会话功能。
 * 
 * 【主要功能】
 * 1. 音频采集与播放 - 通过I2S接口采集麦克风数据并播放扬声器音频
 * 2. 抖动缓冲区管理 - 优化音频流处理，减少"刺啦刺啦"噪音
 * 3. 模式控制系统 - 实现正常模式和设置模式的切换
 * 4. 频道管理 - 支持8个频道切换和多人会话
 * 5. 用户交互界面 - 通过串口命令控制系统
 * 
 * 【重要函数位置】
 * - setup()               [行114] - 系统初始化入口
 * - loop()                [行171] - 主循环
 * - initSystem()          [行194] - 初始化系统组件(PSRAM、缓冲区、无线等)
 * - initI2S()             [行231] - 初始化I2S接口配置
 * - initJitterBuffer()    [行354] - 初始化音频抖动缓冲区
 * - jitterBufferAdd()     [行401] - 向抖动缓冲区添加音频数据
 * - jitterBufferRead()    [行500] - 从抖动缓冲区读取音频数据
 * - detectVoiceActivity() [行597] - 语音活动检测(VAD)
 * - processAudioIO()      [行639] - 处理音频输入并发送
 * - playRemoteAudio()     [行679] - 播放远程接收的音频数据
 * - applyAudioProcessing()[行735] - 音频处理(降噪、音量控制、压缩)
 * - runNormalMode()       [行841] - 正常工作模式处理
 * - runSettingsMode()     [行1080] - 设置模式处理
 * - displayStatus()       [行1274] - 显示系统状态
 * 
 * 【实现原理】
 * 1. 音频流处理链:
 *    麦克风(I2S) → 预处理 → VAD检测 → A-law压缩 → ESP-NOW发送 →
 *    → 接收 → 解压 → 抖动缓冲 → 平滑混音 → 输出(I2S)
 * 
 * 2. 抖动缓冲区:
 *    使用序列号跟踪音频包，可检测丢包和乱序，并动态调整播放速率，
 *    实现平滑播放，减少音频断续和噪音。
 * 
 * 3. 频道切换机制:
 *    基于ESP-NOW自组织网络实现，每个频道独立工作，设备可动态加入离开。
 *    通过wireless_join_channel()函数切换频道(1-8)。
 *    支持通过键盘(1-8键)或外部硬件旋钮选择频道。
 * 
 * 4. 多用户会话处理:
 *    - 使用peerDevices数组跟踪频道内设备
 *    - 每个设备有独立的音频缓冲区和音量控制
 *    - mixAudioFromPeers函数混合多路音频
 * 
 * 本系统采用双模操作模式(正常/设置)，可通过串口命令或外部控制切换。
 * 所有音频采用16位PCM采样，通过G.711 A-law压缩传输，降低带宽需求。
 */

/**
 * ESP32-S3 无线音频传输系统 - 主程序
 * 
 * 本程序实现了基于ESP32-S3的无线对讲系统，使用ESP-NOW进行点对点音频传输
 * 包含高级抖动缓冲区和音频优化，解决"刺啦刺啦"噪音和回声问题
 * 
 * 功能特点:
 * - 数字I2S麦克风和扬声器接口
 * - ESP-NOW低延迟无线传输
 * - VAD语音活动检测
 * - 改进的抖动缓冲区，支持序列号处理
 * - 音频帧平滑处理，减少噪音
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <WiFi.h>

// 项目头文件
#include "config.h"
#include "wireless_system.h"
#include "g711.h"

// 声明无线系统中定义的函数和变量
extern void mixAudioFromPeers(int16_t* outputBuffer, int sampleCount);
extern PeerWithAudio peerDevices[MAX_PEERS];
extern bool setPeerVolume(uint8_t deviceId, float volume);
extern bool setPeerMuted(uint8_t deviceId, bool muted);

//============================ 全局变量 ============================
// 音频控制参数
float audioVolume = 1.0;          // 音频音量(0.1-3.0)
bool espnowTransmitEnabled = true; // ESP-NOW发送功能开关
bool isInitialized = false;        // 系统是否已初始化
bool showDebugInfo = false;        // 显示调试信息

// 工作模式
enum WorkMode {
  MODE_NORMAL,         // 正常工作模式
  MODE_SETTINGS        // 设置模式
};
WorkMode currentMode = MODE_NORMAL;  // 当前工作模式

// 音频缓冲区 - 使用PSRAM分配
int32_t* micBuffer = NULL;        // 麦克风原始数据（32位）
int16_t* processedBuffer = NULL;  // 处理后的音频数据（16位）
int16_t* playBuffer = NULL;       // 播放缓冲区
int16_t* playBuffer1 = NULL;       // 播放缓冲区

// 语音活动检测 (VAD) 参数
#define VAD_THRESHOLD 600         // 语音检测阈值，根据实际环境调整
#define VAD_HOLD_TIME 300         // 语音结束后保持发送的时间(ms)
float backgroundNoise = 2000;     // 初始环境噪声估计值
float currentEnergy = 0;          // 当前帧能量
bool vadActive = false;           // 语音活动状态
unsigned long lastVoiceTime = 0;  // 最后检测到语音的时间

// 调整抖动缓冲区参数 - 增大缓冲区并调整播放阈值
#define JITTER_BUFFER_SIZE 2400      // 增加到150ms
#define JITTER_MIN_THRESHOLD 800     // 增加最小阈值到50ms
#define JITTER_TARGET_LEVEL 1600     // 增加目标水平到100ms
#define AUDIO_CHUNK_SIZE 160         // 保持不变


// 缓冲区中的音频块结构
typedef struct {
    int16_t samples[AUDIO_CHUNK_SIZE];  // 音频样本
    uint16_t sequenceNum;              // 序列号
    bool valid;                        // 数据是否有效
} AudioChunk;

// 抖动缓冲区
typedef struct {
    AudioChunk* chunks;               // 音频块数组
    int capacity;                     // 缓冲区容量 (块数)
    int size;                         // 当前存储的块数
    uint16_t nextExpectedSeq;         // 下一个期望的序列号
    uint16_t oldestSeq;               // 最老的序列号
    uint16_t newestSeq;               // 最新的序列号
    bool initialized;                 // 是否已初始化
    bool underflow;                   // 缓冲区是否曾经下溢
    float playbackRate;               // 播放速率调整因子
} JitterBuffer;

JitterBuffer jbuffer;                 // 全局抖动缓冲区

// 统计信息
unsigned long lastStatsTime = 0;      // 上次显示统计的时间
unsigned long lastAudioSendTime = 0;  // 上次发送音频的时间
unsigned long sampleCount = 0;        // 采集样本计数
unsigned long sendCount = 0;          // 发送计数
uint16_t audioSeqNum = 0;            // 发送数据包序列号

//============================ 函数声明 ============================
bool initSystem();
bool initI2S();
void processAudioIO();
void playRemoteAudio();
bool detectVoiceActivity(int16_t* samples, int sampleCount);
void applyAudioProcessing(int32_t* input, int16_t* output, size_t samples);
void displayStatus();
void runNormalMode();
void runSettingsMode();
void printSystemInfo();
void testMicrophone();

// 抖动缓冲区函数
void initJitterBuffer();
bool jitterBufferAdd(int16_t* samples, int numSamples, uint16_t sequenceNum);
int jitterBufferRead(int16_t* outputBuffer, int maxSamples);

//============================ 初始化函数 ============================
void setup() {
  // 初始化串口
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n===== ESP32-S3 无线音频传输系统 V2.0 =====");
  
  // 打印系统信息
  printSystemInfo();
  
  // 初始化系统
  if (initSystem()) {
    isInitialized = true;
    Serial.println("\n系统初始化完成，进入正常工作模式");
    Serial.printf("设备ID: %d, 固件版本: %s\n", DEVICE_ID, FW_VERSION);
    Serial.println("本机MAC: " + wireless_get_my_mac());
    Serial.println("目标MAC: " + wireless_get_peer_mac());
    Serial.println("WiFi通道: " + String(wireless_get_channel()));
    Serial.println("ESP-NOW发送: " + String(espnowTransmitEnabled ? "开启" : "关闭"));
    
    // 初始化频道系统
    if (wireless_setup_channels()) {
      // 如果已加入频道，显示频道信息
      uint8_t channelId = wireless_get_current_channel_id();
      if (channelId > 0) {
        Serial.printf("当前频道: %d\n", channelId);
        
        // 获取频道内的对等设备
        PeerInfo peers[MAX_PEERS];
        int peerCount = wireless_get_peers_in_channel(peers, MAX_PEERS);
        
        if (peerCount > 0) {
          Serial.printf("频道内设备数: %d\n", peerCount);
        } else {
          Serial.println("频道内暂无其他设备");
        }
      } else {
        Serial.println("当前未加入任何频道");
      }
    } else {
      Serial.println("频道系统初始化失败");
    }
    
    Serial.println("\n控制命令:");
    Serial.println("s - 进入设置模式");
    Serial.println("+ - 增加音量");
    Serial.println("- - 降低音量");
    Serial.println("e - 切换ESP-NOW发送");
    Serial.println("d - 切换调试信息");
    Serial.println("p - 显示系统状态");
    Serial.println("t - 运行ESP-NOW诊断");
    Serial.println("c - 扫描最佳通道");
  } else {
    Serial.println("系统初始化失败！");
  }
}

void loop() {
  // 如果初始化失败，不执行任何操作
  if (!isInitialized) {
    delay(1000);
    return;
  }
  
  // 让无线系统处理接收到的数据
  wireless_process();
  
  // 根据当前模式执行不同功能
  if (currentMode == MODE_SETTINGS) {
    runSettingsMode();
  } else {
    runNormalMode();
  }
}

//============================ 系统初始化 ============================
/**
 * 初始化系统组件
 * - 检查PSRAM
 * - 分配音频缓冲区
 * - 初始化无线系统
 * - 初始化I2S接口
 * - 初始化抖动缓冲区
 * 
 * @return 初始化是否成功
 */
bool initSystem() {
  // 检查PSRAM状态
  Serial.println("检查PSRAM状态...");
  if (esp_spiram_is_initialized()) {
    size_t psramSize = ESP.getPsramSize();
    Serial.printf("PSRAM已初始化，大小: %d 字节\n", psramSize);
  } else {
    Serial.println("警告：PSRAM未初始化！");
    return false;
  }
  
  // 清晰区分不同用途的缓冲区，使用calloc初始化为0
  micBuffer = (int32_t*)heap_caps_calloc(BLOCK_SIZE, sizeof(int32_t), MALLOC_CAP_SPIRAM);
  processedBuffer = (int16_t*)heap_caps_calloc(BLOCK_SIZE, sizeof(int16_t), MALLOC_CAP_SPIRAM);
  playBuffer = (int16_t*)heap_caps_calloc(BLOCK_SIZE, sizeof(int16_t), MALLOC_CAP_SPIRAM);
  playBuffer1 = (int16_t*)heap_caps_calloc(BLOCK_SIZE, sizeof(int16_t), MALLOC_CAP_SPIRAM);  

  if (!micBuffer || !processedBuffer || !playBuffer) {
    Serial.println("错误：无法分配音频缓冲区");
    return false;
  }
  
  // 初始化各个子系统
  if (!wireless_init(DEVICE_ID)) {
    Serial.println("无线系统初始化失败");
    return false;
  }
  
  if (!initI2S()) {
    Serial.println("I2S初始化失败");
    return false;
  }
  
  // 初始化抖动缓冲区
  initJitterBuffer();
  
  return true;
}

//============================ I2S初始化 ============================
/**
 * 初始化I2S接口，配置麦克风和扬声器
 * - 卸载可能存在的驱动
 * - 配置麦克风I2S（INMP441使用32位格式）
 * - 配置扬声器I2S（MAX98357A使用16位格式）
 * 
 * @return 初始化是否成功
 */
bool initI2S() {
  Serial.println("初始化I2S接口...");
  
  // 尝试卸载可能存在的驱动
  esp_err_t err = i2s_driver_uninstall(I2S_MIC_PORT);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("麦克风驱动卸载错误: %d\n", err);
  }
  
  err = i2s_driver_uninstall(I2S_SPK_PORT);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("扬声器驱动卸载错误: %d\n", err);
  }
  
  delay(100);
  
  // 配置麦克风I2S
  i2s_config_t i2s_mic_config;
  memset(&i2s_mic_config, 0, sizeof(i2s_config_t));
  
  i2s_mic_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  i2s_mic_config.sample_rate = SAMPLE_RATE;
  i2s_mic_config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT; // INMP441需要32位
  i2s_mic_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2s_mic_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_mic_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2s_mic_config.dma_buf_count = 8;
  i2s_mic_config.dma_buf_len = BLOCK_SIZE;
  i2s_mic_config.use_apll = false;
  i2s_mic_config.tx_desc_auto_clear = false;
  
  // 麦克风引脚配置
  i2s_pin_config_t mic_pin_config;
  memset(&mic_pin_config, 0, sizeof(i2s_pin_config_t));
  
  mic_pin_config.bck_io_num = I2S_MIC_BCLK_PIN;
  mic_pin_config.ws_io_num = I2S_MIC_LRCK_PIN;
  mic_pin_config.data_out_num = I2S_PIN_NO_CHANGE;
  mic_pin_config.data_in_num = I2S_MIC_DATA_PIN;
  
  // 安装麦克风I2S驱动
  err = i2s_driver_install(I2S_MIC_PORT, &i2s_mic_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("麦克风驱动安装失败: %d\n", err);
    return false;
  }
  
  err = i2s_set_pin(I2S_MIC_PORT, &mic_pin_config);
  if (err != ESP_OK) {
    Serial.printf("麦克风引脚配置失败: %d\n", err);
    return false;
  }
  
  // 配置扬声器I2S
  i2s_config_t i2s_spk_config;
  memset(&i2s_spk_config, 0, sizeof(i2s_config_t));
  
  i2s_spk_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  i2s_spk_config.sample_rate = SAMPLE_RATE;
  i2s_spk_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_spk_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2s_spk_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_spk_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2s_spk_config.dma_buf_count = 8;
  i2s_spk_config.dma_buf_len = BLOCK_SIZE;
  i2s_spk_config.use_apll = false;
  i2s_spk_config.tx_desc_auto_clear = true;
  
  // 扬声器引脚配置
  i2s_pin_config_t spk_pin_config;
  memset(&spk_pin_config, 0, sizeof(i2s_pin_config_t));
  
  spk_pin_config.bck_io_num = I2S_SPK_BCLK_PIN;
  spk_pin_config.ws_io_num = I2S_SPK_LRCK_PIN;
  spk_pin_config.data_out_num = I2S_SPK_DATA_PIN;
  spk_pin_config.data_in_num = I2S_PIN_NO_CHANGE;
  
  // 安装扬声器I2S驱动
  err = i2s_driver_install(I2S_SPK_PORT, &i2s_spk_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("扬声器驱动安装失败: %d\n", err);
    return false;
  }
  
  err = i2s_set_pin(I2S_SPK_PORT, &spk_pin_config);
  if (err != ESP_OK) {
    Serial.printf("扬声器引脚配置失败: %d\n", err);
    return false;
  }
  
  // 发送一段静音清除可能的噪声
  uint8_t silence[512] = {0};
  size_t bytes_written = 0;
  i2s_write(I2S_SPK_PORT, silence, sizeof(silence), &bytes_written, 100);
  
  Serial.println("I2S接口初始化成功");
  return true;
}

//============================ 抖动缓冲区函数 ============================
/**
 * 初始化改进的抖动缓冲区
 * - 分配内存
 * - 初始化结构体字段
 * - 清空所有音频块
 */
void initJitterBuffer() {
  // 计算需要的块数
  int numChunks = JITTER_BUFFER_SIZE / AUDIO_CHUNK_SIZE + 1;
  
  // 分配内存
  jbuffer.chunks = (AudioChunk*)heap_caps_malloc(
      numChunks * sizeof(AudioChunk), 
      MALLOC_CAP_SPIRAM
  );
  
  if (jbuffer.chunks == NULL) {
    Serial.println("错误：无法为抖动缓冲区分配内存");
    return;
  }
  
  // 初始化结构
  jbuffer.capacity = numChunks;
  jbuffer.size = 0;
  jbuffer.nextExpectedSeq = 0;
  jbuffer.oldestSeq = 0;
  jbuffer.newestSeq = 0;
  jbuffer.initialized = true;
  jbuffer.underflow = false;
  jbuffer.playbackRate = 1.0f;
  
  // 清空所有音频块
  for (int i = 0; i < numChunks; i++) {
    jbuffer.chunks[i].valid = false;
    jbuffer.chunks[i].sequenceNum = 0;
    memset(jbuffer.chunks[i].samples, 0, AUDIO_CHUNK_SIZE * sizeof(int16_t));
  }
  
  Serial.printf("抖动缓冲区已初始化，容量: %d 块 (%d 样本)\n", 
               numChunks, numChunks * AUDIO_CHUNK_SIZE);
}

/**
 * 向抖动缓冲区添加音频数据
 * - 处理序列号连续性
 * - 填充丢失的数据包
 * - 更新缓冲区状态
 * 
 * @param samples 音频样本数组
 * @param numSamples 样本数量
 * @param sequenceNum 数据包序列号
 * @return 操作是否成功
 */
bool jitterBufferAdd(int16_t* samples, int numSamples, uint16_t sequenceNum) {
  if (!jbuffer.initialized || samples == NULL || numSamples <= 0)
    return false;
  
  // 第一次接收数据时设置起始序列号
  if (jbuffer.size == 0) {
    jbuffer.nextExpectedSeq = sequenceNum;
    jbuffer.oldestSeq = sequenceNum;
    jbuffer.newestSeq = sequenceNum;
  }
  
  // 检查序列号是否过于陈旧
  if (sequenceNum < jbuffer.oldestSeq && jbuffer.size > 0) {
    // 太旧的包，忽略
    return false;
  }
  
  // 更新最新序列号
  if (sequenceNum > jbuffer.newestSeq) {
    jbuffer.newestSeq = sequenceNum;
  }
  
  // 计算缓冲区中的位置
  int index = sequenceNum % jbuffer.capacity;
  
  // 复制数据（最多一个块大小）
  int copySize = min(numSamples, AUDIO_CHUNK_SIZE);
  memcpy(jbuffer.chunks[index].samples, samples, copySize * sizeof(int16_t));
  
  // // 如果不足一个块，用静音填充剩余部分
  // if (copySize < AUDIO_CHUNK_SIZE) {
  //   memset(&jbuffer.chunks[index].samples[copySize], 0, 
  //         (AUDIO_CHUNK_SIZE - copySize) * sizeof(int16_t));
  // }
  
  // 更新块信息
  jbuffer.chunks[index].sequenceNum = sequenceNum;
  jbuffer.chunks[index].valid = true;
  
  // 更新缓冲区大小（如果是新块）
  if (jbuffer.size < jbuffer.capacity) {
    jbuffer.size++;
  }
  
  //  // 如果有丢包，添加静音填充
  //  if (sequenceNum > jbuffer.nextExpectedSeq) {
  //   // 有丢包，填充静音
  //    for (uint16_t seq = jbuffer.nextExpectedSeq; seq < sequenceNum; seq++) {
  //      int fillIndex = seq % jbuffer.capacity;
      
  //      // 只填充无效的块
  //      if (!jbuffer.chunks[fillIndex].valid) {
  //        memset(jbuffer.chunks[fillIndex].samples, 0, 
  //               AUDIO_CHUNK_SIZE * sizeof(int16_t));
  //       jbuffer.chunks[fillIndex].sequenceNum = seq;
  //        jbuffer.chunks[fillIndex].valid = true;
        
  //       // 更新缓冲区大小（如果是新块）
  //       if (jbuffer.size < jbuffer.capacity) {
  //         jbuffer.size++;
  //       }
  //     }
  //   }
  // }
  
  // 更新下一个期望序列号
  jbuffer.nextExpectedSeq = sequenceNum + 1;
  
  return true;
}

/**
 * 从抖动缓冲区读取数据
 * 
 * 此函数提供智能的音频数据读取：
 * - 动态调整播放速率以维持目标缓冲区水平
 * - 应用交叉淡入淡出技术减少不连续性
 * - 处理缓冲区下溢情况
 * 
 * @param outputBuffer 输出缓冲区
 * @param maxSamples 最大读取样本数
 * @return 实际读取的样本数
 */
int jitterBufferRead(int16_t* outputBuffer, int maxSamples) {
    if (!jbuffer.initialized || outputBuffer == NULL || maxSamples <= 0)
        return 0;
    
    // 检查缓冲区是否准备好播放
    if (jbuffer.size < JITTER_MIN_THRESHOLD / AUDIO_CHUNK_SIZE) {
        if (jbuffer.size > 0) {
            jbuffer.underflow = true;
        }
        // 缓冲区不足，返回静音
        memset(outputBuffer, 0, maxSamples * sizeof(int16_t));
        return maxSamples;
    }
    
    // 计算需要读取的块数
    int chunksToRead = min(maxSamples / AUDIO_CHUNK_SIZE, jbuffer.size);
    if (chunksToRead == 0) chunksToRead = 1;  // 至少读一块
    
    // 平滑恢复正常播放速率
    jbuffer.playbackRate = 0.95f * jbuffer.playbackRate + 0.05f * 1.0f;

    // 实际播放的样本数 (基于播放速率)
    int effectiveChunks = round(chunksToRead * jbuffer.playbackRate);
    effectiveChunks = max(1, min(effectiveChunks, jbuffer.size));
    
    // 计算实际可以播放的样本数
    int samplesRead = 0;
    
    // 读取并拼接各块
    for (int i = 0; i < effectiveChunks && samplesRead < maxSamples; i++) {
        uint16_t seq = (jbuffer.oldestSeq + i) % 65536;
        int index = seq % jbuffer.capacity;
        
        if (jbuffer.chunks[index].valid && 
            jbuffer.chunks[index].sequenceNum == seq) {
            
            // 计算可复制的样本数
            int copySize = min(AUDIO_CHUNK_SIZE, maxSamples - samplesRead);
            
            // 应用平滑处理 - 与前一个块和后一个块进行交叉淡入淡出
            for (int j = 0; j < copySize; j++) {
                float sample = jbuffer.chunks[index].samples[j];
                
                // 边界处理 - 在块的边缘应用淡入淡出
                if (j < 4) {  // 前4个样本淡入
                    float fadeIn = j / 4.0f;
                    if (i > 0) {  // 如果不是第一个块，与前一个块交叉淡入
                        uint16_t prevSeq = (seq - 1) % 65536;
                        int prevIndex = prevSeq % jbuffer.capacity;
                        
                        if (jbuffer.chunks[prevIndex].valid && 
                            jbuffer.chunks[prevIndex].sequenceNum == prevSeq) {
                            // 混合前一个块的末尾样本
                            float prevSample = jbuffer.chunks[prevIndex].samples[AUDIO_CHUNK_SIZE - 4 + j];
                            sample = prevSample * (1.0f - fadeIn) + sample * fadeIn;
                        }
                    }
                } else if (j >= copySize - 4 && j < copySize) {  // 最后4个样本淡出
                    float fadeOut = (copySize - j) / 4.0f;
                    if (i < effectiveChunks - 1) {  // 如果不是最后一个块，与后一个块交叉淡出
                        uint16_t nextSeq = (seq + 1) % 65536;
                        int nextIndex = nextSeq % jbuffer.capacity;
                        
                        if (jbuffer.chunks[nextIndex].valid && 
                            jbuffer.chunks[nextIndex].sequenceNum == nextSeq) {
                            // 混合后一个块的开始样本
                            float nextSample = jbuffer.chunks[nextIndex].samples[j - (copySize - 4)];
                            sample = sample * fadeOut + nextSample * (1.0f - fadeOut);
                        }
                    }
                }
                
                // 存储处理后的样本 - 数据已经是16位PCM，不需要再解码
                outputBuffer[samplesRead + j] = (int16_t)sample;
            }
            
            samplesRead += copySize;
        }
        
        // 标记块为无效（已消费）
        jbuffer.chunks[index].valid = false;
    }
    
    // 更新缓冲区状态
    jbuffer.size -= effectiveChunks;
    jbuffer.oldestSeq = (jbuffer.oldestSeq + effectiveChunks) % 65536;
    
    // 如果缓冲区已经溢出过，显示恢复消息
    if (jbuffer.underflow && jbuffer.size >= JITTER_MIN_THRESHOLD / AUDIO_CHUNK_SIZE) {
        Serial.println("抖动缓冲区已恢复播放");
        jbuffer.underflow = false;
    }
    
    return samplesRead;
}

//============================ 音频处理函数 ============================
/**
 * 语音活动检测 (VAD) 函数
 * - 计算当前帧能量
 * - 自适应更新背景噪声水平
 * - 与阈值比较检测语音活动
 * 
 * @param samples 音频样本数组
 * @param sampleCount 样本数量
 * @return 是否检测到语音活动
 */
bool detectVoiceActivity(int16_t* samples, int sampleCount) {
  // 能量平滑系数
  const float ENERGY_SMOOTHING = 0.7;
  
  // 1. 计算当前帧能量
  float energy = 0;
  for (int i = 0; i < sampleCount; i++) {
    energy += samples[i] * samples[i];
  }
  energy = sqrt(energy / sampleCount);
  
  // 2. 平滑能量值 (避免突然变化)
  currentEnergy = ENERGY_SMOOTHING * currentEnergy + 
                 (1 - ENERGY_SMOOTHING) * energy;
  
  // 3. 环境噪声自适应更新 (仅在低能量时缓慢更新)
  if (currentEnergy < backgroundNoise * 200) {
    backgroundNoise = 0.95 * backgroundNoise + 0.05 * currentEnergy;
  }
  
  // 4. 语音检测判决
  bool voiceDetected = (currentEnergy > backgroundNoise + VAD_THRESHOLD);
  
  // 5. 状态更新
  if (voiceDetected) {
    lastVoiceTime = millis();
    vadActive = true;
  } else if (millis() - lastVoiceTime > VAD_HOLD_TIME) {
    vadActive = false;
  }
  
  // 调试信息
  if (showDebugInfo && voiceDetected && !vadActive) {
    Serial.printf("语音检测：能量=%.1f, 噪声=%.1f, 阈值=%.1f\n", 
                 currentEnergy, backgroundNoise, VAD_THRESHOLD);
  }
  
  // 6. 返回是否应该传输
  return vadActive;
}

/**
 * 处理音频输入和发送
 * - 从麦克风读取数据
 * - 应用音频处理
 * - 检测语音活动
 * - 发送到远程设备
 */
void processAudioIO() {
  size_t bytes_read = 0;
  
  // 从麦克风读取数据
  esp_err_t err = i2s_read(I2S_MIC_PORT, micBuffer, BLOCK_SIZE * sizeof(int32_t), &bytes_read, 0);
  
  if (err == ESP_OK && bytes_read > 0) {
    int samples_read = bytes_read / sizeof(int32_t);
    sampleCount += samples_read;
    
    // 应用音频处理 - 转换为16位并应用音量控制和噪声门限
    applyAudioProcessing(micBuffer, processedBuffer, samples_read);
    
    // 执行VAD检测
    // bool shouldTransmit = detectVoiceActivity(processedBuffer, min(samples_read, 256));
    bool shouldTransmit = true;  // 暂时始终传输

    //  size_t bytes_written = 0;
    // i2s_write(I2S_SPK_PORT, processedBuffer, BLOCK_SIZE * sizeof(int16_t), &bytes_written, 0);

    // 发送到远程设备
    if (espnowTransmitEnabled && shouldTransmit) {
      // 控制发送频率
        bool sent = wireless_send_audio(processedBuffer, samples_read);        
        if (sent) {
          sendCount++;
          audioSeqNum++; // 为下一个包增加序列号
        }
    }
  }
}

/**
 * 播放从远程设备接收到的音频数据
 * 
 * 此函数:
 * 1. 从无线模块获取接收到的音频数据
 * 2. 将数据添加到抖动缓冲区
 * 3. 从抖动缓冲区读取数据进行播放
 */
void playRemoteAudio() {
  // 从无线模块获取接收到的音频数据 (保留兼容旧版本代码)
  int samples_read = wireless_get_audio(playBuffer, BLOCK_SIZE);
  static uint16_t localAudioSeq = 0;
  
  // 创建输出缓冲区
  int16_t outputBuffer[BLOCK_SIZE];
  
  if (samples_read > 0) {
    // 将接收到的数据分块添加到抖动缓冲区
    for (int offset = 0; offset < samples_read; offset += AUDIO_CHUNK_SIZE) {
      int chunkSize = min(AUDIO_CHUNK_SIZE, samples_read - offset);
      jitterBufferAdd(&playBuffer[offset], chunkSize, localAudioSeq++);
    }
  }

  // 准备播放缓冲区
  int outSamples = 0;
  
  // 使用混音功能处理多路音频
  mixAudioFromPeers(outputBuffer, BLOCK_SIZE);
  outSamples = BLOCK_SIZE; // 混音器总是生成完整的缓冲区
  
  // 如果混音缓冲区为空(没有活跃设备)，则使用常规抖动缓冲区
  bool hasAudio = false;
  for (int i = 0; i < BLOCK_SIZE; i++) {
    if (outputBuffer[i] != 0) {
      hasAudio = true;
      break;
    }
  }
  
  if (!hasAudio) {
    // 退回到传统方法 - 从抖动缓冲区读取
    outSamples = jitterBufferRead(outputBuffer, BLOCK_SIZE);
  }
  
  // 播放处理后的音频
  if (outSamples > 0) {
    size_t bytes_written = 0;
    i2s_write(I2S_SPK_PORT, outputBuffer, outSamples * sizeof(int16_t), &bytes_written, 0);
  }
}

/**
 * 音频处理函数 - 处理并压缩麦克风输入
 * 
 * 此函数完成三个主要任务：
 * 1. 将32位麦克风数据转换为16位PCM
 * 2. 应用噪声门限和音量控制
 * 3. 将16位PCM压缩为8位A-law编码
 * 
 * @param input 输入数据 (32位)
 * @param output 输出数据 (将存储8位A-law压缩值)
 * @param samples 样本数量
 */
void applyAudioProcessing(int32_t* input, int16_t* output, size_t samples) {
  // 噪声门限阈值 - 降低背景噪音
  static const int32_t NOISE_THRESHOLD = 600;
  
  // 确保g711查找表已初始化(如果使用查找表优化)
  #if USE_LOOKUP_TABLES
  static bool tablesInitialized = false;
  if (!tablesInitialized) {
    g711_init_tables();
    tablesInitialized = true;
    Serial.println("G711查找表已初始化");
  }
  #endif
  
  // 处理每个音频样本
  for (size_t i = 0; i < samples; i++) {
    // 从32位INMP441数据中提取有效的16位PCM (右移16位)
    int32_t sample = input[i] >> 16;
  
    // 噪声门限处理 - 消除低音量背景噪声
    if (abs(sample) < NOISE_THRESHOLD) {
      // 低于门限的信号视为噪声，直接设为零
      sample = 0;
    } else {
      // 对介于门限和2倍门限之间的信号进行软衰减
      if (abs(sample) < NOISE_THRESHOLD * 2) {
        // 软衰减公式: 根据与阈值的距离进行线性缩放
        sample = sample * (abs(sample) - NOISE_THRESHOLD) / NOISE_THRESHOLD;
      }
      
      // 应用音量控制 - 根据用户设置的音量调整信号强度
      sample = (int32_t)(sample * audioVolume);
    }
    
    // 限制到16位PCM范围 (-32768到32767)
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
   
    // 压缩为8位A-law格式 - 这是关键改变！
    // 我们把8位压缩值存在16位数组的低8位，高8位清零
    // 这样虽然有些浪费空间，但保持了与现有代码结构的兼容性
    uint8_t compressed = linear2alaw((int16_t)sample);
    
    // 将8位压缩值存入16位数组
    // 注意：这里我们只使用低8位，高8位设为0
    output[i] = (int16_t)(compressed & 0xFF);
  }
  
  // 调试信息 - 帮助了解压缩效果
  static unsigned long lastDebugTime = 0;
  if (millis() - lastDebugTime > 10000) {  // 每10秒输出一次
    Serial.println("音频已压缩为8位A-law格式");
    lastDebugTime = millis();
  }
}

//============================ 测试函数 ============================
/**
 * 测试麦克风
 * - 读取短段音频样本
 * - 计算统计信息 (最大值、平均值)
 */
void testMicrophone() {
  static unsigned long lastTestTime = 0;
  if (millis() - lastTestTime < 5000) return; // 每5秒测试一次
  
  lastTestTime = millis();
  
  // 创建测试缓冲区
  int32_t testBuffer[64];
  size_t bytes_read = 0;
  
  // 读取麦克风
  esp_err_t err = i2s_read(I2S_MIC_PORT, testBuffer, sizeof(testBuffer), &bytes_read, 100);
  
  if (err == ESP_OK && bytes_read > 0) {
    int samples = bytes_read / sizeof(int32_t);
    
    // 计算统计信息
    int nonZeroCount = 0;
    int32_t maxVal = 0;
    int32_t sumVal = 0;
    
    for (int i = 0; i < samples; i++) {
      int32_t sample = testBuffer[i] >> 16;
      sumVal += abs(sample);
      if (sample != 0) nonZeroCount++;
      if (abs(sample) > maxVal) maxVal = abs(sample);
    }
    
    int32_t avgVal = nonZeroCount > 0 ? sumVal / nonZeroCount : 0;
    
    Serial.printf("麦克风测试: 样本=%d, 非零=%d, 最大值=%ld, 平均值=%ld\n",
                 samples, nonZeroCount, maxVal, avgVal);
  } else {
    Serial.printf("麦克风测试失败: %d\n", err);
  }
}

//============================ 模式处理函数 ============================
/**
 * 正常工作模式
 * - 处理用户输入命令
 * - 处理音频输入输出
 * - 定期显示统计信息
 */
void runNormalMode() {
  // 读取串口命令
  if (Serial.available() > 0) {
    char command = Serial.read();
    
    // 单独处理频道选择命令 (1-8)
    if (command >= '1' && command <= '8') {
      uint8_t channelId = command - '0';  // 将字符转换为数字
      if (wireless_join_channel(channelId)) {
        Serial.printf("已加入频道 %d\n", channelId);
      } else {
        Serial.println("加入频道失败");
      }
    }
    // 其他命令使用 switch 语句
    else {
      switch (command) {
        case 's': // 进入设置模式
          currentMode = MODE_SETTINGS;
          runSettingsMode();
          break;
        case '+': // 增加音量
          audioVolume += 0.1;
          if (audioVolume > 3.0) audioVolume = 3.0;
          Serial.printf("音量: %.1f\n", audioVolume);
          break;
        case '-': // 降低音量
          audioVolume -= 0.1;
          if (audioVolume < 0.1) audioVolume = 0.1;
          Serial.printf("音量: %.1f\n", audioVolume);
          break;
        case 'e': // 切换ESP-NOW发送
          espnowTransmitEnabled = !espnowTransmitEnabled;
          Serial.printf("ESP-NOW发送: %s\n", espnowTransmitEnabled ? "开启" : "关闭");
          break;
        case 'd': // 切换调试信息
          showDebugInfo = !showDebugInfo;
          Serial.printf("调试信息: %s\n", showDebugInfo ? "开启" : "关闭");
          break;
        case 'p': // 显示系统状态
          displayStatus();
          break;
        case 't': // 运行ESP-NOW诊断
          wireless_diagnostic();
          break;
        case 'c': { // 扫描最佳通道
          int bestChannel = wireless_find_best_channel();
          Serial.printf("最佳通道: %d\n", bestChannel);
          if (wireless_set_channel(bestChannel)) {
            Serial.println("已切换到最佳通道");
          }
          break;
        }
        case '0': // 离开当前频道
          if (wireless_leave_channel()) {
            Serial.println("已离开频道");
          } else {
            Serial.println("离开频道失败");
          }
          break;
        case 'm': { // 静音/取消静音用户
          // 显示当前频道中的用户列表
          PeerInfo peers[MAX_PEERS];
          int peerCount = wireless_get_peers_in_channel(peers, MAX_PEERS);
          
          if (peerCount == 0) {
            Serial.println("当前频道中没有其他用户");
            break;
          }
          
          Serial.println("当前频道中的用户:");
          for (int i = 0; i < peerCount; i++) {
            Serial.printf("%d) 用户ID: %d\n", i+1, peers[i].deviceId);
          }
          
          Serial.println("输入用户编号(1-8)来切换静音状态:");
          
          // 等待用户输入
          unsigned long startTime = millis();
          while (Serial.available() == 0 && millis() - startTime < 5000) {
            delay(100); // 等待输入，最多5秒
          }
          
          if (Serial.available() > 0) {
            char userSel = Serial.read();
            if (userSel >= '1' && userSel <= '8') {
              int idx = userSel - '1';
              if (idx < peerCount) {
                bool currentMute = false;
                // 查找当前静音状态
                for (int i = 0; i < MAX_PEERS; i++) {
                  if (peerDevices[i].info.active && 
                      peerDevices[i].info.deviceId == peers[idx].deviceId) {
                    currentMute = peerDevices[i].muted;
                    break;
                  }
                }
                
                // 切换静音状态
                if (setPeerMuted(peers[idx].deviceId, !currentMute)) {
                  Serial.printf("用户 %d %s\n", peers[idx].deviceId, 
                               !currentMute ? "已静音" : "已取消静音");
                } else {
                  Serial.println("设置静音状态失败");
                }
              }
            }
          }
          break;
        }
        case 'v': { // 调整用户音量
          char volCmd = ' ';
          // 等待下一个字符
          unsigned long startTime = millis();
          while (Serial.available() == 0 && millis() - startTime < 1000) {
            delay(50);
          }
          
          if (Serial.available() > 0) {
            volCmd = Serial.read();
          }
          
          if (volCmd != '+' && volCmd != '-') {
            Serial.println("输入'v+'增加用户音量或'v-'减少用户音量");
            break;
          }
          
          // 显示当前频道中的用户列表
          PeerInfo peers[MAX_PEERS];
          int peerCount = wireless_get_peers_in_channel(peers, MAX_PEERS);
          
          if (peerCount == 0) {
            Serial.println("当前频道中没有其他用户");
            break;
          }
          
          Serial.println("当前频道中的用户:");
          for (int i = 0; i < peerCount; i++) {
            // 查找当前音量
            float currentVol = 1.0f;
            for (int j = 0; j < MAX_PEERS; j++) {
              if (peerDevices[j].info.active && 
                  peerDevices[j].info.deviceId == peers[i].deviceId) {
                currentVol = peerDevices[j].volume;
                break;
              }
            }
            
            Serial.printf("%d) 用户ID: %d (音量: %.1f)\n", i+1, peers[i].deviceId, currentVol);
          }
          
          Serial.println("输入用户编号(1-8)来调整音量:");
          
          // 等待用户输入
          startTime = millis();
          while (Serial.available() == 0 && millis() - startTime < 5000) {
            delay(100); // 等待输入，最多5秒
          }
          
          if (Serial.available() > 0) {
            char userSel = Serial.read();
            if (userSel >= '1' && userSel <= '8') {
              int idx = userSel - '1';
              if (idx < peerCount) {
                float currentVol = 1.0f;
                // 查找当前音量
                for (int i = 0; i < MAX_PEERS; i++) {
                  if (peerDevices[i].info.active && 
                      peerDevices[i].info.deviceId == peers[idx].deviceId) {
                    currentVol = peerDevices[i].volume;
                    break;
                  }
                }
                
                // 调整音量
                float newVol = currentVol;
                if (volCmd == '+') {
                  newVol += 0.2f;
                  if (newVol > 3.0f) newVol = 3.0f;
                } else { // volCmd == '-'
                  newVol -= 0.2f;
                  if (newVol < 0.0f) newVol = 0.0f;
                }
                
                if (setPeerVolume(peers[idx].deviceId, newVol)) {
                  Serial.printf("用户 %d 音量已调整为 %.1f\n", peers[idx].deviceId, newVol);
                } else {
                  Serial.println("调整音量失败");
                }
              }
            }
          }
          break;
        }
        default:
          Serial.println("\n可用命令:");
          Serial.println("1-8: 切换频道");
          Serial.println("0: 离开当前频道");
          Serial.println("s: 进入设置模式");
          Serial.println("+/-: 调整音量");
          Serial.println("e: 切换ESP-NOW发送");
          Serial.println("d: 切换调试信息");
          Serial.println("p: 显示系统状态");
          Serial.println("t: 运行ESP-NOW诊断");
          Serial.println("c: 扫描最佳通道");
          Serial.println("m: 静音/取消静音用户");
          Serial.println("v+/v-: 增加/减少用户音量");
      }
    }
  }
  
  // 处理音频输入/输出
  processAudioIO();
  
  // 播放远程音频
  playRemoteAudio();
  
  // 调试模式下定期测试麦克风
  if (showDebugInfo) {
    testMicrophone();
  }
  
  // 每10秒显示一次状态
  static unsigned long lastStatusTime = 0;
  if (millis() - lastStatusTime > 10000) {
    displayStatus();
    lastStatusTime = millis();
  }
  
  // 稍微延迟，让出CPU时间
  delay(5);
}

/**
 * 设置模式
 * - 显示菜单
 * - 处理用户选项
 * - 同时继续处理接收的音频
 */
void runSettingsMode() {
  Serial.println("\n===== 设置模式 =====");
  Serial.println("1. 设置WiFi通道 (当前: " + String(wireless_get_channel()) + ")");
  Serial.println("2. 切换ESP-NOW发送 (当前: " + String(espnowTransmitEnabled ? "开启" : "关闭") + ")");
  Serial.println("3. 切换调试信息 (当前: " + String(showDebugInfo ? "开启" : "关闭") + ")");
  Serial.println("4. 重置无线系统");
  Serial.println("5. 显示系统状态");
  Serial.println("6. 扫描最佳通道");
  Serial.println("7. 运行ESP-NOW诊断");
  Serial.println("8. 返回正常模式");
  Serial.println("==== 频道控制 ====");
  Serial.println("c. 加入频道 (当前: " + String(wireless_get_current_channel_id()) + ")");
  Serial.println("d. 离开当前频道");
  Serial.println("e. 显示所有频道");
  Serial.println("f. 显示当前频道设备");
  Serial.println("请输入选项:");
  
  while (true) {
    if (Serial.available() > 0) {
      char option = Serial.read();
      
      switch (option) {
        case '1': {
          Serial.println("请输入WiFi通道 (1-13):");
          // 等待用户输入
          String input = "";
          while (true) {
            if (Serial.available()) {
              char c = Serial.read();
              if (c >= '0' && c <= '9') {
                input += c;
                Serial.print(c);
              } else if (c == '\r' || c == '\n') {
                if (input.length() > 0) {
                  Serial.println();
                  break;
                }
              }
            }
            delay(10);
          }
          
          int newChannel = input.toInt();
          if (wireless_set_channel(newChannel)) {
            Serial.println("WiFi通道已更新");
          } else {
            Serial.println("无效的通道");
          }
          break;
        }
        
        case '2': {
          espnowTransmitEnabled = !espnowTransmitEnabled;
          Serial.printf("ESP-NOW发送: %s\n", espnowTransmitEnabled ? "开启" : "关闭");
          break;
        }
        
        case '3': {
          showDebugInfo = !showDebugInfo;
          Serial.printf("调试信息: %s\n", showDebugInfo ? "开启" : "关闭");
          break;
        }
        
        case '4': {
          Serial.println("重置无线系统中...");
          wireless_reset();
          Serial.println("无线系统已重置");
          break;
        }
        
        case '5': {
          displayStatus();
          break;
        }
        
        case '6': {
          int bestChannel = wireless_find_best_channel();
          if (bestChannel > 0) {
            wireless_set_channel(bestChannel);
            Serial.printf("已切换到最佳通道: %d\n", bestChannel);
          }
          break;
        }
        
        case '7': {
          wireless_diagnostic();
          break;
        }
        
        case '8': {
          currentMode = MODE_NORMAL;
          Serial.println("返回正常模式");
          return;
        }
        
        case 'c': {
          Serial.println("请输入要加入的频道 (1-8):");
          // 等待用户输入
          String input = "";
          while (true) {
            if (Serial.available()) {
              char c = Serial.read();
              if (c >= '0' && c <= '9') {
                input += c;
                Serial.print(c);
              } else if (c == '\r' || c == '\n') {
                if (input.length() > 0) {
                  Serial.println();
                  break;
                }
              }
            }
            delay(10);
          }
          
          uint8_t channelId = input.toInt();
          if (wireless_join_channel(channelId)) {
            Serial.printf("已加入频道 %d\n", channelId);
          } else {
            Serial.println("加入频道失败");
          }
          break;
        }
        
        case 'd': {
          if (wireless_leave_channel()) {
            Serial.println("已离开频道");
          } else {
            Serial.println("离开频道失败");
          }
          break;
        }
        
        case 'e': {
          ChannelInfo channels[MAX_CHANNELS];
          int count = wireless_get_active_channels(channels, MAX_CHANNELS);
          
          if (count > 0) {
            Serial.println("活跃频道列表:");
            for (int i = 0; i < count; i++) {
              Serial.printf("频道 %d: %d个设备\n", 
                            channels[i].channelId, 
                            channels[i].peerCount);
            }
          } else {
            Serial.println("未发现活跃频道");
          }
          break;
        }
        
        case 'f': {
          PeerInfo peers[MAX_PEERS];
          int count = wireless_get_peers_in_channel(peers, MAX_PEERS);
          
          uint8_t channelId = wireless_get_current_channel_id();
          if (channelId == 0) {
            Serial.println("当前未加入任何频道");
            break;
          }
          
          Serial.printf("频道 %d 中的设备:\n", channelId);
          if (count > 0) {
            for (int i = 0; i < count; i++) {
              Serial.printf("设备 %d: ID=%d, MAC=%02X:%02X:%02X:%02X:%02X:%02X\n", 
                            i+1, peers[i].deviceId,
                            peers[i].mac[0], peers[i].mac[1], peers[i].mac[2],
                            peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
            }
          } else {
            Serial.println("频道内暂无其他设备");
          }
          break;
        }
      }
    }
    
    // 让无线系统继续处理数据，这样可以保持连接
    wireless_process();
    
    // 仍然处理接收的音频，确保即使在设置模式下也能收到声音
    playRemoteAudio();
    
    delay(10);
  }
}

//============================ 状态显示函数 ============================
/**
 * 显示系统状态
 * - 设备信息
 * - 连接状态
 * - 内存使用情况
 * - 任务堆栈信息
 */
void displayStatus() {
  Serial.println("\n===== 系统状态 =====");
  Serial.printf("设备ID: %d, 固件版本: %s\n", DEVICE_ID, FW_VERSION);
  Serial.println("本机MAC: " + wireless_get_my_mac());
  
  // 添加频道状态信息
  uint8_t channelId = wireless_get_current_channel_id();
  if (channelId > 0) {
    Serial.printf("当前频道: %d (WiFi通道: %d)\n", channelId, wireless_get_channel());
    
    // 获取频道内的对等设备
    PeerInfo peers[MAX_PEERS];
    int peerCount = wireless_get_peers_in_channel(peers, MAX_PEERS);
    Serial.printf("频道内设备数: %d\n", peerCount);
    
    if (peerCount > 0) {
      Serial.println("设备列表:");
      for (int i = 0; i < peerCount; i++) {
        Serial.printf("  设备 %d: ID=%d, MAC=%02X:%02X:%02X:%02X:%02X:%02X\n", 
                      i+1, peers[i].deviceId,
                      peers[i].mac[0], peers[i].mac[1], peers[i].mac[2],
                      peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
      }
    }
  } else {
    Serial.println("当前未加入频道");
    Serial.printf("WiFi通道: %d\n", wireless_get_channel());
  }
  
  // 其他状态信息
  uint32_t sent, received, lost;
  wireless_get_stats(&sent, &received, &lost);
  
  Serial.printf("音量: %.1f\n", audioVolume);
  Serial.printf("ESP-NOW发送: %s\n", espnowTransmitEnabled ? "开启" : "关闭");
  Serial.printf("包统计: 发送=%u, 接收=%u, 丢失=%u\n", sent, received, lost);
  Serial.printf("VAD状态: %s (噪声基线: %.1f, 当前能量: %.1f)\n", 
                vadActive ? "活跃" : "静默", backgroundNoise, currentEnergy);
  
  Serial.printf("抖动缓冲区: %d个样本, 目标水平=%d, 最小阈值=%d\n", 
                JITTER_BUFFER_SIZE, JITTER_TARGET_LEVEL, JITTER_MIN_THRESHOLD);
  
  Serial.printf("内存使用: 总PSRAM=%d字节, 空闲=%d字节\n", 
                ESP.getPsramSize(), ESP.getFreePsram());
  
  Serial.println("==================\n");
}

/**
 * 打印系统信息
 * - 芯片和内存信息
 * - 硬件配置
 */
void printSystemInfo() {
  Serial.println("\n===== ESP32-S3 无线音频传输系统 V2.0 =====");
  Serial.println("改进版：使用原始音频传输，专注远程音频");
  Serial.printf("设备ID: %d, 固件版本: %s\n", DEVICE_ID, FW_VERSION);
  Serial.printf("芯片型号: %s, 核心数: %d\n", "ESP32-S3", ESP.getChipCores());
  Serial.printf("CPU频率: %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("闪存大小: %d字节\n", ESP.getFlashChipSize());
  
  if (esp_spiram_is_initialized()) {
    Serial.printf("PSRAM大小: %d字节\n", ESP.getPsramSize());
  } else {
    Serial.println("PSRAM未初始化");
  }
  
  // 硬件配置
  Serial.println("硬件配置:");
  Serial.printf("麦克风I2S: BCLK=%d, LRCK=%d, DATA=%d\n", 
               I2S_MIC_BCLK_PIN, I2S_MIC_LRCK_PIN, I2S_MIC_DATA_PIN);
  Serial.printf("扬声器I2S: BCLK=%d, LRCK=%d, DATA=%d\n", 
               I2S_SPK_BCLK_PIN, I2S_SPK_LRCK_PIN, I2S_SPK_DATA_PIN);
  Serial.printf("采样率: %d Hz\n", SAMPLE_RATE);
  
  Serial.println("====================");
}
