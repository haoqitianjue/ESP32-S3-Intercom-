/**
 * ESP32-S3 无线音频传输系统 - 无线通信模块 (wireless_system.cpp)
 * 
 * 【文件说明】
 * 本文件实现ESP32-S3的ESP-NOW无线通信系统，负责设备配对、音频数据传输、
 * 频道管理和多用户会话功能。是系统的核心通信组件。
 * 
 * 【主要功能】
 * 1. ESP-NOW初始化与管理 - 配置ESP-NOW协议，处理点对点通信
 * 2. 音频数据传输 - 发送和接收音频数据包，支持序列号和丢包检测
 * 3. 频道管理系统 - 实现8个频道的创建、加入和离开
 * 4. 多用户会话 - 管理频道内的多个对等设备，支持音频混合
 * 5. MAC地址管理 - 动态更新MAC地址，提高连接稳定性和安全性
 * 6. 环形缓冲区 - 高效管理音频数据流，减少延迟和抖动
 * 
 * 【重要函数位置】
 * - wireless_init()         [行476] - 初始化无线系统
 * - wireless_process()      [行681] - 无线系统主处理函数
 * - wireless_send_audio()   [行570] - 发送音频数据
 * - wireless_get_audio()    [行653] - 获取接收的音频数据
 * - wireless_join_channel() [行1550] - 加入特定频道
 * - wireless_leave_channel()[行1609] - 离开当前频道
 * - mixAudioFromPeers()     [行1858] - 混合多个对等设备的音频
 * - onDataReceived()        [行1174] - ESP-NOW数据接收回调函数
 * - initRingBuffer()        [行1411] - 初始化环形缓冲区
 * - initPeerDevices()       [行1935] - 初始化对等设备列表
 * - generateRandomMAC()     [行152] - 生成随机MAC地址
 * - updateMacAddress()      [行200] - 更新设备MAC地址
 * 
 * 【实现原理】
 * 1. ESP-NOW通信机制:
 *    基于Wi-Fi的轻量级点对点通信协议，无需连接即可传输数据，
 *    适合低延迟、小数据量的应用场景，如音频传输。
 * 
 * 2. 频道管理系统:
 *    - 每个频道(1-8)使用独立的通信空间
 *    - 基于MAC地址编码实现频道隔离(MAC地址第5字节存储频道信息)
 *    - 动态维护频道内设备列表(peerList)
 * 
 * 3. 音频数据传输:
 *    - 将音频分片打包为AudioPacket结构体
 *    - 添加序列号和设备ID信息
 *    - 通过ESP-NOW发送，接收方重组并处理
 * 
 * 4. 环形缓冲区与线程安全:
 *    - 使用RingBuffer结构存储音频数据
 *    - 采用互斥锁保护共享访问
 *    - 处理溢出和下溢情况，确保音频连续性
 * 
 * 5. 多用户音频处理:
 *    - 每个对等设备有独立的音频缓冲区(PeerWithAudio)
 *    - mixAudioFromPeers函数智能混合多路音频
 *    - 支持独立的音量控制和静音功能
 * 
 * 本模块通过创新的动态MAC管理和序列号处理机制，解决了ESP-NOW的连接不稳定问题，
 * 显著提高了系统在复杂环境下的可靠性和音频质量。
 */

/*
 * ESP32-S3无线音频传输 - 无线通信模块
 * 
 * 实现ESP-NOW点对点低延迟音频传输
 * 解决了以下问题:
 * 1. ESP-NOW连接不稳定问题
 * 2. 远距离通信干扰问题
 * 3. 多设备网络协调问题
 * 4. 通道选择与优化
 */

#include "wireless_system.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>  // 引入Preferences库，用于存储信道配置
#include <driver/i2s.h>
#include <esp_random.h>  // 用于生成随机MAC地址
#include <esp_wifi.h>    // 用于设置MAC地址的底层API

//============================ 全局对象实例 ============================
// 创建Preferences对象，用于持久化存储
static Preferences preferences;

//============================ 常量定义 ============================
// 动态MAC地址存储
uint8_t myMacAddressBytes[6] = {0};  // 本机MAC地址(字节形式)
uint8_t originalMacAddress[6] = {0}; // 原始MAC地址(备份用)
uint8_t peerMacAddressBytes[6] = {0}; // 对等设备MAC地址(字节形式)

// 特殊数据包类型标识
const uint16_t MSG_CONNECT_REQUEST = 0xFFFF;  // 连接请求标识
const uint16_t MSG_CONNECT_CONFIRM = 0xFFFE;  // 连接确认标识
const uint16_t MSG_HEARTBEAT = 0xFFFD;        // 心跳包标识

//============================ 数据结构定义 ============================
// 音频数据包结构 - 紧凑设计，优化大小
typedef struct __attribute__((packed)) {
    uint8_t deviceId;           // 设备ID，用于识别发送者
    uint16_t sequenceNum;       // 序列号，用于检测丢包
    uint8_t dataSize;           // 数据大小（样本数）
    int16_t audioData[RAW_SAMPLES_PER_PACKET]; // 原始音频数据（16位PCM）
} AudioPacket;

// 探测数据包结构，用于通道选择
typedef struct {
    uint8_t type;       // 0xFA表示探测数据包
    uint8_t deviceId;   // 发送设备的ID
    uint16_t sequence;  // 序列号，避免重复处理
    uint8_t checksum;   // 简单校验和
} ProbePacket;

// 探测响应数据包结构
typedef struct {
    uint8_t type;       // 0xFB表示探测响应
    uint8_t deviceId;   // 发送设备的ID
    uint8_t targetId;   // 目标设备ID
    uint8_t checksum;   // 简单校验和
} ProbeResponse;



//============================ 全局变量定义 ============================
// 设备相关
uint8_t deviceId = 1;                // 本设备ID
uint8_t peerAddress[6];              // 对等设备MAC地址
String myMacAddress = "";            // 本机MAC地址
int currentChannel = DEFAULT_WIFI_CHANNEL; // 当前WiFi通道
volatile bool connectionDetected = false;  // 通道连接检测标志

// 统计信息
uint32_t sentPacketsCount = 0;       // 发送的数据包计数
uint32_t receivedPacketsCount = 0;   // 接收的数据包计数
uint32_t packetLossCount = 0;        // 丢包计数
unsigned long lastReceivedTime = 0;  // 上次接收数据的时间
unsigned long lastSentTime = 0;      // 上次发送数据的时间
unsigned long macChangeTimes = 0;    // MAC地址变更次数

// 连接状态
bool connectionEstablished = false;  // 连接建立标志

// 环形缓冲区
RingBuffer rxBuffer;                 // 接收音频环形缓冲区
int16_t* rxBufferData = NULL;        // 接收缓冲区数据

// 音频缓冲区
BUFFER_ATTR int16_t outgoingAudioBuffer[BLOCK_SIZE*2]; // 发送缓冲区

// 序列号
uint16_t sequenceNumber = 0;         // 发送数据包序列号
uint16_t expectedSequence = 0;       // 期望接收的序列号
static uint16_t probeSequence = 0;   // 探测包序列号

// 错误统计
int sendErrorCount = 0;              // 发送错误计数
int consecutiveErrors = 0;           // 连续错误计数

// 频道和多人会话相关变量
static PeerInfo peerList[MAX_PEERS];             // 对等设备列表
static ChannelInfo channelList[MAX_CHANNELS];    // 频道列表
static uint8_t currentChannelId = 0;             // 当前频道ID，0表示未加入任何频道
static uint8_t lastChannelPeerCount = 0;         // 上次检测到的频道内对等设备数量
static unsigned long lastPeerScanTime = 0;       // 上次扫描对等设备的时间

// 为每个对等设备创建单独的缓冲区
PeerWithAudio peerDevices[MAX_PEERS];

//============================ 函数声明 ============================
// MAC地址管理函数
void generateRandomMAC(uint8_t* mac, bool isLocal);
bool updateMacAddress(bool isChannelChange = false);
bool resetToOriginalMac();

// 辅助工具函数
bool isEmptyAddr(const uint8_t* addr);
bool ensure_espnow_initialized();

// 环形缓冲区函数
void initRingBuffer(RingBuffer *rb, int size);
bool writeToRingBuffer(RingBuffer *rb, int16_t *data, int len);
int readFromRingBuffer(RingBuffer *rb, int16_t *data, int maxLen);
int getRingBufferAvailable(RingBuffer *rb);
void freeRingBuffer(RingBuffer *rb);

// 连接管理函数
void sendConnectionRequest();
void sendConnectionConfirm(const uint8_t *mac_addr);
bool sendHeartbeat();

// 通道选择与管理函数
int findWorkingChannel();
bool tryChannelConnection(int channel, int timeout);
void sendProbePacket();
void handleProbePacket(const uint8_t* data, int len);
void sendProbeResponse(uint8_t targetId);

// 工具函数
String macToString(const uint8_t* mac);
bool compareMAC(const uint8_t* mac1, const uint8_t* mac2);

// ESP-NOW回调函数
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void onDataReceived(const uint8_t *mac_addr, const uint8_t *data, int data_len);

// 混音函数 - 将多个设备的音频混合在一起
void mixAudioFromPeers(int16_t* outputBuffer, int sampleCount);

// 对等设备初始化
void initPeerDevices();

//============================ MAC地址管理函数 ============================

/**
 * 生成随机MAC地址
 * 
 * 就像间谍的假身份证生成器，可以生成符合标准的随机MAC地址
 * 
 * @param mac 输出MAC地址数组
 * @param isLocal 是否设置为本地管理的MAC (一般设为true)
 */
void generateRandomMAC(uint8_t* mac, bool isLocal) {
    // 使用ESP32硬件随机数生成器 - 这比软件随机数更"随机"！
    uint32_t randVal = esp_random();
    
    // 首先复制原始MAC作为基础，这有助于保持设备特性
    memcpy(mac, originalMacAddress, 6);
    
    // 修改MAC地址，但保留前三个字节，这些通常是厂商标识位
    // 仅修改后三个字节，既可保证合法性，又有足够的变化
    mac[3] = isLocal ? deviceId : (uint8_t)(randVal & 0xFF);
    mac[4] = isLocal ? currentChannel : (uint8_t)((randVal >> 8) & 0xFF);
    mac[5] = (uint8_t)((randVal >> 16) & 0xFF);
    
    // 设置MAC地址类型位
    if(isLocal) {
        mac[0] |= 0x02;  // 设置本地管理位 - 表示"这是私有地址，不是厂商分配的"
        mac[0] &= 0xFE;  // 清除组播位 - 确保不是广播地址
    } else {
        mac[0] &= 0xFE;  // 清除组播位
    }
    
    // 确保MAC地址有效
    if(mac[0] == 0x00 && mac[1] == 0x00 && mac[2] == 0x00) {
        mac[0] = originalMacAddress[0];  // 恢复原始MAC地址第一个字节
        mac[1] = originalMacAddress[1];  // 恢复原始MAC地址第二个字节
        mac[2] = originalMacAddress[2];  // 恢复原始MAC地址第三个字节
    }
    
    // 确保不是全0或全F
    bool allZeros = true;
    bool allOnes = true;
    for(int i = 0; i < 6; i++) {
        if(mac[i] != 0x00) allZeros = false;
        if(mac[i] != 0xFF) allOnes = false;
    }
    
    if(allZeros || allOnes) {
        // 如果是全0或全F，使用设备ID作为最后一个字节
        mac[5] = deviceId | 0x01;  // 确保最后一位是1
    }
    
    // 避免使用ESP32保留的MAC地址范围
    // 注意：ESP32可能对某些MAC地址范围有限制
    if(mac[0] == 0x18 && mac[1] == 0xFE && mac[2] == 0x34) {
        mac[0] = originalMacAddress[0];
        mac[1] = originalMacAddress[1];
        mac[2] = originalMacAddress[2];
    }
}

/**
 * 更新MAC地址
 * 
 * 真正的"变脸大法"，不仅生成新身份，还全方位更新系统状态
 * 
 * @param isChannelChange 是否因为频道变更而更新MAC
 * @return 是否更新成功
 */
