/**
 * ESP32-S3 无线音频传输系统 - 无线通信头文件 (wireless_system.h)
 * 
 * 【文件说明】
 * 本头文件定义了ESP32-S3无线通信系统的接口，包括数据结构、常量定义和函数声明。
 * 它是连接主程序与无线实现的桥梁，提供了完整的ESP-NOW通信、频道管理和多用户会话功能。
 * 
 * 【主要组件】
 * 1. 数据结构定义:
 *    - AudioPacket: 音频数据包结构 [行32]
 *    - PeerInfo: 对等设备信息结构 [行178]
 *    - ChannelInfo: 频道信息结构 [行187]
 *    - RingBuffer: 环形缓冲区结构 [行194]
 *    - PeerWithAudio: 带音频缓冲的对等设备 [行204]
 * 
 * 2. 常量与配置:
 *    - 默认WiFi通道 [行24]
 *    - 音频块大小 [行25]
 *    - 音频传输间隔 [行26]
 *    - 最大对等设备数 [行176]
 *    - 最大频道数 [行177]
 * 
 * 3. 核心功能接口:
 *    - 无线系统初始化与管理 [行48-106]
 *    - 音频数据传输 [行108-146]
 *    - 频道和多用户会话管理 [行212-309]
 * 
 * 【实现说明】
 * 本系统基于ESP-NOW协议实现点对点实时音频传输，支持以下功能:
 * 
 * 1. 基础通信功能:
 *    - 设备自动配对与连接
 *    - 音频数据收发与管理
 *    - 通道扫描与最佳通道选择
 * 
 * 2. 频道管理系统:
 *    - 支持8个频道动态切换
 *    - 设备可在频道间移动
 *    - 频道内设备自动发现
 * 
 * 3. 多用户会话:
 *    - 每个频道支持多达8个用户
 *    - 用户音频独立控制(音量、静音)
 *    - 多路音频智能混合
 * 
 * 该接口设计注重简洁性和灵活性，主程序只需调用少量函数即可实现复杂的无线音频通信功能，
 * 同时隐藏了ESP-NOW的底层细节，提供更高层次的抽象，便于应用开发。
 */

#ifndef WIRELESS_SYSTEM_H
#define WIRELESS_SYSTEM_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_heap_caps.h>
#include <esp32/spiram.h>
#include <driver/i2s.h>
#include <g711.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
//============================ 设备配置 ============================
// 设备相关配置
#define DEVICE_ID 2            // 设备ID，为每台设备设置一个唯一值(1-8)
#define FW_VERSION "2.0.0"     // 固件版本

// 默认WiFi通道，可以通过测试确定最佳通道
#define DEFAULT_WIFI_CHANNEL 1  // 1, 6, 11是较好的选择

//============================ I2S引脚定义 ============================
// I2S引脚定义 - 麦克风接口
#define I2S_MIC_BCLK_PIN     17   // 麦克风BCLK时钟引脚
#define I2S_MIC_LRCK_PIN     16   // 麦克风WS/LRCK字时钟引脚
#define I2S_MIC_DATA_PIN     18   // 麦克风SD数据输入引脚

// I2S引脚定义 - 扬声器接口
#define I2S_SPK_BCLK_PIN     5    // 扬声器BCLK时钟引脚
#define I2S_SPK_LRCK_PIN     6    // 扬声器WS字时钟引脚
#define I2S_SPK_DATA_PIN     7    // 扬声器数据输出引脚

// I2S端口定义
#define I2S_MIC_PORT I2S_NUM_0    // 麦克风使用I2S端口0
#define I2S_SPK_PORT I2S_NUM_1    // 扬声器使用I2S端口1

