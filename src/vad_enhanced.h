/**
 * VAD增强模块 - 基于参考实现的简化状态机VAD
 * 
 * 功能特性：
 * - 状态机驱动，稳定可靠
 * - 连续帧检测，减少误触发
 * - 自适应噪声估计
 * - 多特征融合检测
 */

#ifndef VAD_ENHANCED_H
#define VAD_ENHANCED_H

#include <stdint.h>

//================== VAD配置参数 ==================
#define VAD_SAMPLE_RATE 8000
#define VAD_BLOCK_SIZE 160  // 更新为160以匹配iLBC-20ms帧大小

// 基本阈值参数 - 优化首音触发和数字识别
#define VAD_ENERGY_THRESHOLD 1800        // 进一步降低能量阈值，改善首音和"5"等数字触发
#define VAD_AMPLITUDE_THRESHOLD 3500    // 降低振幅阈值，适应轻声说话
#define VAD_ZERO_CROSSING_MIN 12        // 降低最小过零率，适应连续音
#define VAD_ZERO_CROSSING_MAX 65        // 扩大最大过零率范围

// 自适应参数 - 优化连续语音检测
#define VAD_ADAPTIVE_FACTOR 1.8f        // 降低自适应因子，提高敏感度
#define VAD_NOISE_ADAPT_RATE 0.03f      // 降低噪声适应速率，保持稳定

// 连续帧检测参数 - 优化首音响应速度
#define VAD_REQUIRED_VOICE_FRAMES 1     // 降低到1帧，大幅提高首音触发速度
#define VAD_REQUIRED_SILENCE_FRAMES 12  // 增加到12帧，更好保护连续音

// 时间参数
#define VAD_MIN_VOICE_DURATION_MS 60    // 最小语音持续时间
#define VAD_HANGOVER_TIME_MS 200        // 语音尾部时间

// 高级特征参数
#define VAD_PEAK_TO_AVERAGE_RATIO 3.2f  // 峰均比阈值 - 机场环境平衡值

//================== VAD状态定义 ==================
typedef enum {
    VAD_STATE_SILENCE,       // 静音状态
    VAD_STATE_VOICE_ONSET,   // 语音开始
    VAD_STATE_VOICE_ACTIVE,  // 语音活跃
    VAD_STATE_VOICE_HANGOVER // 语音尾部
} vad_state_t;

//================== VADEnhanced类 ==================
class VADEnhanced {
public:
    VADEnhanced();
    ~VADEnhanced();
    
    // 初始化VAD系统
    bool init();
    
    // 处理音频块，返回是否检测到语音
    bool processBlock(int16_t* audioBlock, int blockSize);
    
    // 获取检测结果
    bool isVoiceDetected() const { return voiceDetected; }
    
    // 重置VAD状态
    void reset();
    
private:
    // 噪声估计
    int32_t estimatedNoiseLevel;    // 估计的噪声能量水平
    int16_t estimatedNoisePeak;     // 估计的噪声峰值
    int32_t estimatedZeroCrossing;  // 估计的噪声过零率
    
    // 状态跟踪
    vad_state_t state;              // 当前VAD状态
    uint32_t voiceStartTime;        // 语音开始时间
    uint32_t lastVoiceTime;         // 最后检测到语音的时间
    uint32_t hangoverEndTime;       // 尾部结束时间
    
    // 连续帧检测
    uint8_t consecutiveVoiceFrames;    // 连续检测到语音的帧数
    uint8_t consecutiveSilenceFrames;  // 连续检测到静音的帧数
    
    // 语音状态管理
    bool voiceDetected;
    
    // 初始化状态
    bool initialized;
    uint32_t frameCount;            // 处理的帧计数
    
    // 内部处理方法
    int32_t calculateZeroCrossings(const int16_t* buffer, int size);
    float calculatePeakToAverageRatio(const int16_t* buffer, int size, int16_t peak, int32_t energy);
    bool detectVoiceFeatures(int32_t energy, int16_t amplitude, int32_t zeroCrossings, float peakRatio);
    void updateNoiseEstimate(int32_t energy, int16_t amplitude, int32_t zeroCrossings);
    uint32_t getCurrentTime();
};

#endif // VAD_ENHANCED_H
