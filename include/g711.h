/**
 * ESP32-S3 无线音频传输系统 - G.711编解码头文件 (g711.h)
 * 
 * 【文件说明】
 * 本头文件定义了G.711音频压缩标准的A-law和μ-law编解码接口，
 * 用于音频数据的压缩和解压缩，减少无线传输的带宽需求。
 * 
 * 【主要功能接口】
 * 1. 单样本编解码函数:
 *    - linear2alaw() [行38]: 将16位PCM转换为A-law格式
 *    - alaw2linear() [行47]: 将A-law格式转换为16位PCM
 *    - linear2ulaw() [行56]: 将16位PCM转换为μ-law格式
 *    - ulaw2linear() [行65]: 将μ-law格式转换为16位PCM
 * 
 * 2. 批量处理函数:
 *    - g711a_encode() [行74]: 批量A-law编码
 *    - g711a_decode() [行84]: 批量A-law解码
 *    - g711u_encode() [行94]: 批量μ-law编码
 *    - g711u_decode() [行104]: 批量μ-law解码
 * 
 * 3. DSP优化函数:
 *    - g711a_encode_dsp() [行114]: 使用DSP优化的A-law编码
 *    - g711u_encode_dsp() [行124]: 使用DSP优化的μ-law编码
 * 
 * 4. 工具函数:
 *    - g711_init_tables() [行134]: 初始化查找表
 * 
 * 【配置选项】
 * - USE_LOOKUP_TABLES [行35]: 控制是否使用查表法加速
 *   设为1时使用预计算表，提高速度；设为0时使用实时计算，节省内存
 * 
 * 【压缩效果】
 * G.711提供2:1的压缩比，将16位PCM压缩为8位数据。
 * 虽然有一定的质量损失，但对人声频段(300-3400Hz)的保真度很高，
 * 适合实时语音通信应用。
 * 
 * 本接口设计简洁直观，既可以单独处理单个样本，也可以批量处理
 * 整个音频块，满足不同场景的需求。
 */

/*
* G711 音频编解码库 - 优化版本 for ESP32-S3
*
* 这个库实现了A-law和μ-law两种音频压缩算法，就像是音频世界的两种减肥秘方！
* 可以将16位PCM音频数据(胖胖的音频)压缩成8位数据(苗条的音频)，
* 实现2:1的压缩比，同时保持较好的语音质量(保留肌肉，减掉脂肪)。
*/

#ifndef G711_H
#define G711_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// G711编解码基本常量 - 这些就像烹饪食谱中的基本配料
#define SIGN_BIT    (0x80)      /* A-law字节的符号位 - 相当于食谱中的盐 */
#define QUANT_MASK  (0xf)       /* 量化字段掩码 - 提取最低4位，像过滤咖啡渣 */
#define NSEGS       (8)         /* A-law分段数 - 分8个档次的辣度 */
#define SEG_SHIFT   (4)         /* 分段号左移位数 - 调整香料比例 */
#define SEG_MASK    (0x70)      /* 分段字段掩码 - 提取分段号，像选择调味料 */
#define BIAS        (0x84)      /* 线性编码的偏置值 - 调整基础口味 */

// 压缩格式类型 - 就像选择减肥方案：低碳还是低脂？
#define FORMAT_ALAW 0           /* A-law格式 - 欧洲风格减肥法 */
#define FORMAT_ULAW 1           /* μ-law格式 - 美式减肥法 */

// 控制优化策略的宏 - 决定是用算法计算还是查表
#define USE_LOOKUP_TABLES 1     /* 使用查找表加速转换 - 像是用备好的食谱而不是现场计算配方 */

/**
 * 将线性PCM值转换为A-law压缩格式
 * 
 * 就像把一杯大水(16位)浓缩成一小杯浓缩液(8位)
 * 
 * @param pcm_val 16位有符号线性PCM值（-32768到32767）
 * @return 8位无符号A-law压缩值
 */