bool updateMacAddress(bool isChannelChange) {
    uint8_t newMac[6];
    
    // 根据不同情况生成MAC地址
    if (isChannelChange) {
        // 频道变更 - 在MAC中编码设备ID和频道信息
        generateRandomMAC(newMac, true);
        
        // 将设备信息编码到MAC地址中，便于识别
        newMac[3] = deviceId;           // 第4字节存设备ID
        newMac[4] = currentChannel;     // 第5字节存频道号
        // 第6字节保持随机，增加唯一性
    } else {
        // 普通随机MAC
        generateRandomMAC(newMac, true);
    }
    
    // 暂时关闭WiFi - 就像换装前先回更衣室一样
    WiFi.mode(WIFI_OFF);
    delay(20);  // 增加延迟时间
    
    // 设置新MAC地址 - 这才是真正"换身份证"的操作！
    esp_err_t result = esp_wifi_set_mac(WIFI_IF_STA, newMac);
    if (result != ESP_OK) {
        // 详细记录错误类型
        if (result == 12289) { // ESP_ERR_WIFI_MAC_FAIL
            Serial.println("❌ MAC地址更新失败: ESP_ERR_WIFI_MAC_FAIL (可能是MAC地址格式不正确或硬件限制)");
            
            // 尝试使用不同方式生成MAC
            Serial.println("🔄 尝试使用备用方法生成MAC地址...");
            uint8_t altMac[6];
            memcpy(altMac, originalMacAddress, 6);
            
            // 修改最后两个字节，保持前面兼容WiFi规范
            altMac[4] = currentChannel;
            altMac[5] = (deviceId << 4) | (random(0, 16) & 0x0F);
            
            // 再次尝试设置MAC
            result = esp_wifi_set_mac(WIFI_IF_STA, altMac);
            if (result != ESP_OK) {
                Serial.printf("❌ 备用MAC地址也更新失败，错误码: %d\n", result);
                // 失败了就还原WiFi，不要留在断开状态
                WiFi.mode(WIFI_STA);
                return false;
            } else {
                // 成功使用备用方法，更新MAC记录
                memcpy(newMac, altMac, 6);
            }
        } else {
            Serial.printf("❌ MAC地址更新失败，错误码: %d\n", result);
            // 失败了就还原WiFi，不要留在断开状态
            WiFi.mode(WIFI_STA);
            return false;
        }
    }
    
    // 重新启用WiFi - 穿好新衣服，重新出场！
    WiFi.mode(WIFI_STA);
    delay(20);  // 增加延迟时间
    
    // 验证MAC是否真的改变了
    uint8_t checkMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, checkMac);
    
    if (memcmp(newMac, checkMac, 6) != 0) {
        Serial.println("❌ MAC地址验证失败，设置后与预期不符");
        return false;
    }
    
    // 保存新MAC地址
    memcpy(myMacAddressBytes, newMac, 6);
    myMacAddress = macToString(newMac);
    macChangeTimes++;
    
    Serial.printf("✅ MAC地址已更新为: %s\n", myMacAddress.c_str());
    
    // ESP-NOW在MAC变化后需要重新初始化
    esp_now_deinit();
    delay(20);  // 增加延迟时间
    
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ MAC变更后ESP-NOW重新初始化失败");
        return false;
    }
    
    // 重新注册回调
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataReceived);
    
    // 重新添加对等设备
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    
    memcpy(peerInfo.peer_addr, peerAddress, 6);
    peerInfo.channel = currentChannel;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("❌ MAC变更后对等设备重新添加失败");
        return false;
    }
    
    return true;
}

/**
 * 恢复到原始MAC地址
 * 
 * 间谍任务结束，恢复真实身份
 * 
 * @return 是否恢复成功
 */
bool resetToOriginalMac() {
    // 暂时关闭WiFi
    WiFi.mode(WIFI_OFF);
    delay(10);
    
    // 恢复原始MAC
    esp_err_t result = esp_wifi_set_mac(WIFI_IF_STA, originalMacAddress);
    
    // 重新启用WiFi
    WiFi.mode(WIFI_STA);
    delay(10);
    
    if (result != ESP_OK) {
        Serial.printf("❌ 恢复原始MAC失败，错误码: %d\n", result);
        return false;
    }
    
    // 更新当前MAC记录
    memcpy(myMacAddressBytes, originalMacAddress, 6);
    myMacAddress = macToString(originalMacAddress);
    
    Serial.printf("✅ 已恢复原始MAC地址: %s\n", myMacAddress.c_str());
    
    // ESP-NOW重新初始化
    esp_now_deinit();
    delay(10);
    
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ MAC恢复后ESP-NOW重新初始化失败");
        return false;
    }
    
    // 重新注册回调
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataReceived);
    
    // 重新添加对等设备
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    
    memcpy(peerInfo.peer_addr, peerAddress, 6);
    peerInfo.channel = currentChannel;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("❌ MAC恢复后对等设备重新添加失败");
        return false;
    }
    
    return true;
}

//============================ 初始化函数 ============================

/**
 * 初始化无线系统
 * 
 * 就像给你的电子设备全面"体检"并做好通信准备
 * 
 * @param devId 设备ID
 * @return 初始化是否成功
 */
bool wireless_init(uint8_t devId) {
    // 初始化Preferences用于存储通道信息
    preferences.begin("wireless", false);
    
    // 初始化接收缓冲区 - 开辟一块内存空间存放收到的语音
    rxBufferData = (int16_t*)heap_caps_malloc(RX_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (rxBufferData == NULL) {
        Serial.println("❌ 内存分配失败，无法创建接收缓冲区");
        return false;
    }
    initRingBuffer(&rxBuffer, RX_BUFFER_SIZE);
    
    Serial.println("\n===== 🚀 开始初始化ESP-NOW无线系统 =====");
    
    // 1. 彻底重置WiFi - 就像重启电脑一样，从头开始
    WiFi.disconnect(true);  // 断开所有连接并清除保存的网络
    WiFi.mode(WIFI_OFF);    // 完全关闭WiFi
    delay(50);              // 给WiFi一些时间完全关闭
    
    // 2. 设置为STA模式
    WiFi.mode(WIFI_STA);    // 设置为Station模式
    WiFi.disconnect();      // 确保不会连接到任何AP
    delay(50);
    
    // 3. 获取并保存原始MAC地址 - 记住自己的"真实身份"
    esp_wifi_get_mac(WIFI_IF_STA, originalMacAddress);
    String origMacStr = macToString(originalMacAddress);
    Serial.println("📝 设备原始MAC地址: " + origMacStr);
    
    // 4. 生成随机MAC地址
    uint8_t randomMac[6];
    generateRandomMAC(randomMac, true);
    
    // 5. 应用随机MAC地址 - 换上"新身份证"
    esp_wifi_set_mac(WIFI_IF_STA, randomMac);
    delay(10);
    
    // 6. 验证MAC地址是否变更成功
    esp_wifi_get_mac(WIFI_IF_STA, myMacAddressBytes);
    myMacAddress = macToString(myMacAddressBytes);
    Serial.println("🆕 已设置随机MAC地址: " + myMacAddress);
    
    // 7. 设备ID初始化
    deviceId = devId;
    Serial.printf("🏷️ 设备ID: %d\n", deviceId);
    
    // 8. 配置WiFi参数 - 优化无线传输参数
    WiFi.setSleep(WIFI_PS_NONE);      // 禁用WiFi省电模式，让ESP32保持"警醒"状态
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // 使用最高可用功率，增加传输距离
    WiFi.setAutoReconnect(false);     // 禁用自动连接，减少干扰
    
    // 9. 选择并设置最佳WiFi通道
    int workingChannel = findWorkingChannel();
    WiFi.channel(workingChannel);
    currentChannel = workingChannel;
    Serial.printf("📶 使用通道: %d 进行通信\n", currentChannel);
    delay(20);
    
    // 10. 初始化ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW初始化失败");
        return false;
    }
    Serial.println("✅ ESP-NOW初始化成功");
    
    // 11. 注册回调函数 - 设置信息发送和接收的"处理器"
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataReceived);
    
    // 12. 添加对等设备 - 就像添加通讯录联系人
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, peerAddress, 6);
    peerInfo.channel = currentChannel;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("❌ 添加对等设备失败");
        return false;
    }
    Serial.println("✅ 对等设备添加成功");
    
    // 13. 连接状态初始化
    connectionEstablished = false;
    
    // 14. 主动发起连接请求
    if (deviceId == 1) {  // 设备1主动发起连接
        sendConnectionRequest();
    }
    
    Serial.println("===== 🎉 ESP-NOW无线系统初始化完成 =====\n");
    return true;
}

/**
 * 寻找最佳WiFi通道
 * 
 * 就像寻找最空旷的高速公路车道
 * 
 * @return 找到的工作通道号
 */
int findWorkingChannel() {
    // 预定义通道列表（标准非重叠通道）- WiFi的"快车道"
    const int channels[] = {1, 6, 11};
    const int numChannels = 3;
    
    // 从存储中读取上一次成功的通道
    int lastChannel = preferences.getInt("lastChannel", channels[0]);
    Serial.printf("📝 上次使用的通道: %d\n", lastChannel);
    
    // 首先尝试上次成功的通道 - 先走熟悉的路
    if (tryChannelConnection(lastChannel, 1500)) {
        Serial.printf("✅ 在上次使用的通道%d上找到设备\n", lastChannel);
        return lastChannel;
    }
    
    // 如果上次通道失败，按顺序尝试所有预定义通道
    Serial.println("🔄 尝试其他标准通道...");
    for (int i = 0; i < numChannels; i++) {
        int channel = channels[i];
        if (channel == lastChannel) continue; // 跳过已经尝试过的通道
        
        if (tryChannelConnection(channel, 1500)) {
            Serial.printf("✅ 在通道%d上找到设备\n", channel);
            // 保存成功的通道以便下次使用
            preferences.putInt("lastChannel", channel);
            return channel;
        }
    }
    
    // 如果所有通道都没有找到设备，选择默认通道
    Serial.println("📢 未在任何通道上找到设备，使用默认通道并等待连接");
    int defaultChannel = channels[0]; // 使用通道1作为默认值
    
    // 存储选择的默认通道
    preferences.putInt("lastChannel", defaultChannel);
    return defaultChannel;
}

/**
 * 尝试在特定通道上连接
 * 
 * 就像在某个频率上呼叫，看看有没有人应答
 * 
 * @param channel 要尝试的通道
 * @param timeout 等待超时时间(毫秒)
 * @return 是否在该通道上找到设备
 */
bool tryChannelConnection(int channel, int timeout) {
    Serial.printf("🔍 尝试在通道%d上寻找设备...\n", channel);
    
    // 切换到指定通道，并更新MAC地址
    currentChannel = channel;
    WiFi.channel(currentChannel);
    
    // 这里我们添加动态MAC更新！
    updateMacAddress(true);  // true表示这是频道变更引起的MAC更新
    
    delay(50); // 给WiFi一些时间完成通道切换
    
    // 重置连接状态标志
    connectionDetected = false;
    
    // 发送探测包 - 就像在黑暗中喊"有人吗？"
    sendProbePacket();
    
    // 等待响应，超时时间内检查
    unsigned long startTime = millis();
    while (millis() - startTime < timeout) {
        // 在此期间，任何接收到的数据包都会通过回调设置connectionDetected标志
        delay(10);
        
        // 定期重发探测包
        if ((millis() - startTime) % 300 == 0) {
            sendProbePacket();
        }
        
        // 检查是否收到响应
        if (connectionDetected) {
            Serial.printf("✅ 在通道%d上检测到响应!\n", channel);
            return true;
        }
    }
    
    Serial.printf("❌ 通道%d上未检测到设备，超时\n", channel);
    return false;
}

/**
 * 设置WiFi通道
 * 
 * 就像调整收音机频率，找到清晰的通信频道
 * 
 * @param channel 要设置的通道号(1-13)
 * @return 设置是否成功
 */
