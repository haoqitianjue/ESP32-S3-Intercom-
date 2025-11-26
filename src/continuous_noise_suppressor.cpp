/**
 * 连续噪声抑制器实现
 */

#include "continuous_noise_suppressor.h"
#include <Arduino.h>
#include <math.h>

ContinuousNoiseSuppressor::ContinuousNoiseSuppressor() 
    : noiseFloor(0.0f)
    , noiseVariance(0.0f)
    , noiseProfileFrames(0)
    , noiseProfileReady(false)
    , previousVadState(false)
    , vadActiveFrames(0)
    , suppressionFactor(1.0f)
    , adaptiveThreshold(0.0f) {
}

bool ContinuousNoiseSuppressor::init() {
    resetNoiseProfile();
    Serial.println("✅ 连续噪声抑制器初始化完成");
    return true;
}

void ContinuousNoiseSuppressor::processVoiceWithNoiseSuppression(int16_t* buffer, int size, bool vadActive) {
    // 检测VAD状态变化
    if (vadActive && !previousVadState) {
        // VAD刚刚触发，开始建立噪声模板
        resetNoiseProfile();
        vadActiveFrames = 0;
    }
    
    if (vadActive) {
        vadActiveFrames++;
        
        // 在VAD触发的前几帧快速建立噪声模板
        if (vadActiveFrames <= NOISE_PROFILE_FRAMES) {
            updateNoiseProfile(buffer, size);
        }
        
        // 如果噪声模板就绪，进行连续噪声抑制
        if (noiseProfileReady) {
            updateSuppressionParameters();
            applyContinuousNoiseSuppression(buffer, size);
        }
    } else {
        // VAD未触发时重置帧计数
        vadActiveFrames = 0;
    }
    
    previousVadState = vadActive;
}

void ContinuousNoiseSuppressor::updateNoiseProfile(int16_t* buffer, int size) {
    float frameEnergy = calculateFrameEnergy(buffer, size);
    float frameVariance = calculateFrameVariance(buffer, size, frameEnergy);
    
    if (noiseProfileFrames == 0) {
        // 第一帧，初始化
        noiseFloor = frameEnergy;
        noiseVariance = frameVariance;
    } else {
        // 累积平均
        float alpha = 1.0f / (noiseProfileFrames + 1);
        noiseFloor = noiseFloor * (1.0f - alpha) + frameEnergy * alpha;
        noiseVariance = noiseVariance * (1.0f - alpha) + frameVariance * alpha;
    }
    
    noiseProfileFrames++;
    
    // 检查是否可以建立稳定的噪声模板
    if (noiseProfileFrames >= NOISE_PROFILE_FRAMES) {
        // 判断噪声是否足够稳定（方差相对较小）
        float stabilityRatio = noiseVariance / (noiseFloor + 1.0f);
        if (stabilityRatio < NOISE_STABILITY_THRESHOLD) {
            noiseProfileReady = true;
            adaptiveThreshold = noiseFloor * VOICE_PROTECTION_RATIO;
        }
    }
}

void ContinuousNoiseSuppressor::applyContinuousNoiseSuppression(int16_t* buffer, int size) {
    int suppressedSamples = 0;
    int protectedSamples = 0;
    
    for (int i = 0; i < size; i++) {
        int16_t sample = buffer[i];
        int16_t absSample = abs(sample);
        
        // 判断当前采样点是否可能是噪声
        if (absSample <= adaptiveThreshold) {
            // 可能是噪声，应用抑制
            buffer[i] = (int16_t)(sample * suppressionFactor);
            suppressedSamples++;
        } else {
            // 可能是语音，轻微抑制保护语音质量
            float voiceProtectionFactor = MIN_SUPPRESSION_FACTOR;
            buffer[i] = (int16_t)(sample * voiceProtectionFactor);
            protectedSamples++;
        }
    }
    
    // 可选的调试信息（仅在需要时启用）
    #ifdef DEBUG_NOISE_SUPPRESSOR
    if (vadActiveFrames % 100 == 0) {
        Serial.printf("🔧 降噪: 抑制率%.1f%%, 系数%.2f\n",
                     (float)suppressedSamples * 100.0f / size, suppressionFactor);
    }
    #endif
}

void ContinuousNoiseSuppressor::updateSuppressionParameters() {
    // 根据VAD活跃时间动态调整抑制强度
    // 刚开始比较保守，随着时间推移可以更激进
    
    if (vadActiveFrames <= 3) {
        // 前3帧保守处理，避免切断语音起始
        suppressionFactor = MIN_SUPPRESSION_FACTOR;
    } else if (vadActiveFrames <= (3 + SUPPRESSION_RAMP_FRAMES)) {
        // 4-10帧逐渐增强抑制
        float progress = (vadActiveFrames - 3) / (float)SUPPRESSION_RAMP_FRAMES;
        suppressionFactor = MIN_SUPPRESSION_FACTOR + 
                          (BASE_SUPPRESSION_FACTOR - MIN_SUPPRESSION_FACTOR) * progress;
    } else {
        // 10帧后可以更激进
        suppressionFactor = BASE_SUPPRESSION_FACTOR;
        
        // 如果噪声很稳定，可以进一步增强抑制
        float stabilityRatio = noiseVariance / (noiseFloor + 1.0f);
        if (stabilityRatio < NOISE_STABILITY_THRESHOLD * 0.5f) {
            suppressionFactor = MAX_SUPPRESSION_FACTOR;
        }
    }
}

float ContinuousNoiseSuppressor::calculateFrameEnergy(int16_t* buffer, int size) {
    float energy = 0.0f;
    for (int i = 0; i < size; i++) {
        energy += abs(buffer[i]);
    }
    return (size > 0) ? energy / size : 0.0f;
}

float ContinuousNoiseSuppressor::calculateFrameVariance(int16_t* buffer, int size, float mean) {
    float variance = 0.0f;
    for (int i = 0; i < size; i++) {
        float diff = abs(buffer[i]) - mean;
        variance += diff * diff;
    }
    return (size > 0) ? variance / size : 0.0f;
}

void ContinuousNoiseSuppressor::resetNoiseProfile() {
    noiseFloor = 0.0f;
    noiseVariance = 0.0f;
    noiseProfileFrames = 0;
    noiseProfileReady = false;
    vadActiveFrames = 0;
    suppressionFactor = 1.0f;
    adaptiveThreshold = 0.0f;
}