unsigned char linear2alaw(int pcm_val);

/**
 * 将线性PCM值转换为μ-law压缩格式
 * 
 * 另一种浓缩方法，口味略有不同
 * 
 * @param pcm_val 16位有符号线性PCM值（-32768到32767）
 * @return 8位无符号μ-law压缩值
 */
unsigned char linear2ulaw(int pcm_val);

/**
 * 将A-law压缩值转换回线性PCM格式
 * 
 * 把浓缩液重新兑成一杯水
 * 
 * @param a_val 8位无符号A-law压缩值
 * @return 16位有符号线性PCM值
 */
int16_t alaw2linear(unsigned char a_val);

/**
 * 将μ-law压缩值转换回线性PCM格式
 * 
 * 另一种方法把浓缩液兑回原状
 * 
 * @param u_val 8位无符号μ-law压缩值
 * @return 16位有符号线性PCM值
 */
int16_t ulaw2linear(unsigned char u_val);

/**
 * 批量解码A-law压缩数据到线性PCM
 * 
 * 一次处理一整批数据，像流水线作业
 * 
 * @param amp 输出PCM数据缓冲区
 * @param g711a_data 输入A-law压缩数据缓冲区
 * @param g711a_bytes 输入缓冲区的字节数
 * @return 产生的PCM样本字节数
 */
int g711a_decode(int16_t amp[], const unsigned char g711a_data[], int g711a_bytes);

/**
 * 批量解码μ-law压缩数据到线性PCM
 * 
 * @param amp 输出PCM数据缓冲区
 * @param g711u_data 输入μ-law压缩数据缓冲区
 * @param g711u_bytes 输入缓冲区的字节数
 * @return 产生的PCM样本字节数
 */
int g711u_decode(int16_t amp[], const unsigned char g711u_data[], int g711u_bytes);

/**
 * 批量编码线性PCM数据到A-law压缩格式
 * 
 * @param g711_data 输出A-law压缩数据缓冲区
 * @param amp 输入PCM数据缓冲区
 * @param len 输入PCM样本数量
 * @return 产生的压缩数据字节数
 */
int g711a_encode(unsigned char g711_data[], const int16_t amp[], int len);

/**
 * 批量编码线性PCM数据到μ-law压缩格式
 * 
 * @param g711_data 输出μ-law压缩数据缓冲区
 * @param amp 输入PCM数据缓冲区
 * @param len 输入PCM样本数量
 * @return 产生的压缩数据字节数
 */
int g711u_encode(unsigned char g711_data[], const int16_t amp[], int len);

/**
 * DSP优化版本：批量解码A-law数据
 * 利用ESP32-S3的DSP指令集加速处理，像是给引擎装了涡轮增压器
 */
int g711a_decode_dsp(int16_t amp[], const unsigned char g711a_data[], int g711a_bytes);

/**
 * DSP优化版本：批量解码μ-law数据
 * 利用ESP32-S3的DSP指令集加速处理
 */
int g711u_decode_dsp(int16_t amp[], const unsigned char g711u_data[], int g711u_bytes);

/**
 * DSP优化版本：批量编码PCM数据到A-law格式
 * 利用ESP32-S3的DSP指令集加速处理
 */
int g711a_encode_dsp(unsigned char g711_data[], const int16_t amp[], int len);

/**
 * DSP优化版本：批量编码PCM数据到μ-law格式
 * 利用ESP32-S3的DSP指令集加速处理
 */
int g711u_encode_dsp(unsigned char g711_data[], const int16_t amp[], int len);

/**
 * 初始化G711查找表以加速编解码
 * 
 * 预先计算所有可能的转换结果，存入表中以后直接查表，不用再算
 * 就像提前做好一周的饭菜，需要时直接拿出来热一下
 */
void g711_init_tables(void);

#ifdef __cplusplus
}
#endif

#endif /* G711_H */