bool wireless_set_channel(int channel) {
    if (channel < 1 || channel > 13) {
        Serial.println("❌ 无效的通道号，必须在1-13之间");
        return false;
    }
    
    Serial.printf("🔄 切换到WiFi通道 %d...\n", channel);
    
    // 保存旧通道，以便出错时回退
    int oldChannel = currentChannel;
    
    // 更新通道
    currentChannel = channel;
    
    // 设置WiFi通道
    WiFi.channel(currentChannel);
    
    // 尝试更新MAC地址
    bool macUpdateResult = updateMacAddress(true);  // true表示这是频道变更
    
    // 如果MAC更新失败，但通道设置成功
    if (!macUpdateResult) {
        Serial.println("⚠️ MAC地址更新失败，但尝试继续通道切换");
        
        // 尝试替代方法：重新初始化ESP-NOW而不改变MAC地址
        esp_now_deinit();
        delay(50);
        
        if (esp_now_init() != ESP_OK) {
            Serial.println("❌ ESP-NOW重新初始化失败，回退到原通道");
            // 切换失败，恢复旧通道
            currentChannel = oldChannel;
            WiFi.channel(oldChannel);
            return false;
        }
        
        // 重新注册回调
        esp_now_register_send_cb(onDataSent);
        esp_now_register_recv_cb(onDataReceived);
        
        // 重新添加对等设备
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        
        memcpy(peerInfo.peer_addr, peerAddress, 6);
        peerInfo.channel = currentChannel;  // 使用新通道
        peerInfo.encrypt = false;
        
        // 先删除旧的对等设备，确保干净地添加新设备
        esp_now_del_peer(peerAddress);
        
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("❌ 对等设备重新添加失败，回退到原通道");
            currentChannel = oldChannel;
            WiFi.channel(oldChannel);
            return false;
        }
        
        // 保存通道设置到Preferences
        preferences.begin("wireless", false);
        preferences.putInt("channel", currentChannel);
        preferences.end();
        
        // 添加ESP-NOW初始化检查
        if (!ensure_espnow_initialized()) {
            Serial.println("❌ ESP-NOW状态检查失败，回退到原通道");
            currentChannel = oldChannel;
            WiFi.channel(oldChannel);
            return false;
        }
        
        Serial.printf("✅ 已切换到通道 %d (MAC地址未更新，但ESP-NOW已重配置)\n", currentChannel);
        return true;
    }
    
    // MAC更新成功，但还需要确保对等设备通道一致
    
    // 检查广播对等设备是否存在
    esp_now_peer_info_t peerInfo;
    bool isPeerExist = esp_now_is_peer_exist(peerAddress);
    
    // 如果存在，检查通道是否与当前通道一致
    if (isPeerExist && esp_now_get_peer(peerAddress, &peerInfo) == ESP_OK) {
        if (peerInfo.channel != currentChannel) {
            // 通道不一致，需要更新
            esp_now_del_peer(peerAddress);
            
            // 重新添加对等设备
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, peerAddress, 6);
            peerInfo.channel = currentChannel;
            peerInfo.encrypt = false;
            
            if (esp_now_add_peer(&peerInfo) != ESP_OK) {
                Serial.println("⚠️ 广播对等设备通道更新失败，但继续使用");
            } else {
                Serial.println("✅ 广播对等设备通道已更新");
            }
        }
    } else {
        // 对等设备不存在，添加新的
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, peerAddress, 6);
        peerInfo.channel = currentChannel;
        peerInfo.encrypt = false;
        
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("⚠️ 广播对等设备添加失败，但继续使用");
        } else {
            Serial.println("✅ 广播对等设备已添加");
        }
    }
    
    // 保存通道设置到Preferences
    preferences.begin("wireless", false);
    preferences.putInt("channel", currentChannel);
    preferences.end();
    
    // 添加ESP-NOW初始化检查
    if (!ensure_espnow_initialized()) {
        Serial.println("⚠️ ESP-NOW状态检查显示未初始化，正在修复...");
        // 已经包含修复机制，继续运行
    }
    
    Serial.printf("✅ 已切换到通道 %d，MAC地址已更新\n", currentChannel);
    return true;
}

/**
 * 发送音频数据 - 分片发送以适应ESP-NOW限制
 *
 * 将音频数据分片发送，每片最多RAW_SAMPLES_PER_PACKET个样本
 * 
 * @param audio_data 音频数据
 * @param samples 样本数
 * @return 是否成功发送
 */
bool wireless_send_audio(int16_t* audio_data, size_t samples) {
    // 参数验证
    if (audio_data == NULL || samples == 0) {
        return false;
    }
    
    // 检查连接状态
    if (!connectionEstablished) {
        static unsigned long lastConnectErrorPrint = 0;
        static unsigned long lastReconnectAttempt = 0;
        unsigned long currentTime = millis();
        
        // 每5秒打印一次错误
        if (currentTime - lastConnectErrorPrint > 5000) {  
            Serial.println("⚠️ ESP-NOW未连接，无法发送音频");
            lastConnectErrorPrint = currentTime;
        }
        
        // 每10秒尝试一次重新连接
        if (currentTime - lastReconnectAttempt > 10000) {
            Serial.println("🔄 尝试重新建立连接");
            sendConnectionRequest();
            lastReconnectAttempt = currentTime;
        }
        
        return false;
    }
    
    // 检查对等设备地址是否有效（非全0和非全FF）
    bool validPeerAddr = false;
    bool allZeros = true;
    bool allOnes = true;
    
    for (int i = 0; i < 6; i++) {
        if (peerAddress[i] != 0) allZeros = false;
        if (peerAddress[i] != 0xFF) allOnes = false;
    }
    
    validPeerAddr = !allZeros && !allOnes;
    
    // 如果对等设备地址无效，尝试重新建立连接
    if (!validPeerAddr) {
        static unsigned long lastAddrErrorPrint = 0;
        if (millis() - lastAddrErrorPrint > 5000) {
            Serial.println("⚠️ 对等设备地址无效，尝试重新建立连接");
            sendConnectionRequest();
            lastAddrErrorPrint = millis();
        }
        return false;
    }
    
    // 确保对等设备已添加到ESP-NOW
    if (!esp_now_is_peer_exist(peerAddress)) {
        Serial.println("⚠️ 对等设备未添加到ESP-NOW，尝试添加");
        
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, peerAddress, 6);
        peerInfo.channel = currentChannel;
        peerInfo.encrypt = false;
        
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("❌ 添加对等设备失败");
            return false;
        }
    }
    
    // 将音频数据复制到发送缓冲区
    int actualSamples = min(samples, (size_t)(BLOCK_SIZE * 2));
    memcpy(outgoingAudioBuffer, audio_data, actualSamples * sizeof(int16_t));
    
    // 分片发送，每次最多发送RAW_SAMPLES_PER_PACKET个样本
    bool anySuccess = false;
    int sentSamples = 0;
    
    while (sentSamples < actualSamples) {
        // 控制发送频率
        static unsigned long lastFragmentTime = 0;
        if (millis() - lastFragmentTime < TRANSMIT_INTERVAL ) {
            delay(1); // 短暂延时，让出CPU
            continue;
        }
        
        lastFragmentTime = millis();
        
        // 计算本片要发送的样本数
        int samplesToSend = min(RAW_SAMPLES_PER_PACKET, actualSamples - sentSamples);
        
        // 创建音频包
        AudioPacket packet;
        packet.deviceId = deviceId;
        packet.sequenceNum = sequenceNumber++;
        packet.dataSize = samplesToSend;
        
        // 复制音频数据到包
        memcpy(packet.audioData, &outgoingAudioBuffer[sentSamples], samplesToSend * sizeof(int16_t));
        
        // 计算要发送的实际字节数
        int packetSize = 4 + samplesToSend * sizeof(int16_t); // 4字节头部 + 音频数据
        
        // 发送数据包
        esp_err_t result = esp_now_send(peerAddress, (uint8_t*)&packet, packetSize);
        if (result == ESP_OK) {
            sentPacketsCount++;
            anySuccess = true;
            sentSamples += samplesToSend;
            lastSentTime = millis();
            consecutiveErrors = 0;
        } else {
            sendErrorCount++;
            consecutiveErrors++;
            
            // 如果连续多次失败，打印错误并返回
            if (consecutiveErrors > 10) {
                Serial.printf("❌ 发送音频失败，错误码: %d，连续错误: %d\n", result, consecutiveErrors);
                
                // 连续错误过多，可能连接已断开
                if (consecutiveErrors > 20) {
                    connectionEstablished = false;
                }
                
                return false;
            }
            
            // 短暂延时后重试
            delay(2);
        }
    }
    
    return anySuccess;
}

/**
 * 获取接收的音频数据
 * 
 * 从接收缓冲区中读取音频数据
 * 
 * @param buffer 目标缓冲区
 * @param max_samples 最大样本数
 * @return 实际读取的样本数
 */
int wireless_get_audio(int16_t* buffer, size_t max_samples) {
    if (buffer == NULL || max_samples == 0) {
        return 0;
    }
         
    // 从环形缓冲区读取数据
    int samplesRead = readFromRingBuffer(&rxBuffer, buffer, max_samples);

    // 调试信息
    static unsigned long lastReadDebug = 0;
    static int totalReadSamples = 0;
    
    totalReadSamples += samplesRead;
    
    if (millis() - lastReadDebug > 5000 && totalReadSamples > 0) {
        Serial.printf("📊 已读取音频: %d 样本\n", totalReadSamples);
        lastReadDebug = millis();
        totalReadSamples = 0;
    }
    
    return samplesRead;
}

/**
 * 处理无线系统状态
 * 
 * 维护连接状态，定期发送心跳包等
 */
void wireless_process() {
    static bool sentInitialPacket = false;
    static unsigned long lastHeartbeatTime = 0;
    static unsigned long lastConnectionCheckTime = 0;
    static unsigned long lastMacUpdateTime = 0;
    static unsigned long lastEspNowCheckTime = 0;
    
    // 启动时发送初始连接包
    if (!sentInitialPacket && deviceId == 1) {
        sendConnectionRequest();
        sentInitialPacket = true;
    }
    
    // 周期性检查ESP-NOW状态 (每10秒)
    if (millis() - lastEspNowCheckTime > 10000) {
        ensure_espnow_initialized();
        lastEspNowCheckTime = millis();
    }
    
    // 发送心跳包保持连接
    if (connectionEstablished && millis() - lastHeartbeatTime > 3000) {
        sendHeartbeat();
        lastHeartbeatTime = millis();
    }
    
    // 检查连接状态
    if (millis() - lastConnectionCheckTime > 10000) {
        // 如果长时间没有收到数据，认为连接已断开
        if (connectionEstablished && millis() - lastReceivedTime > 30000) {
            Serial.println("⚠️ 长时间未收到数据，连接可能已断开");
            connectionEstablished = false;
        }
        
        // 如果未连接，尝试重新建立连接
        if (!connectionEstablished && deviceId == 1) {
            Serial.println("🔄 尝试重新建立连接");
            sendConnectionRequest();
        }
        
        lastConnectionCheckTime = millis();
    }
    
    // 定期更新MAC地址 - 就像间谍定期换身份一样
    if (millis() - lastMacUpdateTime > 300000) {  // 每5分钟更新一次
        // 只有在连接状态下才随机更新，避免通信中断
        if (connectionEstablished) {
            updateMacAddress(false); // false表示不是频道变更，只是常规更新
        }
        lastMacUpdateTime = millis();
    }
}

/**
 * 寻找最佳WiFi通道
 * 
 * 扫描周围环境，分析各通道的干扰情况，选择最佳通道
 * 
 * @return 最佳通道号
 */
