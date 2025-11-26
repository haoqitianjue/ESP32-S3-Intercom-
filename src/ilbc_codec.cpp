#include "ilbc_codec.h"

// 全局编解码器实例
iLBCEncode* ilbcEncoder = nullptr;
iLBCDecode* ilbcDecoder = nullptr;

/**
 * 初始化iLBC编解码器
 * @return 成功返回true，失败返回false
 */
bool initILBCCodecs() {
    Serial.println("  初始化iLBC编码器...");

    try {

        // 创建编码器实例 (20ms模式)
        ilbcEncoder = new iLBCEncode(ms20);
        if (!ilbcEncoder) {
            Serial.println("❌ iLBC编码器内存分配失败");
            return false;
        }
        
        Serial.println("  初始化iLBC解码器...");
        
        // 创建解码器实例 (20ms模式，启用增强器)
        ilbcDecoder = new iLBCDecode(ms20, true);
        if (!ilbcDecoder) {
            Serial.println("❌ iLBC解码器内存分配失败");
            delete ilbcEncoder;
            ilbcEncoder = nullptr;
            return false;
        }
        
        Serial.println("✅ iLBC编解码器初始化成功");
        Serial.printf("   - 20ms模式: %d样本 -> %d字节\n",
                     ilbcEncoder->getSamples(), ilbcEncoder->getEncodedBytes());
        
        return true;
        
    } catch (const std::exception& e) {
        Serial.printf("❌ iLBC编解码器初始化异常: %s\n", e.what());
        endILBCCodecs();
        return false;
    } catch (...) {
        Serial.println("❌ iLBC编解码器初始化发生未知异常");
        endILBCCodecs();
        return false;
    }
}

/**
 * 清理iLBC编解码器资源
 */
void endILBCCodecs() {
    if (ilbcEncoder) {
        delete ilbcEncoder;
        ilbcEncoder = nullptr;
    }
    if (ilbcDecoder) {
        delete ilbcDecoder;
        ilbcDecoder = nullptr;
    }
    Serial.println("iLBC编解码器资源已清理");
}

/**
 * iLBC编码：160个PCM样本 -> 38字节iLBC数据
 * @param pcmData 输入的PCM数据（160个int16_t样本）
 * @param ilbcData 输出的iLBC数据（38字节）
 * @return 编码成功返回38，失败返回0
 */
size_t encodeILBC(const int16_t* pcmData, uint8_t* ilbcData) {
    // 添加延迟以确保编码器完全初始化
    static bool firstCall = true;
    if (firstCall) {
        delay(50);
        firstCall = false;
    }

    if (!ilbcEncoder) {
        return 0; // 静默失败，减少串口输出
    }
    if (!pcmData) {
        Serial.println("❌ PCM数据为空");
        return 0;
    }
    if (!ilbcData) {
        Serial.println("❌ iLBC输出缓冲区为空");
        return 0;
    }

    try {
        // 检查编码器状态
        int expectedSamples = ilbcEncoder->getSamples();
        int expectedBytes = ilbcEncoder->getEncodedBytes();

        if (expectedSamples != 160 || expectedBytes != 38) {
            return 0; // 静默失败，避免串口输出
        }

        // 临时移除调试信息以减少栈消耗

        // 调用iLBC编码器
        int encodedBytes = ilbcEncoder->encode((int16_t*)pcmData, ilbcData);

        if (encodedBytes == 38) {
            return 38;
        } else {
            // 简化错误信息以减少栈消耗
            return 0;
        }

    } catch (...) {
        // 简化异常处理以减少栈消耗
        return 0;
    }
}

/**
 * iLBC解码：38字节iLBC数据 -> 160个PCM样本
 * @param ilbcData 输入的iLBC数据（38字节）
 * @param pcmData 输出的PCM数据（160个int16_t样本）
 * @return 解码成功返回160，失败返回0
 */
size_t decodeILBC(const uint8_t* ilbcData, int16_t* pcmData) {
    if (!ilbcDecoder || !ilbcData || !pcmData) {
        return 0;
    }

    try {
        // 调用iLBC解码器
        int decodedBytes = ilbcDecoder->decode((uint8_t*)ilbcData, pcmData, MODE_NORMAL);

        // decodedBytes是字节数，需要转换为样本数
        int decodedSamples = decodedBytes / sizeof(int16_t);

        if (decodedSamples == 160) {
            return 160;
        } else {
            return 0; // 静默失败，避免串口输出
        }

    } catch (...) {
        return 0; // 静默失败，避免串口输出
    }
}
