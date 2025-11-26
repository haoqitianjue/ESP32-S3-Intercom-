#ifndef ILBC_CODEC_H
#define ILBC_CODEC_H

#include <Arduino.h>
#include "iLBC.h"

/**
 * 简化的iLBC编解码器包装类
 * 提供与原ADPCM相同的接口，便于无缝替换
 */

// 全局编解码器实例
extern iLBCEncode* ilbcEncoder;
extern iLBCDecode* ilbcDecoder;

// 简化的编解码函数
bool initILBCCodecs();
void endILBCCodecs();
size_t encodeILBC(const int16_t* pcmData, uint8_t* ilbcData);
size_t decodeILBC(const uint8_t* ilbcData, int16_t* pcmData);

#endif // ILBC_CODEC_H