int wireless_find_best_channel() {
  Serial.println("🔍 正在自动扫描最佳WiFi通道...");
  
  // 标准非重叠通道，这些通道是WiFi设计中互不干扰的频段
  const int PRIMARY_CHANNELS[] = {1, 6, 11};
  
  // 存储每个通道的干扰评分（分数越低越好）
  int channelScores[14] = {0};
  
  // 开始WiFi扫描，使用一个合理的超时时间以避免启动过程阻塞太久
  WiFi.scanDelete(); // 清除之前的扫描结果以释放内存
  int networksFound = WiFi.scanNetworks(true, true, false, 200); // 减少扫描时间到200ms
  
  // 等待扫描完成，但设置最长等待时间
  unsigned long scanStartTime = millis();
  while (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    delay(10);
    // 如果扫描时间超过3秒，强制跳出等待
    if (millis() - scanStartTime > 3000) {
      Serial.println("⚠️ 扫描超时，使用默认通道");
      return DEFAULT_WIFI_CHANNEL;
    }
  }
  
  networksFound = WiFi.scanComplete();
  
  if (networksFound > 0) {
    Serial.printf("📡 发现%d个WiFi网络\n", networksFound);
    
    // 分析每个网络对各通道的干扰影响
    for (int i = 0; i < networksFound; i++) {
      int channel = WiFi.channel(i);
      int rssi = WiFi.RSSI(i);
      
      if (channel >= 1 && channel <= 13) {
        // 信号强度转换为干扰分数
        int interference = abs(rssi);
        channelScores[channel] += interference;
        
        // 考虑WiFi通道重叠影响
        for (int adj = channel-2; adj <= channel+2; adj++) {
          if (adj >= 1 && adj <= 13 && adj != channel) {
            // 相邻通道受到的干扰随距离递减
            int factor = (abs(adj-channel) == 1) ? 70 : 30;
            channelScores[adj] += interference * factor / 100;
          }
        }
      }
    }
    
    // 打印通道干扰分析结果，方便调试和观察
    Serial.println("📊 通道干扰分析:");
    for (int ch = 1; ch <= 13; ch++) {
      if (channelScores[ch] > 0) {
        Serial.printf("  通道 %2d: 干扰分数 %5d\n", ch, channelScores[ch]);
      }
    }
    
    // 优先从标准非重叠通道(1、6、11)中选择最佳通道
    int bestChannel = PRIMARY_CHANNELS[0]; // 默认使用通道1
    int lowestScore = channelScores[bestChannel];
    
    for (int i = 0; i < 3; i++) {
      int ch = PRIMARY_CHANNELS[i];
      if (channelScores[ch] < lowestScore) {
        lowestScore = channelScores[ch];
        bestChannel = ch;
      }
    }
    
    // 只有当其它通道明显更好时才考虑使用非标准通道
    for (int ch = 1; ch <= 13; ch++) {
      bool isPrimary = false;
      for (int i = 0; i < 3; i++) {
        if (ch == PRIMARY_CHANNELS[i]) {
          isPrimary = true;
          break;
        }
      }
      
      // 只有非标准通道且明显更好(干扰少30%以上)时才选择
      if (!isPrimary && channelScores[ch] < lowestScore * 0.7) {
        lowestScore = channelScores[ch];
        bestChannel = ch;
        Serial.printf("💡 选择非标准通道%d，因其干扰显著更低\n", ch);
      }
    }
    
    Serial.printf("✅ 自动选择通道 %d 作为最佳通道\n", bestChannel);
    WiFi.scanDelete(); // 释放扫描结果，节省内存
    return bestChannel;
  } else {
    if (networksFound == 0) {
      Serial.println("📝 未发现网络，使用默认通道");
    } else {
      Serial.printf("❌ 扫描错误(代码: %d)，使用默认通道\n", networksFound);
    }
    return DEFAULT_WIFI_CHANNEL;
  }
}

//============================ 诊断和维护函数 ============================

/**
 * 执行连接测试与诊断
 * 
 * 进行一系列测试，输出诊断信息
 */
void wireless_diagnose() {
    Serial.println("\n===== 🔍 ESP-NOW 系统诊断 =====");
    
    // 连接状态检查
    Serial.printf("📡 连接状态: %s\n", connectionEstablished ? "已连接" : "未连接");
    Serial.printf("⏱️ 最后接收时间: %lu ms前\n", millis() - lastReceivedTime);
    Serial.printf("⏱️ 最后发送时间: %lu ms前\n", millis() - lastSentTime);
    
    // 统计信息
    Serial.printf("📊 已发送包数: %u\n", sentPacketsCount);
    Serial.printf("📊 已接收包数: %u\n", receivedPacketsCount);
    Serial.printf("📊 丢包数: %u\n", packetLossCount);
    Serial.printf("📊 发送错误数: %d\n", sendErrorCount);
    Serial.printf("📊 MAC地址变更次数: %lu\n", macChangeTimes);
    
    // WiFi状态
    Serial.printf("📶 当前通道: %d\n", currentChannel);
    Serial.printf("📶 信号强度: %d dBm\n", WiFi.RSSI());
    Serial.printf("📶 当前MAC地址: %s\n", myMacAddress.c_str());
    
    // 发送测试包
    uint8_t testData[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    Serial.println("🚀 发送测试包...");
    
    esp_err_t result = esp_now_send(peerAddress, testData, sizeof(testData));
    Serial.printf("📝 测试包发送结果: %s (错误码: %d)\n", 
                 result == ESP_OK ? "成功" : "失败", result);
    
    // 测试不同包大小
    Serial.println("📏 测试不同包大小:");
    for (int size = 10; size <= 250; size += 60) {
        uint8_t* buf = (uint8_t*)malloc(size);
        if (buf) {
            memset(buf, 0xAA, size);
            result = esp_now_send(peerAddress, buf, size);
            Serial.printf("  包大小 %d: %s\n", size, 
                         result == ESP_OK ? "成功" : "失败");
            free(buf);
            delay(10); // 短暂延时避免发送过快
        }
    }
    
    Serial.println("===== 📝 诊断完成 =====\n");
}

/**
 * 重置无线系统
 * 
 * 完全重置ESP-NOW，清除状态变量
 */
void wireless_reset() {
    Serial.println("🔄 重置无线系统...");
    
    // 卸载ESP-NOW
    esp_now_deinit();
    delay(100);
    
    // 恢复原始MAC地址
    resetToOriginalMac();
    
    // 重置状态变量
    connectionEstablished = false;
    sentPacketsCount = 0;
    receivedPacketsCount = 0;
    packetLossCount = 0;
    sendErrorCount = 0;
    sequenceNumber = 0;
    expectedSequence = 0;
    
    // 重新初始化ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW重新初始化失败");
        return;
    }
    
    // 重新注册回调
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataReceived);
    
    // 重新添加对等设备
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    
    memcpy(peerInfo.peer_addr, peerAddress, 6);
    peerInfo.channel = currentChannel;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("❌ 对等设备重新添加失败");
        return;
    }
    
    // 清空接收缓冲区
    wireless_flush_rx_buffer();
    
    Serial.println("✅ 无线系统已重置");
}

/**
 * 清空接收缓冲区
 * 
 * 重置缓冲区状态，丢弃所有数据
 */
void wireless_flush_rx_buffer() {
    if (rxBuffer.mutex != NULL) {
        if (xSemaphoreTake(rxBuffer.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            rxBuffer.readIndex = 0;
            rxBuffer.writeIndex = 0;
            rxBuffer.available = 0;
            rxBuffer.overflow = false;
            
            xSemaphoreGive(rxBuffer.mutex);
        }
    }
}

/**
 * 获取目标设备的MAC地址
 * 
 * @return MAC地址字符串
 */
String wireless_get_peer_mac() {
    return macToString(peerAddress);
}

/**
 * 获取本机MAC地址
 * 
 * @return MAC地址字符串
 */
String wireless_get_my_mac() {
    return myMacAddress;
}

/**
 * 获取当前通道
 * 
 * @return 当前通道号
 */
int wireless_get_channel() {
    return currentChannel;
}

/**
 * 获取统计信息
 * 
 * @param sent 已发送数据包数
 * @param received 已接收数据包数
 * @param lost 丢包数
 */
void wireless_get_stats(uint32_t* sent, uint32_t* received, uint32_t* lost) {
    if (sent) *sent = sentPacketsCount;
    if (received) *received = receivedPacketsCount;
    if (lost) *lost = packetLossCount;
}

/**
 * 获取连接状态
 * 
 * @return 是否已建立连接
 */
bool wireless_is_connected() {
    // 判断连接标志和最近通信时间
    return connectionEstablished && 
           ((millis() - lastReceivedTime < 30000) || (millis() - lastSentTime < 30000));
}

//============================ 探测和通信函数 ============================

/**
 * 发送探测包
 * 
 * 广播特殊的探测包，让其他设备知道我们在此通道上
 */
void sendProbePacket() {
    ProbePacket packet;
    packet.type = 0xFA;
    packet.deviceId = deviceId;
    packet.sequence = probeSequence++;
    packet.checksum = packet.type ^ packet.deviceId ^ (packet.sequence & 0xFF) ^ ((packet.sequence >> 8) & 0xFF);
    
    // 发送探测包 - 使用广播地址
    uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(broadcastAddr, (uint8_t*)&packet, sizeof(packet));
}

/**
 * 处理接收到的探测包
 * 
 * @param data 接收到的数据
 * @param len 数据长度
 */
void handleProbePacket(const uint8_t* data, int len) {
    // 确保数据包长度正确
    if (len != sizeof(ProbePacket)) return;
    
    const ProbePacket* packet = (const ProbePacket*)data;
    
    // 验证这是一个探测包
    if (packet->type != 0xFA) return;
    
    // 验证不是自己发送的
    if (packet->deviceId == deviceId) return;
    
    // 验证校验和
    uint8_t checksum = packet->type ^ packet->deviceId ^ (packet->sequence & 0xFF) ^ ((packet->sequence >> 8) & 0xFF);
    if (checksum != packet->checksum) return;
    
    // 设置连接检测标志
    connectionDetected = true;
    
    // 发送探测响应
    sendProbeResponse(packet->deviceId);
    
    Serial.printf("📡 收到来自设备%d的探测包，已回复\n", packet->deviceId);
}

/**
 * 发送探测响应
 * 
 * @param targetId 目标设备ID
 */
void sendProbeResponse(uint8_t targetId) {
    ProbeResponse response;
    response.type = 0xFB;
    response.deviceId = deviceId;
    response.targetId = targetId;
    response.checksum = response.type ^ response.deviceId ^ response.targetId;
    
    // 发送响应
    esp_now_send(peerAddress, (uint8_t*)&response, sizeof(response));
}

/**
 * 发送连接请求
 * 
 * 发送特殊包通知对方设备建立连接
 */
void sendConnectionRequest() {
    Serial.println("🔄 尝试建立连接...");
    
    // 确保ESP-NOW已初始化
    if (!ensure_espnow_initialized()) {
        Serial.println("❌ ESP-NOW未初始化，无法发送连接请求");
        return;
    }
    
    // 创建连接请求包，使用最小数据大小
    uint8_t packet[4];
    packet[0] = (uint8_t)(MSG_CONNECT_REQUEST & 0xFF);
    packet[1] = (uint8_t)((MSG_CONNECT_REQUEST >> 8) & 0xFF);
    packet[2] = deviceId;
    packet[3] = 0; // 保留字节
    
    // 设置广播地址
    uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    // 确保广播地址已添加为对等设备
    if (!esp_now_is_peer_exist(broadcastAddr)) {
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, broadcastAddr, 6);
        peerInfo.channel = currentChannel;
        peerInfo.encrypt = false;
        
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("⚠️ 添加广播对等设备失败，但尝试继续");
        }
    }
    
    // 确保通道一致
    if (WiFi.channel() != currentChannel) {
        Serial.printf("⚠️ 修正WiFi通道: 当前=%d, 应为=%d\n", WiFi.channel(), currentChannel);
        WiFi.channel(currentChannel);
        delay(10);
    }
    
    // 发送连接请求（多次尝试）
    bool success = false;
    for (int i = 0; i < 3; i++) {
        esp_err_t result = esp_now_send(broadcastAddr, packet, sizeof(packet));
        if (result == ESP_OK) {
            Serial.println("✅ 已发送连接请求");
            lastSentTime = millis();
            success = true;
            break;
        } else {
            Serial.printf("❌ 发送连接请求失败，错误码: %d (尝试 %d/3)\n", result, i+1);
            
            // 如果是内存或通道错误，尝试重新初始化
            if (result == 12393 || result == 12301) { // ESP_ERR_ESPNOW_NO_MEM 或通道错误
                Serial.println("⚠️ 尝试重新初始化ESP-NOW...");
                
                // 重新初始化ESP-NOW
                esp_now_deinit();
                delay(50);
                
                if (esp_now_init() != ESP_OK) {
                    Serial.println("❌ ESP-NOW重新初始化失败");
                    continue;
                }
                
                // 重新注册回调
                esp_now_register_send_cb(onDataSent);
                esp_now_register_recv_cb(onDataReceived);
                
                // 重新添加广播对等设备
                esp_now_peer_info_t peerInfo;
                memset(&peerInfo, 0, sizeof(peerInfo));
                memcpy(peerInfo.peer_addr, broadcastAddr, 6);
                peerInfo.channel = currentChannel;
                peerInfo.encrypt = false;
                
                if (esp_now_add_peer(&peerInfo) != ESP_OK) {
                    Serial.println("❌ 重新添加广播对等设备失败");
                }
            }
            
            delay(50 * (i + 1)); // 递增延迟
        }
    }
    
    if (!success) {
        Serial.println("❌ 所有连接请求尝试均失败");
    }
}

