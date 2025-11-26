/**
 * 连续噪声抑制器 - 专门针对VAD触发时的连续巨大背景噪声抑制
 */

#ifndef CONTINUOUS_NOISE_SUPPRESSOR_H
#define CONTINUOUS_NOISE_SUPPRESSOR_H

#include <stdint.h>

// 连续噪声抑制器类
class ContinuousNoiseSuppressor {
public:
    ContinuousNoiseSuppressor();
    
    // 初始化抑制器
    bool init();
    
    // VAD触发时的噪声抑制处理
    // buffer: 音频缓冲区（会被直接修改）
    // size: 缓冲区大小
    // vadActive: VAD是否触发
    void processVoiceWithNoiseSuppression(int16_t* buffer, int size, bool vadActive);
    
    // 重置噪声模板（用于环境变化时）
    void resetNoiseProfile();
    
    // 获取当前抑制状态信息（用于调试）
    bool isNoiseProfileReady() const { return noiseProfileReady; }
    float getCurrentSuppressionFactor() const { return suppressionFactor; }
    float getNoiseFloor() const { return noiseFloor; }
    
private:
    // 噪声模板相关
    float noiseFloor;           // 噪声底线能量
    float noiseVariance;        // 噪声方差（稳定性指标）
    uint32_t noiseProfileFrames; // 噪声模板建立帧数
    bool noiseProfileReady;     // 噪声模板是否就绪
    
    // VAD状态跟踪
    bool previousVadState;      // 上一帧VAD状态
    uint32_t vadActiveFrames;   // VAD连续活跃帧数
    
    // 动态抑制参数
    float suppressionFactor;    // 当前噪声抑制系数
    float adaptiveThreshold;    // 自适应语音保护阈值
    
    // 内部处理方法
    void updateNoiseProfile(int16_t* buffer, int size);
    void applyContinuousNoiseSuppression(int16_t* buffer, int size);
    float calculateFrameEnergy(int16_t* buffer, int size);
    float calculateFrameVariance(int16_t* buffer, int size, float mean);
    void updateSuppressionParameters();
};

// 配置参数（可根据实际环境调整）

// 噪声模板建立参数
#define NOISE_PROFILE_FRAMES        5       // 建立噪声模板需要的帧数
#define NOISE_STABILITY_THRESHOLD   0.3f    // 噪声稳定性阈值（越小越稳定）

// 抑制强度参数
#define BASE_SUPPRESSION_FACTOR     0.7f    // 基础抑制系数（0.7 = 30%抑制）
#define MAX_SUPPRESSION_FACTOR      0.3f    // 最大抑制系数（0.3 = 70%抑制）
#define MIN_SUPPRESSION_FACTOR      0.9f    // 最小抑制系数（0.9 = 10%抑制）

// 自适应参数
#define VOICE_PROTECTION_RATIO      2.0f    // 语音保护比例（相对噪声底线）
#define SUPPRESSION_RAMP_FRAMES     7       // 抑制强度渐进帧数

#endif // CONTINUOUS_NOISE_SUPPRESSOR_H