//============================ 音频参数定义 ============================
#define SAMPLE_RATE 16000         // 16kHz采样率
#define BLOCK_SIZE 320            // 每次处理20ms音频(320个样本@16kHz)
#define MAX_PACKET_SIZE 220       // ESP-NOW最大有效负载约250字节，留出一些余量
#define RAW_SAMPLES_PER_PACKET 80 // 一个包能发送的最大16位PCM样本数
#define TRANSMIT_INTERVAL 5      // 10ms发送间隔，提高实时性
#define NOISE_GATE_THRESHOLD 600  // 噪声门限阈值，降低以便捕获更多声音



// 缓冲区大小定义 - 利用PSRAM
#define MIC_BUFFER_SIZE 4800      // 麦克风缓冲区(300ms)
#define RX_BUFFER_SIZE 32000      // 接收缓冲区(2秒)
//============================ 内存分配属性 ============================
#define BUFFER_ATTR __attribute__((aligned(4)))  // 缓冲区属性，用于PSRAM对齐

//============================ 公共接口函数 ============================

/**
 * 生成随机MAC地址
 * 
 * @param mac 存储生成的MAC地址(6字节数组)
 * @param isLocal 是否生成本地管理的MAC地址
 */
void generateRandomMAC(uint8_t* mac, bool isLocal);

/**
 * 初始化无线系统
 * 
 * @param deviceId 本设备ID
 * @return 初始化是否成功
 */
bool wireless_init(uint8_t deviceId);

/**
 * 发送音频数据
 * 
 * @param audio_data 音频数据缓冲区
 * @param samples 样本数
 * @return 发送是否成功
 */
bool wireless_send_audio(int16_t* audio_data, size_t samples);

/**
 * 获取接收的音频数据
 * 
 * @param buffer 输出缓冲区
 * @param max_samples 最大样本数
 * @return 实际获取的样本数
 */
int wireless_get_audio(int16_t* buffer, size_t max_samples);

/**
 * 扫描并选择最佳通道
 * 
 * @return 选择的最佳通道
 */
int wireless_find_best_channel();

/**
 * 设置WiFi通道
 * 
 * @param channel 通道编号(1-13)
 * @return 设置是否成功
 */
bool wireless_set_channel(int channel);
/**
 * 获取目标设备的MAC地址
 * 
 * @return MAC地址字符串
 */
String wireless_get_peer_mac();

/**
 * 获取本机MAC地址
 * 
 * @return MAC地址字符串
 */
String wireless_get_my_mac();

/**
 * 获取连接状态
 * 
 * @return 是否连接
 */
bool wireless_is_connected();

/**
 * 获取当前通道
 * 
 * @return 当前通道号
 */
int wireless_get_channel();

/**
 * 获取统计信息
 * 
 * @param sent 已发送数据包数
 * @param received 已接收数据包数
 * @param lost 丢包数
 */
void wireless_get_stats(uint32_t* sent, uint32_t* received, uint32_t* lost);

/**
 * 处理接收到的数据和维护连接
 * 此函数应当在每个循环中调用
 */
void wireless_process();

/**
 * 重置无线系统
 */
void wireless_reset();
/**
 * 清空接收缓冲区
 */
void wireless_flush_rx_buffer();

/**
 * 执行连接测试并输出诊断信息
 */
void wireless_diagnose();

/**
 * 设置对等设备MAC地址
 * 
 * @param mac MAC地址数组(6字节)
 * @return 设置是否成功
 */
bool wireless_set_peer_mac(const uint8_t* mac);

/**
 * 设置本机MAC地址
 * 
 * @param mac MAC地址数组(6字节)
 * @return 设置是否成功
 */
bool wireless_set_my_mac(const uint8_t* mac);

//============================ 多人会话和频道切换 ============================
// 频道和对等设备限制
#define MAX_PEERS 8         // 最大对等设备数量
#define MAX_CHANNELS 8      // 支持的频道数量
#define ALL_PEERS 0xFF      // 广播到所有对等设备的标识