/**
 * 发送连接确认
 * 
 * 响应连接请求，确认连接已建立
 * 
 * @param mac_addr 目标设备MAC地址
 */
void sendConnectionConfirm(const uint8_t *mac_addr) {
    if (!mac_addr) return;
    
    Serial.println("📤 发送连接确认...");
    
    // 创建连接确认包
    uint8_t packet[4]; // 仅需要4字节
    packet[0] = (uint8_t)(MSG_CONNECT_CONFIRM & 0xFF);
    packet[1] = (uint8_t)((MSG_CONNECT_CONFIRM >> 8) & 0xFF);
    packet[2] = deviceId;
    packet[3] = 0; // 保留字节
    
    // 使用传入的MAC地址发送确认包
    int retries = 0;
    esp_err_t result = ESP_FAIL;
    
    while (retries < 5 && result != ESP_OK) {
        // 在每次尝试之前等待一段时间
        if (retries > 0) {
            delay(50 * retries); // 渐进式增加延迟
        }
        
        // 确保对等设备已添加
        if (!esp_now_is_peer_exist(mac_addr)) {
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, mac_addr, 6);
            peerInfo.channel = currentChannel;
            peerInfo.encrypt = false;
            
            esp_now_add_peer(&peerInfo);
        }
        
        // 发送确认包
        result = esp_now_send(mac_addr, packet, sizeof(packet));
        
        if (result != ESP_OK) {
            Serial.printf("❌ 发送连接确认失败，错误码: %d (尝试 %d/5)\n", result, retries + 1);
            retries++;
            
            // 如果是内存不足错误，尝试释放一些内存
            if (result == 12393) { // ESP_ERR_ESPNOW_NO_MEM
                Serial.println("⚠️ 检测到内存不足错误，尝试释放资源...");
                // 强制释放资源
                esp_now_deinit();
                delay(100);
                if (esp_now_init() != ESP_OK) {
                    Serial.println("❌ ESP-NOW重新初始化失败");
                    continue;
                }
                
                // 重新注册回调
                esp_now_register_send_cb(onDataSent);
                esp_now_register_recv_cb(onDataReceived);
                
                // 重新添加对等设备
                if (!esp_now_is_peer_exist(mac_addr)) {
                    esp_now_peer_info_t peerInfo;
                    memset(&peerInfo, 0, sizeof(peerInfo));
                    memcpy(peerInfo.peer_addr, mac_addr, 6);
                    peerInfo.channel = currentChannel;
                    peerInfo.encrypt = false;
                    
                    esp_now_add_peer(&peerInfo);
                }
            }
        } else {
            // 保存对等设备MAC地址，以便后续通信
            memcpy(peerAddress, mac_addr, 6);
            connectionEstablished = true;
            lastReceivedTime = millis();
            Serial.printf("✅ 已与设备ID %d建立连接\n", packet[2]);
            
            // 发送另一个心跳包，确认连接建立
            delay(50);
            sendHeartbeat();
            break;
        }
    }
    
    if (result != ESP_OK) {
        Serial.println("❌ 所有连接确认尝试均失败");
    }
}

/**
 * 发送心跳包
 * 
 * 维持连接，表明设备仍然活跃
 * 
 * @return 发送是否成功
 */
bool sendHeartbeat() {
    // 确保ESP-NOW已初始化
    if (!ensure_espnow_initialized()) {
        Serial.println("❌ ESP-NOW未初始化，无法发送心跳包");
        return false;
    }
    
    // 如果目标地址是全0，可能是未初始化状态
    bool emptyPeerAddr = isEmptyAddr(peerAddress);
    
    // 如果是空地址，使用广播地址
    uint8_t* destAddr = peerAddress;
    if (emptyPeerAddr) {
        static uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        destAddr = broadcastAddr;
        
        // 确保广播地址已添加为对等设备
        if (!esp_now_is_peer_exist(broadcastAddr)) {
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, broadcastAddr, 6);
            peerInfo.channel = currentChannel;
            peerInfo.encrypt = false;
            
            esp_now_add_peer(&peerInfo);
        }
    }
    
    // 创建特殊包，使用最小数据大小
    uint8_t packet[4];
    packet[0] = (uint8_t)(MSG_HEARTBEAT & 0xFF);
    packet[1] = (uint8_t)((MSG_HEARTBEAT >> 8) & 0xFF);
    packet[2] = deviceId;
    packet[3] = 0; // 保留字节
    
    // 确保ESP-NOW已初始化并对等设备已添加
    if (!esp_now_is_peer_exist(destAddr)) {
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, destAddr, 6);
        peerInfo.channel = currentChannel;
        peerInfo.encrypt = false;
        
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("⚠️ 添加心跳对等设备失败");
        }
    }
    
    // 发送心跳包
    esp_err_t result = esp_now_send(destAddr, packet, sizeof(packet));
    
    if (result == ESP_OK) {
        lastSentTime = millis();
        return true;
    } else {
        Serial.printf("⚠️ 发送心跳包失败，错误码: %d\n", result);
        return false;
    }
}

//============================ ESP-NOW回调函数 ============================

/**
 * ESP-NOW发送回调
 * 
 * 处理数据包发送结果
 * 
 * @param mac_addr 目标MAC地址
 * @param status 发送状态
 */
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        sendErrorCount++;
    }
}

/**
 * ESP-NOW接收回调 - 处理接收到的数据包
 * 
 * 此函数处理从其他设备接收到的数据包：
 * - 探测包（用于通道选择）
 * - 控制包（连接请求/确认）
 * - 音频数据包（包含压缩的A-law音频）
 * 
 * @param mac_addr 源MAC地址
 * @param data 数据
 * @param data_len 数据长度
 */
void onDataReceived(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
    // 更新接收时间和计数
    lastReceivedTime = millis();
    receivedPacketsCount++;
    
    // 检查数据包类型，处理探测包
    if (data_len > 0) {
        uint8_t packetType = data[0];
        
        // 处理探测类型的包
        if (packetType == 0xFA) {
            handleProbePacket(data, data_len);
            return;
        }
        
        // 处理探测响应包
        if (packetType == 0xFB) {
            // 收到探测响应，设置标志
            connectionDetected = true;
            return;
        }
    }
    
    // 确保数据包至少有基本头部
    if (data == NULL || data_len < 4) {
        Serial.println("⚠️ 接收到无效数据包");
        return;
    }
    
    // 处理ESP-NOW连接特殊数据包
    if (data_len == 4) {
        uint16_t msgType = data[0] | (data[1] << 8);
        uint8_t senderId = data[2];
        
        // 不处理自己发送的数据包
        if (senderId == deviceId) {
            return;
        }
        
        // 连接请求
        if (msgType == MSG_CONNECT_REQUEST) {
            Serial.printf("🔔 收到设备%d的连接请求\n", senderId);
            
            // 保存对等设备MAC地址，以便后续通信
            memcpy(peerAddress, mac_addr, 6);
            
            // 确保对等设备已添加到ESP-NOW
            if (!esp_now_is_peer_exist(mac_addr)) {
                esp_now_peer_info_t peerInfo;
                memset(&peerInfo, 0, sizeof(peerInfo));
                memcpy(peerInfo.peer_addr, mac_addr, 6);
                peerInfo.channel = currentChannel;
                peerInfo.encrypt = false;
                
                if (esp_now_add_peer(&peerInfo) != ESP_OK) {
                    Serial.println("⚠️ 添加对等设备失败，但尝试继续处理连接请求");
                }
            }
            
            // 延迟一下，确保ESP-NOW处理完成
            delay(50);
            
            // 设置连接状态
            connectionEstablished = true;
            
            // 发送确认包
            sendConnectionConfirm(mac_addr);
            return;
        }
        
        // 连接确认
        if (msgType == MSG_CONNECT_CONFIRM) {
            Serial.printf("✅ 收到设备%d的连接确认\n", senderId);
            
            // 保存对等设备MAC地址
            memcpy(peerAddress, mac_addr, 6);
            
            // 设置连接状态并发送心跳包确认
            connectionEstablished = true;
            
            // 延迟一下，确保ESP-NOW处理完成
            delay(30);
            
            // 发送心跳包确认连接
            sendHeartbeat();
            return;
        }
        
        // 心跳包
        if (msgType == MSG_HEARTBEAT) {
            // 收到心跳包，更新连接状态
            connectionEstablished = true;
            return;
        }
    }
    
    // 音频数据包
    const AudioPacket *packet = (const AudioPacket *)data;
    
    // 验证设备ID - 不处理自己发送的数据包
    if (packet->deviceId == deviceId) {
        return;
    }
    
    // 音频包的序列号检查和丢包处理
    if (packet->sequenceNum != MSG_CONNECT_REQUEST && 
        packet->sequenceNum != MSG_CONNECT_CONFIRM && 
        packet->sequenceNum != MSG_HEARTBEAT) {
        
        if (expectedSequence > 0 && packet->sequenceNum > expectedSequence) {
            int missed = packet->sequenceNum - expectedSequence;
            packetLossCount += missed;
            
            // 大量丢包时可以插入静音数据
            if (missed > 10) {
                // 插入一些静音样本到缓冲区
                int16_t silence[100] = {0};
                writeToRingBuffer(&rxBuffer, silence, 100);
            }
        }
        
        // 更新期望序列号
        expectedSequence = packet->sequenceNum + 1;
    }
    
    // 提取设备ID和频道信息
    uint8_t peerDeviceId = 0;
    uint8_t peerChannelId = 0;
    
    // 从数据包中提取设备ID
    if (data_len >= sizeof(AudioPacket)) {
        peerDeviceId = packet->deviceId;
    }
    
    // 从MAC地址提取频道信息和设备ID（如果数据包中没有）
    if (peerDeviceId == 0 && mac_addr[3] > 0) {
        peerDeviceId = mac_addr[3];
    }
    
    // 从MAC地址提取频道信息
    if (mac_addr[4] > 0 && mac_addr[4] <= MAX_CHANNELS) {
        peerChannelId = mac_addr[4];
    }
    
    // 处理音频数据
    if (packet->dataSize > 0 && packet->dataSize <= RAW_SAMPLES_PER_PACKET) {
        // 表明连接已建立（如果收到了音频数据）
        connectionEstablished = true;
    
        // 创建临时缓冲区用于解压缩的数据
        int16_t decodedAudio[RAW_SAMPLES_PER_PACKET];
        
        // 解压缩A-law编码的音频数据
        for (int i = 0; i < packet->dataSize; i++) {
            // 从16位值中提取低8位作为A-law压缩值
            uint8_t compressedSample = (uint8_t)(packet->audioData[i] & 0xFF);
            // 解压缩为16位PCM
            decodedAudio[i] = alaw2linear(compressedSample);
        }
        
        // 调试信息 - 偶尔打印
        static unsigned long lastDebugPrint = 0;
        static int packetCount = 0;
        
        packetCount++;
        
        if (millis() - lastDebugPrint > 5000) { // 每5秒打印一次
            Serial.printf("🎵 音频接收: 包数=%d, 样本数=%d, 已解压缩A-law\n", 
                         packetCount, packet->dataSize);
            lastDebugPrint = millis();
            packetCount = 0;
        }
        
        // 1. 将解压缩后的音频数据写入公共接收缓冲区（保持向后兼容）
        if (!writeToRingBuffer(&rxBuffer, decodedAudio, packet->dataSize)) {
            // 缓冲区溢出，清空一些旧数据再试
            int16_t tempBuf[BLOCK_SIZE];
            readFromRingBuffer(&rxBuffer, tempBuf, BLOCK_SIZE / 2);
            writeToRingBuffer(&rxBuffer, decodedAudio, packet->dataSize);
        }
        
        // 2. 同时写入设备对应的独立缓冲区（用于混音）
        if (peerDeviceId > 0 && peerDeviceId <= MAX_PEERS) {
            int peerIndex = -1;
            
            // 查找设备在peerDevices数组中的位置
            for (int i = 0; i < MAX_PEERS; i++) {
                if (peerDevices[i].info.active && peerDevices[i].info.deviceId == peerDeviceId) {
                    peerIndex = i;
                    break;
                }
            }
            
            // 如果找到了设备，写入其独立缓冲区
            if (peerIndex >= 0) {
                if (!writeToRingBuffer(&peerDevices[peerIndex].audioBuffer, decodedAudio, packet->dataSize)) {
                    // 缓冲区溢出处理
                    int16_t tempBuf[BLOCK_SIZE];
                    readFromRingBuffer(&peerDevices[peerIndex].audioBuffer, tempBuf, BLOCK_SIZE / 2);
                    writeToRingBuffer(&peerDevices[peerIndex].audioBuffer, decodedAudio, packet->dataSize);
                }
            }
        }
    }
    
    // 如果获取到了有效的对等设备信息，更新对等设备列表
    if (peerDeviceId > 0 && peerDeviceId != deviceId) {  // 排除自己
        bool deviceFound = false;
        int emptySlot = -1;
        
        // 查找设备是否已存在或有空槽位
        for (int i = 0; i < MAX_PEERS; i++) {
            if (peerList[i].active && peerList[i].deviceId == peerDeviceId) {
                // 更新已存在的设备信息
                peerList[i].lastSeen = millis();
                peerList[i].channel = peerChannelId;
                memcpy(peerList[i].mac, mac_addr, 6);
                deviceFound = true;
                
                // 同时更新peerDevices数组中的设备信息
                peerDevices[i].info = peerList[i];
                
                break;
            } else if (!peerList[i].active && emptySlot == -1) {
                // 记录第一个空槽位
                emptySlot = i;
            }
        }
        
        // 如果是新设备且有空槽位，添加到列表
        if (!deviceFound && emptySlot != -1) {
            peerList[emptySlot].deviceId = peerDeviceId;
            peerList[emptySlot].channel = peerChannelId;
            peerList[emptySlot].active = true;
            peerList[emptySlot].lastSeen = millis();
            memcpy(peerList[emptySlot].mac, mac_addr, 6);
            
            // 同时更新peerDevices数组中的设备信息
            peerDevices[emptySlot].info = peerList[emptySlot];
            peerDevices[emptySlot].volume = 1.0f;
            peerDevices[emptySlot].muted = false;
            
            // 添加为ESP-NOW对等设备，以便可以发送数据
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, mac_addr, 6);
            peerInfo.channel = currentChannel;  // 使用当前WiFi通道
            peerInfo.encrypt = false;  // 不加密
            
            if (esp_now_add_peer(&peerInfo) != ESP_OK) {
                Serial.printf("❌ 无法添加对等设备ID %d\n", peerDeviceId);
            } else {
                Serial.printf("✅ 添加新对等设备ID %d 到频道 %d\n", peerDeviceId, peerChannelId);
            }
        }
    }
}

