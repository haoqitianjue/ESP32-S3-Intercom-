#ifndef VOICE_PROMPTS_H
#define VOICE_PROMPTS_H

#include <stdint.h>
#include <stddef.h>

// 语音提示类型
typedef struct {
    const int16_t* data;    // 音频数据指针
    size_t size;            // 样本数量
    uint32_t sampleRate;    // 采样率
} VoicePrompt;

// 语音提示索引
enum VoicePromptIndex {
    VOICE_CHANNEL_1 = 0,
    VOICE_CHANNEL_2,
    VOICE_CHANNEL_3,
    VOICE_CHANNEL_4,
    VOICE_CHANNEL_5,
    VOICE_CHANNEL_6,
    VOICE_CHANNEL_7,
    VOICE_CHANNEL_8,
    VOICE_CHANNEL_9,
    VOICE_CHANNEL_10,
    VOICE_CHANNEL_11,
    VOICE_CHANNEL_12,
    VOICE_CHANNEL_13,
    VOICE_CHANNEL_14,
    VOICE_CHANNEL_15,
    VOICE_CHANNEL_16,
    VOICE_LOW_BATTERY,
    VOICE_SIDETONE_ON,      // 自听音开启语音提示
    VOICE_SIDETONE_OFF,     // 自听音关闭语音提示
    VOICE_VOLUME_UP,        // 音量增加提示音
    VOICE_VOLUME_DOWN,      // 音量减少提示音
    VOICE_STARTUP,          // 开机词语音提示（10个字）
    VOICE_PROMPT_COUNT
};

// 播放语音提示的函数
void playVoicePrompt(VoicePromptIndex index);

// 初始化语音提示模块
void initVoicePrompts();

#endif // VOICE_PROMPTS_H