// 对等设备信息结构体
typedef struct {
    uint8_t mac[6];        // 对等设备MAC地址
    uint8_t deviceId;      // 设备ID
    uint8_t channel;       // 设备频道
    bool active;           // 是否活动
    unsigned long lastSeen; // 最后一次通信时间
} PeerInfo;

// 频道信息结构体
typedef struct {
    uint8_t channelId;     // 频道ID (1-8)
    uint8_t peerCount;     // 频道内对等设备数量
    uint8_t peerIds[MAX_PEERS]; // 频道内对等设备ID列表
    bool active;           // 频道是否活动
} ChannelInfo;

// 环形缓冲区结构，用于音频数据存储
typedef struct {
    int16_t *buffer;        // 缓冲区数据指针
    int size;               // 缓冲区大小
    int readIndex;          // 读取索引
    int writeIndex;         // 写入索引
    int available;          // 可用数据量
    bool overflow;          // 溢出标志
    SemaphoreHandle_t mutex;// 互斥锁保护缓冲区访问
} RingBuffer;

// 为每个对等设备创建单独的缓冲区
typedef struct {
    PeerInfo info;
    RingBuffer audioBuffer;
    float volume;           // 每个设备单独音量控制
    bool muted;             // 静音控制
} PeerWithAudio;

/**
 * 初始化频道系统
 * 
 * @return 初始化是否成功
 */
bool wireless_setup_channels();

/**
 * 加入特定频道
 * 
 * @param channelId 频道ID (1-8)
 * @return 加入是否成功
 */
bool wireless_join_channel(uint8_t channelId);

/**
 * 离开当前频道
 * 
 * @return 操作是否成功
 */
bool wireless_leave_channel();

/**
 * 获取当前频道内的对等设备列表
 * 
 * @param peers 输出的对等设备数组
 * @param maxPeers 最大返回数量
 * @return 实际对等设备数量
 */
int wireless_get_peers_in_channel(PeerInfo* peers, int maxPeers);

/**
 * 向当前频道发送音频数据
 * 
 * @param audio_data 音频数据
 * @param samples 样本数
 * @return 是否成功发送到所有对等设备
 */
bool wireless_send_to_channel(int16_t* audio_data, size_t samples);

/**
 * 向特定设备发送音频数据
 * 
 * @param targetId 目标设备ID，0xFF表示广播给所有设备
 * @param audio_data 音频数据
 * @param samples 样本数
 * @return 是否成功发送
 */
bool wireless_send_to_peer(uint8_t targetId, int16_t* audio_data, size_t samples);

/**
 * 扫描并更新当前频道中的对等设备列表
 */
void wireless_scan_peers();

/**
 * 获取当前活跃的频道列表
 * 
 * @param channels 输出的频道信息数组
 * @param maxChannels 最大返回数量
 * @return 实际活跃频道数量
 */
int wireless_get_active_channels(ChannelInfo* channels, int maxChannels);

/**
 * 获取当前频道信息
 * 
 * @return 当前频道ID，0表示未加入任何频道
 */
uint8_t wireless_get_current_channel_id();

/**
 * 将多个对等设备的音频混合到一个输出缓冲区
 * 
 * @param outputBuffer 输出音频缓冲区
 * @param sampleCount 样本数量
 */
void mixAudioFromPeers(int16_t* outputBuffer, int sampleCount);

/**
 * 调整特定对等设备的音量
 * 
 * @param deviceId 设备ID
 * @param volume 音量值(0.0-3.0)
 * @return 操作是否成功
 */
bool setPeerVolume(uint8_t deviceId, float volume);

/**
 * 设置特定对等设备的静音状态
 * 
 * @param deviceId 设备ID
 * @param muted 是否静音
 * @return 操作是否成功
 */
bool setPeerMuted(uint8_t deviceId, bool muted);

/**
 * 诊断ESP-NOW和WiFi状态
 * 
 * 打印所有可能影响连接的关键状态
 */
void wireless_diagnostic();

#endif /* WIRELESS_SYSTEM_H */
