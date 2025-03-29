/**
 * ESP32-S3 无线音频传输系统 - 配置文件 (config.h)
 * 
 * 【文件说明】
 * 本文件集中管理ESP32-S3无线音频系统的所有硬件和软件配置参数，
 * 包括引脚定义、音频参数、版本信息等。修改此文件可快速调整系统行为，
 * 无需在多个源文件中寻找并更改参数。
 * 
 * 【主要配置项】
 * 1. 系统标识
 *    - FW_VERSION: 固件版本
 *    - DEVICE_ID: 设备ID (1-8)
 * 
 * 2. I2S配置
 *    - 麦克风接口引脚 (BCLK, LRCK, DATA)
 *    - 扬声器接口引脚 (BCLK, LRCK, DATA)
 *    - 采样率和位深度设置
 * 
 * 3. 音频处理参数
 *    - BLOCK_SIZE: 音频处理块大小
 *    - RAW_SAMPLES_PER_PACKET: 每个数据包的原始样本数
 * 
 * 4. 内存分配策略
 *    - BUFFER_ATTR: 缓冲区内存属性 (PSRAM/内部RAM)
 * 
 * 【使用指南】
 * 根据具体硬件和需求调整参数:
 * - 更改设备ID以区分不同设备
 * - 调整I2S引脚以匹配电路连接
 * - 修改音频参数以平衡质量和性能
 * 
 * 所有参数均使用预处理宏定义，以便编译器优化，
 * 不会占用运行时内存。
 */

#ifndef CONFIG_H
#define CONFIG_H

#pragma once

#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <esp32/spiram.h>

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
#define PACKET_SIZE 120           // 发送包大小，压缩后约30ms音频
#define NOISE_GATE_THRESHOLD 600  // 噪声门限阈值

// 缓冲区大小定义 - 利用PSRAM
 
#define RX_BUFFER_SIZE 32000      // 接收缓冲区(2秒)


//============================ 内存分配属性 ============================
#define BUFFER_ATTR __attribute__((aligned(4)))  // 缓冲区属性，用于PSRAM对齐

#endif /* CONFIG_H */