//============================ 工具函数 ============================

/**
 * MAC地址转字符串
 * 
 * @param mac MAC地址
 * @return 格式化的MAC地址字符串
 */
String macToString(const uint8_t* mac) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

/**
 * MAC地址比较辅助函数
 * 
 * @param mac1 第一个MAC地址
 * @param mac2 第二个MAC地址
 * @return 两个MAC地址是否相同
 */
bool compareMAC(const uint8_t* mac1, const uint8_t* mac2) {
    return memcmp(mac1, mac2, 6) == 0;
}

//============================ 环形缓冲区函数 ============================

/**
 * 初始化环形缓冲区
 * 
 * @param rb 环形缓冲区结构
 * @param size 缓冲区大小
 */
void initRingBuffer(RingBuffer *rb, int size) {
    rb->buffer = rxBufferData;  // 使用全局分配的PSRAM缓冲区
    rb->size = size;
    rb->readIndex = 0;
    rb->writeIndex = 0;
    rb->available = 0;
    rb->overflow = false;
    
    // 创建互斥锁保护并发访问
    rb->mutex = xSemaphoreCreateMutex();
}

/**
 * 向环形缓冲区写入数据
 * 
 * @param rb 环形缓冲区结构
 * @param data 数据源
 * @param len 数据长度
 * @return 是否成功写入
 */
bool writeToRingBuffer(RingBuffer *rb, int16_t *data, int len) {
    bool result = false;
    
    // 参数验证
    if (rb->buffer == NULL || data == NULL || len <= 0 || len > rb->size) {
        return false;
    }
    
    // 获取互斥锁，超时10ms
    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // 检查可用空间
        int freeSpace = rb->size - rb->available;
        if (len <= freeSpace) {
            // 有足够空间，写入数据
            for (int i = 0; i < len; i++) {
                rb->buffer[rb->writeIndex] = data[i];
                rb->writeIndex = (rb->writeIndex + 1) % rb->size;
            }
            
            rb->available += len;
            result = true;
        } else {
            rb->overflow = true;
            result = false;
        }
        
        // 释放互斥锁
        xSemaphoreGive(rb->mutex);
    }
    
    return result;
}

/**
 * 从环形缓冲区读取数据
 * 
 * @param rb 环形缓冲区结构
 * @param data 目标缓冲区
 * @param maxLen 最大读取长度
 * @return 实际读取的数据量
 */
int readFromRingBuffer(RingBuffer *rb, int16_t *data, int maxLen) {
    int read = 0;
    
    // 参数验证
    if (rb->buffer == NULL || data == NULL || maxLen <= 0) {
        return 0;
    }
    
    // 获取互斥锁，超时5ms
    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        // 计算实际可读取的数量
        read = min(rb->available, maxLen);
        
        // 读取数据
        for (int i = 0; i < read; i++) {
            data[i] = rb->buffer[rb->readIndex];
            rb->readIndex = (rb->readIndex + 1) % rb->size;
        }
        
        rb->available -= read;
        
        // 释放互斥锁
        xSemaphoreGive(rb->mutex);
    }
    
    return read;
}

/**
 * 获取环形缓冲区中可用数据量
 * 
 * @param rb 环形缓冲区结构
 * @return 可用数据量
 */
int getRingBufferAvailable(RingBuffer *rb) {
    int available = 0;
    
    if (rb == NULL || rb->buffer == NULL) {
        return 0;
    }
    
    // 获取互斥锁，超时5ms
    if (xSemaphoreTake(rb->mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        available = rb->available;
        // 释放互斥锁
        xSemaphoreGive(rb->mutex);
    }
    
    return available;
}

/**
 * 释放环形缓冲区资源
 * 
 * @param rb 环形缓冲区结构
 */
void freeRingBuffer(RingBuffer *rb) {
    if (rb->mutex != NULL) {
        vSemaphoreDelete(rb->mutex);
        rb->mutex = NULL;
    }
    
    // 注意: 这里不释放buffer，因为它是全局分配的
    rb->buffer = NULL;
}

/**
 * 初始化频道系统
 * 
 * 设置频道表并初始化对等设备列表
 * 
 * @return 初始化是否成功
 */
bool wireless_setup_channels() {
    Serial.println("📡 初始化多人会话频道系统...");
    
    // 初始化频道列表
    for (int i = 0; i < MAX_CHANNELS; i++) {
        channelList[i].channelId = i + 1;  // 频道ID从1开始
        channelList[i].peerCount = 0;
        channelList[i].active = false;
        
        for (int j = 0; j < MAX_PEERS; j++) {
            channelList[i].peerIds[j] = 0;
        }
    }
    
    // 初始化对等设备列表
    for (int i = 0; i < MAX_PEERS; i++) {
        memset(peerList[i].mac, 0, 6);
        peerList[i].deviceId = 0;
        peerList[i].channel = 0;
        peerList[i].active = false;
        peerList[i].lastSeen = 0;
    }
    
    // 默认不加入任何频道
    currentChannelId = 0;
    
    // 读取保存的频道设置
    currentChannelId = preferences.getUChar("channelId", 0);
    int savedChannel = preferences.getInt("channel", DEFAULT_WIFI_CHANNEL);
    
    // 确保WiFi通道正确设置
    if (savedChannel > 0 && savedChannel <= 13) {
        Serial.printf("📌 从存储读取WiFi通道: %d\n", savedChannel);
        currentChannel = savedChannel;
        
        // 同步WiFi设置
        WiFi.channel(currentChannel);
        Serial.printf("📌 已设置WiFi通道为: %d\n", WiFi.channel());
        
        // 更新广播对等设备
        memset(peerAddress, 0xFF, 6);  // 设置为广播地址
        
        // 如果已存在，先删除
        if (esp_now_is_peer_exist(peerAddress)) {
            esp_now_del_peer(peerAddress);
        }
        
        // 添加广播对等设备
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, peerAddress, 6);
        peerInfo.channel = currentChannel;
        peerInfo.encrypt = false;
        
        esp_err_t result = esp_now_add_peer(&peerInfo);
        if (result != ESP_OK) {
            Serial.printf("⚠️ 添加广播对等设备失败，错误码: %d\n", result);
        } else {
            Serial.println("✅ 已添加广播对等设备");
        }
    }
    
    if (currentChannelId > 0 && currentChannelId <= MAX_CHANNELS) {
        Serial.printf("📝 从存储中读取频道设置: 频道 %d\n", currentChannelId);
        
        // 尝试加入保存的频道
        return wireless_join_channel(currentChannelId);
    }
    
    Serial.println("✅ 频道系统初始化完成");
    return true;
}

/**
 * 加入特定频道
 * 
 * 修改MAC地址，更新WiFi通道，并重新配置ESP-NOW
 * 
 * @param channelId 频道ID (1-8)
 * @return 加入是否成功
 */
