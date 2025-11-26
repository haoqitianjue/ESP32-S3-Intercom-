/**
 * VAD增强模块实现 - 基于参考实现的简化状态机VAD
 */

#include "vad_enhanced.h"
#include <Arduino.h>
#include <string.h>
#include <math.h>

//================== VADEnhanced实现 ==================

VADEnhanced::VADEnhanced() : initialized(false) {
    reset();
}

VADEnhanced::~VADEnhanced() {
    // 析构函数
}

bool VADEnhanced::init() {
    // 初始化噪声估计
    estimatedNoiseLevel = VAD_ENERGY_THRESHOLD;
    estimatedNoisePeak = VAD_AMPLITUDE_THRESHOLD / 2;
    estimatedZeroCrossing = (VAD_ZERO_CROSSING_MIN + VAD_ZERO_CROSSING_MAX) / 2;
    
    // 初始化状态
    state = VAD_STATE_SILENCE;
    voiceStartTime = 0;
    lastVoiceTime = 0;
    hangoverEndTime = 0;
    
    // 初始化连续帧检测
    consecutiveVoiceFrames = 0;
    consecutiveSilenceFrames = 0;
    
    voiceDetected = false;
    frameCount = 0;
    initialized = true;
    
    return true;
}

void VADEnhanced::reset() {
    if (!initialized) return;
    
    state = VAD_STATE_SILENCE;
    voiceStartTime = 0;
    lastVoiceTime = 0;
    hangoverEndTime = 0;
    consecutiveVoiceFrames = 0;
    consecutiveSilenceFrames = 0;
    voiceDetected = false;
    frameCount = 0;
}

bool VADEnhanced::processBlock(int16_t* audioBlock, int blockSize) {
    if (!initialized || blockSize != VAD_BLOCK_SIZE) {
        return false;
    }
    
    uint32_t currentTime = getCurrentTime();
    frameCount++;
    
    // 计算基本特征
    int32_t energy = 0;
    int16_t maxAmplitude = 0;
    
    for (int i = 0; i < blockSize; i++) {
        int16_t absVal = audioBlock[i] < 0 ? -audioBlock[i] : audioBlock[i];
        energy += absVal;
        if (absVal > maxAmplitude) {
            maxAmplitude = absVal;
        }
    }
    
    // 计算平均能量
    energy = blockSize > 0 ? energy / blockSize : 0;
    
    // 计算过零率
    int32_t zeroCrossings = calculateZeroCrossings(audioBlock, blockSize);
    
    // 计算峰均比
    float peakToAvg = calculatePeakToAverageRatio(audioBlock, blockSize, maxAmplitude, energy);
    
    // 自适应噪声估计 - 只在静音状态下更新
    if (state == VAD_STATE_SILENCE) {
        updateNoiseEstimate(energy, maxAmplitude, zeroCrossings);
    }
    
    // 检测语音特征
    bool voiceFeatureDetected = detectVoiceFeatures(energy, maxAmplitude, zeroCrossings, peakToAvg);
    
    // 更新连续帧计数
    if (voiceFeatureDetected) {
        consecutiveVoiceFrames++;
        consecutiveSilenceFrames = 0;
    } else {
        consecutiveSilenceFrames++;
        consecutiveVoiceFrames = 0;
    }
    
    // 连续帧检测结果
    bool finalVoiceDetected = (consecutiveVoiceFrames >= VAD_REQUIRED_VOICE_FRAMES);
    bool finalSilenceDetected = (consecutiveSilenceFrames >= VAD_REQUIRED_SILENCE_FRAMES);
    
    // 状态机处理
    switch (state) {
        case VAD_STATE_SILENCE:
            if (finalVoiceDetected) {
                state = VAD_STATE_VOICE_ONSET;
                voiceStartTime = currentTime;
                lastVoiceTime = currentTime;
            }
            break;
            
        case VAD_STATE_VOICE_ONSET:
            if (voiceFeatureDetected || finalVoiceDetected) {
                lastVoiceTime = currentTime;
                // 检查是否满足最小持续时间
                if (currentTime - voiceStartTime >= VAD_MIN_VOICE_DURATION_MS) {
                    state = VAD_STATE_VOICE_ACTIVE;
                }
            } else if (finalSilenceDetected) {
                // 语音开始后很快停止，可能是误触发
                if (currentTime - voiceStartTime < VAD_MIN_VOICE_DURATION_MS) {
                    state = VAD_STATE_SILENCE;  // 回到静音状态
                } else {
                    state = VAD_STATE_VOICE_HANGOVER;
                    hangoverEndTime = currentTime + VAD_HANGOVER_TIME_MS;
                }
            }
            break;
            
        case VAD_STATE_VOICE_ACTIVE:
            if (voiceFeatureDetected) {
                lastVoiceTime = currentTime;
            } else if (finalSilenceDetected) {
                state = VAD_STATE_VOICE_HANGOVER;
                hangoverEndTime = currentTime + VAD_HANGOVER_TIME_MS;
            }
            break;
            
        case VAD_STATE_VOICE_HANGOVER:
            if (voiceFeatureDetected) {
                state = VAD_STATE_VOICE_ACTIVE;
                lastVoiceTime = currentTime;
                consecutiveSilenceFrames = 0;  // 重置静音帧计数
            } else if (currentTime >= hangoverEndTime && finalSilenceDetected) {
                state = VAD_STATE_SILENCE;
            }
            break;
    }
    
    // 返回是否处于语音状态
    bool inVoiceState = (state == VAD_STATE_VOICE_ONSET ||
                        state == VAD_STATE_VOICE_ACTIVE ||
                        state == VAD_STATE_VOICE_HANGOVER);
    
    if (state == VAD_STATE_SILENCE) {
        voiceDetected = finalVoiceDetected;  // 必须连续多帧检测到语音
    } else {
        voiceDetected = inVoiceState && (voiceFeatureDetected || finalVoiceDetected);
    }
    
    return voiceDetected;
}

