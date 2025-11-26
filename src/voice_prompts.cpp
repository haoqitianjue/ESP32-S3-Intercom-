#include "voice_prompts.h"
#include "voice_data.h"
#include <Arduino.h>
#include <driver/i2s.h>

// 定义语音提示数组
const VoicePrompt voicePrompts[VOICE_PROMPT_COUNT] = {
    { channel1_audio, channel1_audio_size, 8000 },
    { channel2_audio, channel2_audio_size, 8000 },
    { channel3_audio, channel3_audio_size, 8000 },
    { channel4_audio, channel4_audio_size, 8000 },
    { channel5_audio, channel5_audio_size, 8000 },
    { channel6_audio, channel6_audio_size, 8000 },
    { channel7_audio, channel7_audio_size, 8000 },
    { channel8_audio, channel8_audio_size, 8000 },
    { channel9_audio, channel9_audio_size, 8000 },
    { channel10_audio, channel10_audio_size, 8000 },
    { channel11_audio, channel11_audio_size, 8000 },
    { channel12_audio, channel12_audio_size, 8000 },
    { channel13_audio, channel13_audio_size, 8000 },
    { channel14_audio, channel14_audio_size, 8000 },
    { channel15_audio, channel15_audio_size, 8000 },
    { channel16_audio, channel16_audio_size, 8000 },
    { low_battery_audio, low_battery_audio_size, 8000 },
    { sidetone_on_audio, sidetone_on_audio_size, 8000 },   // 自听音开启语音提示
    { sidetone_off_audio, sidetone_off_audio_size, 8000 }, // 自听音关闭语音提示
    { volume_up_audio, volume_up_audio_size, 8000 },       // 音量增加提示音
    { volume_down_audio, volume_down_audio_size, 8000 },   // 音量减少提示音
    { startup_voice_audio, startup_voice_audio_size, 8000 } // 开机词语音提示
};

// 初始化语音提示模块
void initVoicePrompts() {
    // 可以在这里添加初始化代码，如果需要的话
    Serial.println("✅ 语音提示模块已初始化");
}

// 播放语音提示
void playVoicePrompt(VoicePromptIndex index) {
    if (index < 0 || index >= VOICE_PROMPT_COUNT) {
        return;  // 无效索引
    }

    const VoicePrompt* prompt = &voicePrompts[index];

    // 暂停当前音频处理
    // 注意：在实际应用中，您可能需要更复杂的逻辑来处理音频暂停和恢复

    // 创建临时缓冲区并应用音量调整
    int16_t* tempBuffer = new int16_t[prompt->size];
    if (tempBuffer == NULL) {
        // 内存分配失败，直接使用原始数据播放
        size_t bytesWritten = 0;
        i2s_write(I2S_NUM_1, prompt->data, prompt->size * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    } else {
        // 应用音量缩减 - 根据提示音类型调整音量
        float volumeScale;
        if (index == VOICE_VOLUME_UP || index == VOICE_VOLUME_DOWN) {
            volumeScale = 0.1f;  // 音量提示音使用10%音量，简洁不干扰
        } else if (index == VOICE_STARTUP) {
            volumeScale = 0.1f;  // 开机词使用10%音量，统一标准
        } else {
            volumeScale = 0.1f;  // 其他语音提示使用10%音量
        }

        // 复制并调整音量
        for (size_t i = 0; i < prompt->size; i++) {
            int32_t sample = prompt->data[i] * volumeScale;
            tempBuffer[i] = (int16_t)sample;  // 自动截断到16位范围
        }

        // 播放调整后的音频
        size_t bytesWritten = 0;
        i2s_write(I2S_NUM_1, tempBuffer, prompt->size * sizeof(int16_t), &bytesWritten, portMAX_DELAY);

        // 释放临时缓冲区
        delete[] tempBuffer;
    }

    // 等待播放完成（简单方法，实际应用中可能需要更复杂的逻辑）
    delay(prompt->size * 1000 / prompt->sampleRate);
}