bool wireless_join_channel(uint8_t channelId) {
    if (channelId < 1 || channelId > MAX_CHANNELS) {
        Serial.println("❌ 无效的频道ID，必须在1-8之间");
        return false;
    }
    
    // 如果已经在该频道，不需要重复加入
    if (channelId == currentChannelId) {
        Serial.printf("ℹ️ 已经在频道%d中，无需切换\n", channelId);
        return true;
    }
    
    Serial.printf("🔄 正在加入频道%d...\n", channelId);
    
    // 根据频道选择通道 (通道可以使用1+频道ID，或者其他映射)
    int wifiChannel = 1 + (channelId % 11);  // 将频道映射到WiFi通道1-12
    
    // 设置WiFi通道并更新MAC地址，使其包含频道信息
    bool channelChangeResult = wireless_set_channel(wifiChannel);
    if (!channelChangeResult) {
        // 如果MAC地址更新方式失败，尝试使用简单方法
        Serial.println("🔄 尝试使用备用方法加入频道...");
        
        // 简单地设置WiFi通道，不更新MAC地址
        WiFi.channel(wifiChannel);
        
        // 重新初始化ESP-NOW
        esp_now_deinit();
        delay(20);
        
        if (esp_now_init() != ESP_OK) {
            Serial.println("❌ 备用方法：ESP-NOW重新初始化失败");
            return false;
        }
        
        // 重新注册回调
        esp_now_register_send_cb(onDataSent);
        esp_now_register_recv_cb(onDataReceived);
        
        // 确保广播地址正确设置
        memset(peerAddress, 0xFF, 6);  // 设置为广播地址 FF:FF:FF:FF:FF:FF
        
        // 更新对等设备
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, peerAddress, 6);
        peerInfo.channel = wifiChannel;
        peerInfo.encrypt = false;
        
        // 如果对等设备已存在，先删除
        esp_now_del_peer(peerAddress);
        
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("❌ 备用方法：添加对等设备失败");
            return false;
        } else {
            Serial.println("✅ 已添加广播对等设备");
        }
        
        // 更新当前频道信息
        currentChannel = wifiChannel;
        currentChannelId = channelId;
        
        // 保存频道设置
        preferences.putUChar("channelId", currentChannelId);
        preferences.putInt("channel", currentChannel);
        
        // 确保WiFi通道与当前通道一致
        if (WiFi.channel() != currentChannel) {
            Serial.printf("⚠️ WiFi通道不一致，设置中：WiFi通道=%d, 当前通道=%d\n", 
                         WiFi.channel(), currentChannel);
            WiFi.channel(currentChannel);
            delay(10);
            Serial.printf("✅ 已同步WiFi通道：新通道=%d\n", WiFi.channel());
        }
        
        // 清空对等设备列表，准备重新扫描
        for (int i = 0; i < MAX_PEERS; i++) {
            peerList[i].active = false;
        }
        
        // 立即执行一次对等设备扫描
        wireless_scan_peers();
        
        // 发送一个广播通知包，宣告自己的存在
        sendHeartbeat();
        
        Serial.printf("✅ 已使用备用方法加入频道%d，WiFi通道%d\n", currentChannelId, wifiChannel);
        return true;
    }
    
    // 主方法成功，添加广播地址
    // 确保广播地址正确设置
    memset(peerAddress, 0xFF, 6);
    
    // 添加广播对等设备
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, peerAddress, 6);
    peerInfo.channel = wifiChannel;
    peerInfo.encrypt = false;
    
    // 如果已存在，先移除
    esp_now_del_peer(peerAddress);
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("⚠️ 添加广播对等设备失败，但尝试继续");
    } else {
        Serial.println("✅ 已添加广播对等设备");
    }
    
    // 更新当前频道ID
    currentChannelId = channelId;
    
    // 保存频道设置
    preferences.putUChar("channelId", currentChannelId);
    preferences.putInt("channel", currentChannel);
    
    // 确保WiFi通道与当前通道一致
    if (WiFi.channel() != currentChannel) {
        Serial.printf("⚠️ WiFi通道不一致，设置中：WiFi通道=%d, 当前通道=%d\n", 
                     WiFi.channel(), currentChannel);
        WiFi.channel(currentChannel);
        delay(10);
        Serial.printf("✅ 已同步WiFi通道：新通道=%d\n", WiFi.channel());
    }
    
    // 清空对等设备列表，准备重新扫描
    for (int i = 0; i < MAX_PEERS; i++) {
        peerList[i].active = false;
    }
    
    // 立即执行一次对等设备扫描
    wireless_scan_peers();
    
    // 发送一个广播通知包，宣告自己的存在
    sendHeartbeat();
    
    Serial.printf("✅ 已加入频道%d，WiFi通道%d\n", currentChannelId, wifiChannel);
    return true;
}

/**
 * 离开当前频道
 * 
 * @return 操作是否成功
 */
bool wireless_leave_channel() {
    if (currentChannelId == 0) {
        Serial.println("ℹ️ 当前未加入任何频道");
        return true;
    }
    
    Serial.printf("🔄 正在离开频道%d...\n", currentChannelId);
    
    // 重置频道ID
    currentChannelId = 0;
    preferences.putUChar("channelId", 0);
    
    // 清空对等设备列表
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peerList[i].active) {
            // 移除ESP-NOW对等设备
            esp_now_del_peer(peerList[i].mac);
            peerList[i].active = false;
        }
    }
    
    // 恢复默认通道
    if (!wireless_set_channel(DEFAULT_WIFI_CHANNEL)) {
        Serial.println("⚠️ 恢复默认通道失败，请手动重置设备");
        return false;
    }
    
    Serial.println("✅ 已离开频道，恢复默认通道");
    return true;
}

/**
 * 扫描并更新当前频道中的对等设备列表
 * 
 * 分析ESP-NOW数据包中的设备ID和MAC地址信息
 */
void wireless_scan_peers() {
    // 如果未加入频道，不需要扫描
    if (currentChannelId == 0) return;
    
    // 限制扫描频率，避免过度扫描
    unsigned long currentTime = millis();
    if (currentTime - lastPeerScanTime < 5000) {  // 至少间隔5秒
        return;
    }
    
    lastPeerScanTime = currentTime;
    
    Serial.printf("🔍 扫描频道%d中的对等设备...\n", currentChannelId);
    
    // 标记所有设备为非活跃，稍后通过接收到的数据包来激活
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peerList[i].active && (currentTime - peerList[i].lastSeen > 30000)) {
            // 如果30秒内没有收到设备的数据包，认为已离线
            Serial.printf("📴 设备ID %d 超时未响应，标记为离线\n", peerList[i].deviceId);
            
            // 移除ESP-NOW对等设备
            esp_now_del_peer(peerList[i].mac);
            peerList[i].active = false;
        }
    }
    
    // 注意：实际的对等设备发现是在onDataReceived回调中通过分析接收到的数据包完成的
    // 这里我们只是更新现有设备的状态
    
    // 统计当前活跃设备数量
    int activeCount = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peerList[i].active) {
            activeCount++;
        }
    }
    
    // 如果活跃设备数量变化，输出日志
    if (activeCount != lastChannelPeerCount) {
        Serial.printf("✅ 频道%d中发现%d个活跃设备\n", currentChannelId, activeCount);
        lastChannelPeerCount = activeCount;
    }
}

/**
 * 获取当前频道内的对等设备列表
 * 
 * @param peers 输出的对等设备数组
 * @param maxPeers 最大返回数量
 * @return 实际对等设备数量
 */
int wireless_get_peers_in_channel(PeerInfo* peers, int maxPeers) {
    if (peers == NULL || maxPeers <= 0) {
        return 0;
    }
    
    int count = 0;
    for (int i = 0; i < MAX_PEERS && count < maxPeers; i++) {
        if (peerList[i].active && peerList[i].channel == currentChannelId) {
            // 复制设备信息
            memcpy(&peers[count], &peerList[i], sizeof(PeerInfo));
            count++;
        }
    }
    
    return count;
}

/**
 * 向当前频道发送音频数据
 * 
 * @param audio_data 音频数据
 * @param samples 样本数
 * @return 是否成功发送到所有对等设备
 */
bool wireless_send_to_channel(int16_t* audio_data, size_t samples) {
    // 如果未加入任何频道，返回失败
    if (currentChannelId == 0) {
        return false;
    }
    
    // 确保ESP-NOW已初始化
    if (!ensure_espnow_initialized()) {
        return false;
    }
    
    // 扫描并更新对等设备列表
    wireless_scan_peers();
    
    bool allSuccess = true;
    int sentCount = 0;
    
    // 向频道中所有活跃设备发送音频数据
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peerList[i].active && peerList[i].channel == currentChannelId) {
            // 发送音频数据到该设备
            if (wireless_send_audio(audio_data, samples)) {
                sentCount++;
            } else {
                allSuccess = false;
            }
        }
    }
    
    if (sentCount > 0) {
        return true;  // 至少有一个设备发送成功
    }
    
    return false;  // 没有设备或全部发送失败
}

/**
 * 向特定设备发送音频数据
 * 
 * @param targetId 目标设备ID，0xFF表示广播给所有设备
 * @param audio_data 音频数据
 * @param samples 样本数
 * @return 是否成功发送
 */
bool wireless_send_to_peer(uint8_t targetId, int16_t* audio_data, size_t samples) {
    // 广播模式：发送给所有对等设备
    if (targetId == ALL_PEERS) {
        return wireless_send_to_channel(audio_data, samples);
    }
    
    // 查找目标设备
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peerList[i].active && peerList[i].deviceId == targetId) {
            // 找到目标设备，发送音频数据
            // 这里需要修改wireless_send_audio函数以支持指定目标设备的MAC地址
            return wireless_send_audio(audio_data, samples);
        }
    }
    
    return false;  // 未找到目标设备
}

/**
 * 获取当前活跃的频道列表
 * 
 * @param channels 输出的频道信息数组
 * @param maxChannels 最大返回数量
 * @return 实际活跃频道数量
 */
int wireless_get_active_channels(ChannelInfo* channels, int maxChannels) {
    if (channels == NULL || maxChannels <= 0) {
        return 0;
    }
    
    // 更新频道列表中的对等设备信息
    for (int i = 0; i < MAX_CHANNELS; i++) {
        channelList[i].peerCount = 0;
        
        for (int j = 0; j < MAX_PEERS; j++) {
            if (peerList[j].active && peerList[j].channel == (i+1)) {
                channelList[i].peerIds[channelList[i].peerCount] = peerList[j].deviceId;
                channelList[i].peerCount++;
                channelList[i].active = true;
            }
        }
        
        // 如果当前加入了该频道，也标记为活跃
        if (currentChannelId == (i+1)) {
            channelList[i].active = true;
        }
    }
    
    // 复制活跃频道信息
    int count = 0;
    for (int i = 0; i < MAX_CHANNELS && count < maxChannels; i++) {
        if (channelList[i].active) {
            memcpy(&channels[count], &channelList[i], sizeof(ChannelInfo));
            count++;
        }
    }
    
    return count;
}

/**
 * 获取当前频道信息
 * 
 * @return 当前频道ID，0表示未加入任何频道
 */
uint8_t wireless_get_current_channel_id() {
    return currentChannelId;
}

// 混音函数 - 将多个设备的音频混合在一起
void mixAudioFromPeers(int16_t* outputBuffer, int sampleCount) {
    // 清空输出缓冲区
    memset(outputBuffer, 0, sampleCount * sizeof(int16_t));
    
    // 为每个活跃设备的音频进行混合
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!peerDevices[i].info.active || peerDevices[i].muted) 
            continue;
            
        int16_t peerAudio[BLOCK_SIZE];
        int samplesRead = readFromRingBuffer(&peerDevices[i].audioBuffer, 
                                             peerAudio, sampleCount);
                                             
        if (samplesRead > 0) {
            // 应用音量调整
            for (int j = 0; j < samplesRead; j++) {
                // 32位混音避免溢出
                int32_t mixed = outputBuffer[j] + 
                               (int32_t)(peerAudio[j] * peerDevices[i].volume);
                               
                // 限制值范围
                if (mixed > 32767) mixed = 32767;
                if (mixed < -32768) mixed = -32768;
                
                outputBuffer[j] = (int16_t)mixed;
            }
        }
    }
}

// 预先扫描所有频道上的设备
void scanAllChannels() {
    // 保存当前频道
    uint8_t currentChannel = wireless_get_current_channel_id();
    
    // 扫描所有频道
    for (int i = 1; i <= MAX_CHANNELS; i++) {
        // 临时切换到该频道
        wireless_set_channel(1 + (i % 11));  // 设置WiFi通道
        
        // 发送探测包
        sendProbePacket();
        
        // 给一点时间让设备响应
        delay(100);
        
        // 处理任何接收到的数据
        wireless_process();
    }
    
    // 如果当前在某个频道，恢复
    if (currentChannel > 0) {
        wireless_join_channel(currentChannel);
    } else {
        // 否则恢复默认通道
        wireless_set_channel(DEFAULT_WIFI_CHANNEL);
    }
    
    // 打印扫描结果
    Serial.println("频道扫描结果:");
    for (int i = 1; i <= MAX_CHANNELS; i++) {
        int count = 0;
        for (int j = 0; j < MAX_PEERS; j++) {
            if (peerList[j].active && peerList[j].channel == i) {
                count++;
            }
        }
        if (count > 0) {
            Serial.printf("  频道 %d: %d个设备\n", i, count);
        }
    }
}