int32_t VADEnhanced::calculateZeroCrossings(const int16_t* buffer, int size) {
    int32_t zeroCrossings = 0;
    
    for (int i = 1; i < size; i++) {
        if ((buffer[i] >= 0 && buffer[i-1] < 0) ||
            (buffer[i] < 0 && buffer[i-1] >= 0)) {
            zeroCrossings++;
        }
    }
    
    return zeroCrossings;
}

float VADEnhanced::calculatePeakToAverageRatio(const int16_t* buffer, int size, int16_t peak, int32_t energy) {
    if (size == 0 || energy == 0) {
        return 0.0f;
    }
    
    float average = (float)energy;
    if (average == 0.0f) {
        return 0.0f;
    }
    
    return (float)peak / average;
}

bool VADEnhanced::detectVoiceFeatures(int32_t energy, int16_t amplitude, int32_t zeroCrossings, float peakRatio) {
    // 计算动态阈值
    int32_t dynamicEnergyThreshold = (int32_t)(estimatedNoiseLevel * VAD_ADAPTIVE_FACTOR);
    int16_t dynamicAmplitudeThreshold = (int16_t)(estimatedNoisePeak * VAD_ADAPTIVE_FACTOR);
    
    // 确保阈值不低于基线
    if (dynamicEnergyThreshold < VAD_ENERGY_THRESHOLD) {
        dynamicEnergyThreshold = VAD_ENERGY_THRESHOLD;
    }
    if (dynamicAmplitudeThreshold < VAD_AMPLITUDE_THRESHOLD) {
        dynamicAmplitudeThreshold = VAD_AMPLITUDE_THRESHOLD;
    }
    
    // 基本能量和振幅检测
    bool energyDetected = (energy > dynamicEnergyThreshold);
    bool amplitudeDetected = (amplitude > dynamicAmplitudeThreshold);
    
    // 过零率检测
    bool zeroCrossingValid = (zeroCrossings >= VAD_ZERO_CROSSING_MIN &&
                             zeroCrossings <= VAD_ZERO_CROSSING_MAX);
    
    // 峰均比检测
    bool peakRatioValid = (peakRatio >= VAD_PEAK_TO_AVERAGE_RATIO);
    
    // 优化的特征检测逻辑 - 平衡噪音抑制和语音敏感度
    // 基本能量和振幅检测
    if (energyDetected && amplitudeDetected) {
        // 如果所有特征都支持，直接确认
        if (zeroCrossingValid && peakRatioValid) {
            return true;  // 所有特征都支持，确认为语音
        }
        // 如果有部分高级特征支持，使用较低要求
        else if (zeroCrossingValid || peakRatioValid) {
            // 降低信号强度要求，提高触发敏感度
            bool moderateSignal = (energy > dynamicEnergyThreshold * 1.2f) ||
                                 (amplitude > dynamicAmplitudeThreshold * 1.2f);
            if (moderateSignal) {
                return true;  // 适中信号即可触发
            }
        }
        // 即使没有高级特征支持，如果信号较强也接受
        else {
            bool strongSignal = (energy > dynamicEnergyThreshold * 1.8f) &&
                               (amplitude > dynamicAmplitudeThreshold * 1.8f);
            if (strongSignal) {
                return true;  // 较强信号可以触发
            }
        }
    }

    // 单一特征强信号检测（适应各种语音模式）
    bool singleStrongSignal = (energy > dynamicEnergyThreshold * 2.2f) ||
                             (amplitude > dynamicAmplitudeThreshold * 2.2f);
    if (singleStrongSignal) {
        return true;  // 单一特征较强即可触发
    }
    
    // 在语音活跃状态下使用更宽松的条件，保护连续音
    if (state == VAD_STATE_VOICE_ACTIVE || state == VAD_STATE_VOICE_HANGOVER) {
        // 语音状态下，只需要能量OR振幅满足即可（更宽松）
        if (energyDetected || amplitudeDetected) {
            return true;
        }
        // 或者有一个特征较强（降低要求）
        bool moderateSignal = (energy > dynamicEnergyThreshold * 1.3f) ||
                             (amplitude > dynamicAmplitudeThreshold * 1.3f);
        if (moderateSignal) {
            return true;  // 连续音保护：较低要求即可维持
        }
    }
    
    return false;
}

void VADEnhanced::updateNoiseEstimate(int32_t energy, int16_t amplitude, int32_t zeroCrossings) {
    // 使用指数移动平均更新噪声估计
    estimatedNoiseLevel = (int32_t)(
        (1.0f - VAD_NOISE_ADAPT_RATE) * estimatedNoiseLevel +
        VAD_NOISE_ADAPT_RATE * energy
    );
    
    estimatedNoisePeak = (int16_t)(
        (1.0f - VAD_NOISE_ADAPT_RATE) * estimatedNoisePeak +
        VAD_NOISE_ADAPT_RATE * amplitude
    );
    
    estimatedZeroCrossing = (int32_t)(
        (1.0f - VAD_NOISE_ADAPT_RATE) * estimatedZeroCrossing +
        VAD_NOISE_ADAPT_RATE * zeroCrossings
    );
}

uint32_t VADEnhanced::getCurrentTime() {
    return millis();
}