/**
 * 初始化对等设备音频缓冲区
 * 为每个对等设备创建单独的音频缓冲区
 */
void initPeerDevices() {
    Serial.println("初始化对等设备音频缓冲区...");
    
    for (int i = 0; i < MAX_PEERS; i++) {
        // 初始化对等设备信息
        memset(&peerDevices[i].info, 0, sizeof(PeerInfo));
        peerDevices[i].info.active = false;
        
        // 设置默认音量和静音状态
        peerDevices[i].volume = 1.0f;
        peerDevices[i].muted = false;
        
        // 初始化该设备的音频缓冲区
        initRingBuffer(&peerDevices[i].audioBuffer, BLOCK_SIZE * 10); // 缓冲区大小为BLOCK_SIZE的10倍
    }
    
    Serial.println("对等设备初始化完成");
}

/**
 * 初始化ESP-NOW无线系统
 * 
 * 1. 初始化WiFi
 * 2. 初始化ESP-NOW
 * 3. 设置回调函数
 * 4. 初始化环形缓冲区
 * 
 * @return 初始化是否成功
 */
bool wireless_setup() {
    // 初始化WiFi
    WiFi.mode(WIFI_STA);
    
    // 备份原始MAC地址
    esp_read_mac(originalMacAddress, ESP_MAC_WIFI_STA);
    Serial.print("原始MAC: ");
    Serial.println(macToString(originalMacAddress));
    
    // 初始化ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW初始化失败");
        return false;
    }
    
    // 注册回调函数
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataReceived);
    
    // 从preferences中读取设备ID
    preferences.begin("wireless", false);
    deviceId = preferences.getUChar("deviceId", 1);
    preferences.end();
    
    // 初始化接收环形缓冲区
    int bufferSize = BLOCK_SIZE * 10;  // 10倍块大小作为环形缓冲区
    rxBufferData = (int16_t*)malloc(bufferSize * sizeof(int16_t));
    
    if (rxBufferData == NULL) {
        Serial.println("❌ 无法分配接收缓冲区内存");
        return false;
    }
    
    initRingBuffer(&rxBuffer, bufferSize);
    rxBuffer.buffer = rxBufferData;
    
    // 初始化对等设备音频缓冲区
    initPeerDevices();
    
    // 设置广播地址
    memset(peerAddress, 0xFF, 6);  // 设置为广播地址 FF:FF:FF:FF:FF:FF
    
    // 添加广播对等设备
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    
    memcpy(peerInfo.peer_addr, peerAddress, 6);
    peerInfo.channel = currentChannel;
    peerInfo.encrypt = false;
    
    // 如果已存在，先移除
    esp_now_del_peer(peerAddress);
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("⚠️ 添加广播对等设备失败，尝试继续");
    } else {
        Serial.println("✅ 已添加广播对等设备");
    }
    
    // 初始化频道系统
    wireless_setup_channels();
    
    // 尝试使用存储的WiFi通道
    preferences.begin("wireless", false);
    int savedChannel = preferences.getInt("channel", DEFAULT_WIFI_CHANNEL);
    
    if(savedChannel > 0 && savedChannel <= 13) {
        wireless_set_channel(savedChannel);
    } else {
        // 如果没有存储的通道，使用自动通道选择
        int bestChannel = wireless_find_best_channel();
        if(bestChannel > 0) {
            wireless_set_channel(bestChannel);
        }
    }
    
    preferences.end();
    
    Serial.println("✅ ESP-NOW无线系统初始化完成");
    
    return true;
}

/**
 * 调整特定对等设备的音量
 * 
 * @param deviceId 设备ID
 * @param volume 音量值(0.0-3.0)
 * @return 操作是否成功
 */
bool setPeerVolume(uint8_t deviceId, float volume) {
    // 验证参数
    if (deviceId == 0 || deviceId > MAX_PEERS || volume < 0.0f || volume > 3.0f) {
        return false;
    }
    
    // 限制音量范围
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 3.0f) volume = 3.0f;
    
    // 查找设备
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peerDevices[i].info.active && peerDevices[i].info.deviceId == deviceId) {
            // 设置音量
            peerDevices[i].volume = volume;
            Serial.printf("设备ID %d 音量已调整为 %.1f\n", deviceId, volume);
            return true;
        }
    }
    
    // 未找到对应设备
    return false;
}

/**
 * 设置特定对等设备的静音状态
 * 
 * @param deviceId 设备ID
 * @param muted 是否静音
 * @return 操作是否成功
 */
bool setPeerMuted(uint8_t deviceId, bool muted) {
    // 验证参数
    if (deviceId == 0 || deviceId > MAX_PEERS) {
        return false;
    }
    
    // 查找设备
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peerDevices[i].info.active && peerDevices[i].info.deviceId == deviceId) {
            // 设置静音状态
            peerDevices[i].muted = muted;
            Serial.printf("设备ID %d %s\n", deviceId, muted ? "已静音" : "已取消静音");
            return true;
        }
    }
    
    // 未找到对应设备
    return false;
}

/**
 * 诊断ESP-NOW和WiFi状态
 * 
 * 打印所有可能影响连接的关键状态
 */
void wireless_diagnostic() {
    Serial.println("\n===== ESP-NOW 诊断信息 =====");
    
    // 基本信息
    Serial.printf("📝 设备ID: %d\n", deviceId);
    Serial.printf("📝 当前频道ID: %d\n", currentChannelId);
    Serial.printf("📝 当前WiFi通道: %d\n", currentChannel);
    
    // MAC地址信息
    Serial.printf("📝 当前MAC地址: %s\n", myMacAddress.c_str());
    Serial.printf("📝 原始MAC地址: %s\n", macToString(originalMacAddress).c_str());
    Serial.printf("📝 广播地址设置: %s\n", macToString(peerAddress).c_str());
    
    // 验证广播地址是否设置正确
    bool broadcastValid = true;
    for (int i = 0; i < 6; i++) {
        if (peerAddress[i] != 0xFF) {
            broadcastValid = false;
            break;
        }
    }
    Serial.printf("📝 广播地址有效: %s\n", broadcastValid ? "是" : "否");
    
    // 检查ESP-NOW对等设备
    esp_now_peer_info_t peerInfo;
    bool isPeerExist = esp_now_is_peer_exist(peerAddress) == true;
    Serial.printf("📝 广播对等设备已添加: %s\n", isPeerExist ? "是" : "否");
    
    if (isPeerExist && esp_now_get_peer(peerAddress, &peerInfo) == ESP_OK) {
        Serial.printf("📝 对等设备通道: %d\n", peerInfo.channel);
        Serial.printf("📝 对等设备加密: %s\n", peerInfo.encrypt ? "是" : "否");
        
        // 检查通道是否一致，如果不一致则修复
        if (peerInfo.channel != currentChannel) {
            Serial.printf("⚠️ 检测到通道不一致！ESP-NOW对等设备通道=%d, 当前WiFi通道=%d\n", 
                         peerInfo.channel, currentChannel);
            
            // 删除旧的对等设备
            esp_now_del_peer(peerAddress);
            
            // 重新添加对等设备，确保通道一致
            esp_now_peer_info_t newPeerInfo;
            memset(&newPeerInfo, 0, sizeof(newPeerInfo));
            memcpy(newPeerInfo.peer_addr, peerAddress, 6);
            newPeerInfo.channel = currentChannel;
            newPeerInfo.encrypt = false;
            
            esp_err_t result = esp_now_add_peer(&newPeerInfo);
            if (result == ESP_OK) {
                Serial.println("✅ 已修复通道不一致问题");
            } else {
                Serial.printf("❌ 修复通道不一致失败，错误码: %d\n", result);
            }
        }
    }
    
    // WiFi状态
    Serial.printf("📝 WiFi模式: %d (1=STA, 2=AP, 3=STA+AP)\n", WiFi.getMode());
    Serial.printf("📝 WiFi通道: %d\n", WiFi.channel());
    
    // 如果检测到WiFi通道与设置的通道不一致，尝试修复
    if (WiFi.channel() != currentChannel) {
        Serial.printf("⚠️ 检测到WiFi通道不一致！WiFi当前通道=%d, 期望通道=%d\n",
                     WiFi.channel(), currentChannel);
        
        // 重新设置WiFi通道
        WiFi.channel(currentChannel);
        Serial.println("✅ 已修复WiFi通道不一致");
    }
    
    // 统计信息
    Serial.printf("📝 发送数据包: %u\n", sentPacketsCount);
    Serial.printf("📝 接收数据包: %u\n", receivedPacketsCount);
    Serial.printf("📝 丢包数量: %u\n", packetLossCount);
    Serial.printf("📝 发送错误: %d\n", sendErrorCount);
    
    // 时间信息
    Serial.printf("📝 上次发送时间: %lu ms前\n", millis() - lastSentTime);
    Serial.printf("📝 上次接收时间: %lu ms前\n", millis() - lastReceivedTime);
    
    // 测试连接
    Serial.println("📡 正在测试ESP-NOW连接...");
    bool testResult = sendHeartbeat();
    Serial.printf("📡 测试结果: %s\n", testResult ? "成功" : "失败");
    
    Serial.println("===== 诊断完成 =====\n");
}

/**
 * 检查MAC地址是否为空（全0）
 * 
 * @param addr MAC地址
 * @return 是否为空地址
 */
bool isEmptyAddr(const uint8_t* addr) {
    for (int i = 0; i < 6; i++) {
        if (addr[i] != 0) {
            return false;
        }
    }
    return true;
}

/**
 * 确保ESP-NOW已初始化
 * 
 * 检查ESP-NOW状态，如果未初始化则重新初始化
 * 
 * @return 初始化是否成功
 */
bool ensure_espnow_initialized() {
    // 无法直接检查ESP-NOW是否初始化，使用尝试添加/删除一个测试对等点来检测
    static uint8_t test_addr[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    esp_now_peer_info_t test_peer;
    
    // 尝试获取对等设备作为测试
    esp_err_t result = esp_now_get_peer(test_addr, &test_peer);
    
    // esp_now_is_peer_exist通常会返回错误代码，但不会打印"esp now not init"
    // 如果ESP-NOW初始化正常，应该只是返回设备不存在
    if (result == ESP_ERR_ESPNOW_NOT_INIT) {
        Serial.println("⚠️ 检测到ESP-NOW未初始化，尝试恢复...");
        
        // 重新初始化ESP-NOW
        esp_now_deinit();  // 确保完全释放资源
        delay(50);
        
        if (esp_now_init() != ESP_OK) {
            Serial.println("❌ ESP-NOW重新初始化失败");
            return false;
        }
        
        // 重新注册回调
        esp_now_register_send_cb(onDataSent);
        esp_now_register_recv_cb(onDataReceived);
        
        // 重新添加广播对等设备
        uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, broadcastAddr, 6);
        peerInfo.channel = currentChannel;
        peerInfo.encrypt = false;
        
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("❌ 重新添加广播对等设备失败");
        } else {
            Serial.println("✅ ESP-NOW已重新初始化并添加广播对等设备");
        }
        
        // 如果有特定的对等设备，也重新添加
        if (!isEmptyAddr(peerAddress)) {
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, peerAddress, 6);
            peerInfo.channel = currentChannel;
            peerInfo.encrypt = false;
            
            if (esp_now_add_peer(&peerInfo) != ESP_OK) {
                Serial.println("⚠️ 重新添加对等设备失败");
            }
        }
        
        return true;
    }
    
    return true;  // ESP-NOW已经初始化
}

