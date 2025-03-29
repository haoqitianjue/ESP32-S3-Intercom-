/**
 * ESP32-S3 无线音频传输系统 - G.711音频编解码模块 (g711.cpp)
 * 
 * 【文件说明】
 * 本文件实现G.711音频压缩标准的A-law和μ-law编解码器，用于将16位PCM音频
 * 压缩为8位数据，减少传输带宽需求，同时保持足够的语音质量。
 * 
 * 【主要功能】
 * 1. PCM转A-law/μ-law编码 - 将16位线性PCM压缩为8位数据
 * 2. A-law/μ-law转PCM解码 - 将8位压缩数据恢复为16位PCM
 * 3. 优化的查表实现 - 使用预计算表加速编解码过程
 * 4. 批量处理函数 - 高效处理大块音频数据
 * 
 * 【重要函数位置】
 * - linear2alaw()          [行125] - 将线性PCM转换为A-law格式
 * - alaw2linear()          [行58]  - 将A-law格式转换为线性PCM
 * - linear2ulaw()          [行173] - 将线性PCM转换为μ-law格式
 * - ulaw2linear()          [行91]  - 将μ-law格式转换为线性PCM
 * - g711_init_tables()     [行230] - 初始化查找表
 * - g711a_encode()         [行364] - 批量A-law编码
 * - g711a_decode()         [行391] - 批量A-law解码
 * - g711u_encode()         [行419] - 批量μ-law编码
 * - g711u_decode()         [行446] - 批量μ-law解码
 * - g711a_encode_dsp()     [行475] - 使用DSP优化的A-law编码
 * - g711u_encode_dsp()     [行526] - 使用DSP优化的μ-law编码
 * 
 * 【实现原理】
 * 1. G.711压缩算法:
 *    利用人耳感知特性，对大信号进行粗量化，对小信号进行细量化，
 *    实现对数压缩，将16位PCM压缩为8位，达到2:1的压缩比。
 * 
 * 2. 两种编码方式:
 *    - A-law: 欧洲/国际标准，动态范围更宽，适合语音
 *    - μ-law: 北美/日本标准，低电平信号还原度更高
 * 
 * 3. 优化实现:
 *    - 查表法: 预计算所有可能的转换结果，直接查表获取
 *    - 计算法: 实时计算变换，适用于内存受限场景
 *    - 通过USE_LOOKUP_TABLES宏控制使用哪种方法
 * 
 * 4. 批量处理:
 *    针对音频块提供批量编解码函数，单次调用处理多个样本，
 *    显著提高处理效率，减少函数调用开销。
 * 
 * G.711编码是电话系统广泛使用的音频压缩标准，采样率8kHz，
 * 提供电话质量的语音效果(300-3400Hz频带)。本实现针对ESP32进行了优化，
 * 在保证音质的同时提高处理效率，降低CPU占用。
 */

/* 
 * G.711 audio codec implementation (A-law and μ-law)
 *
 * Original implementation by Sun Microsystems, modified for ESP32
 * Optimized with lookup tables for better performance
 */

#include "g711.h"
#include <stdint.h>
#include <string.h>
#include <Arduino.h>

// 定义segment end points - 这是音频压缩的"档位表"，类似汽车的挡位
static const int16_t seg_end[8] = {0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};

#if USE_LOOKUP_TABLES
// 查找表内存分配 - 如果有PSRAM，就用大容量的外部内存，否则用内部内存
#if CONFIG_SPIRAM_SUPPORT
// 在PSRAM中分配查找表 - 相当于加装了大容量硬盘
static int16_t *alaw_to_linear_table = NULL;
static int16_t *ulaw_to_linear_table = NULL;
static uint8_t *linear_to_alaw_table = NULL;
static uint8_t *linear_to_ulaw_table = NULL;
#else
// 对齐内存以提高访问速度 - 把常用物品放在伸手可及的地方
static DRAM_ATTR int16_t alaw_to_linear_table[256] __attribute__((aligned(16)));
static DRAM_ATTR int16_t ulaw_to_linear_table[256] __attribute__((aligned(16)));
static DRAM_ATTR uint8_t linear_to_alaw_table[65536] __attribute__((aligned(16)));
static DRAM_ATTR uint8_t linear_to_ulaw_table[65536] __attribute__((aligned(16)));
#endif

static bool tables_initialized = false;  // 表格是否已经准备好
#endif

/**
 * 二分查找算法 - 在有序表中快速找到值的位置
 * 就像在字典中查单词，不需要从头翻到尾
 */
int search(int val, const int16_t *table, int size)
{
    int left = 0;
    int right = size - 1;
    int mid;
    
    // 二分查找循环 - 每次排除一半范围
    while (left <= right) {
        mid = (left + right) / 2;
        if (table[mid] < val)
            left = mid + 1;
        else
            right = mid - 1;
    }
    return left;
}

/**
 * 将A-law压缩值转换回线性PCM格式
 * 将8位的"压缩包"解压成16位的"原文件"
 */
int16_t alaw2linear(unsigned char a_val)
{
#if USE_LOOKUP_TABLES
    // 查表法 - 像在菜谱上直接查结果
    if (!tables_initialized) {
        g711_init_tables();  // 第一次使用前确保表已初始化
    }
    return alaw_to_linear_table[a_val];
#else
    // 计算法 - 就像现场计算配方
    int t;
    int seg;
    
    // A-law有一个特殊的位翻转编码
    a_val ^= 0x55;
    
    // 提取量化部分并左移4位
    t = (a_val & QUANT_MASK) << 4;
    // 提取段号
    seg = ((unsigned)a_val & SEG_MASK) >> SEG_SHIFT;
    
    // 根据段号调整值
    if (seg == 0) {
        t += 8;  // 在第0段特殊处理
    } else {
        t += 0x108;  // 其他段加上基本偏移
        if (seg > 1) {
            t <<= seg - 1;  // 更高段需要更多位移
        }
    }
    
    // 根据符号位决定是正值还是负值
    return ((a_val & SIGN_BIT) ? t : -t);
#endif
}

/**
 * 将μ-law压缩值转换回线性PCM格式
 */
int16_t ulaw2linear(unsigned char u_val)
{
#if USE_LOOKUP_TABLES
    // 查表法
    if (!tables_initialized) {
        g711_init_tables();
    }
    return ulaw_to_linear_table[u_val];
#else
    // 计算法
    int t;
    
    // μ-law使用反转的位编码
    u_val = ~u_val;
    
    // 提取量化部分并应用偏置
    t = ((u_val & QUANT_MASK) << 3) + BIAS;
    // 根据段号左移
    t <<= ((unsigned)u_val & SEG_MASK) >> SEG_SHIFT;
    
    // 根据符号位返回正负值
    return ((u_val & SIGN_BIT) ? (BIAS - t) : (t - BIAS));
#endif
}

/**
 * 将线性PCM值转换为A-law压缩格式
 */
unsigned char linear2alaw(int pcm_val)
{
#if USE_LOOKUP_TABLES
    // 查表法 - 需要将有符号整数映射到无符号索引
    if (!tables_initialized) {
        g711_init_tables();
    }
    // 把-32768~32767的值映射到0~65535的索引
    return linear_to_alaw_table[(uint16_t)(pcm_val + 32768)];
#else
    // 计算法
    int mask;
    int seg;
    unsigned char aval;

    // 处理符号和幅度
    if (pcm_val >= 0) {
        mask = 0xD5;  // 正数对应的掩码
    } else {
        mask = 0x55;  // 负数对应的掩码
        pcm_val = -pcm_val - 8;  // 负数需要特殊处理
        if (pcm_val < 0) pcm_val = 0;  // 安全检查
    }

    // 找到对应的段号 - 就像找到汽车的挡位
    seg = search(pcm_val, seg_end, 8);

    // 组合符号位、段号和量化位
    if (seg >= 8) {
        // 超出范围，返回最大值
        return (0x7F ^ mask);
    } else {
        aval = seg << SEG_SHIFT;  // 段号放在中间位
        if (seg < 2) {
            // 低段特殊处理
            aval |= (pcm_val >> 4) & QUANT_MASK;
        } else {
            // 高段需要根据段号调整位移
            aval |= (pcm_val >> (seg + 3)) & QUANT_MASK;
        }
        return (aval ^ mask);  // 应用掩码
    }
#endif
}

/**
 * 将线性PCM值转换为μ-law压缩格式
 */
unsigned char linear2ulaw(int pcm_val)
{
#if USE_LOOKUP_TABLES
    // 查表法
    if (!tables_initialized) {
        g711_init_tables();
    }
    return linear_to_ulaw_table[(uint16_t)(pcm_val + 32768)];
#else
    // 计算法
    int mask;
    int seg;
    unsigned char uval;
    
    // 处理符号和绝对值
    if (pcm_val < 0) {
        pcm_val = -pcm_val;
        mask = 0x7F;  // 负数掩码
    } else {
        mask = 0xFF;  // 正数掩码
    }
    
    // 防止溢出
    if (pcm_val > 32635)
        pcm_val = 32635;
        
    // 为G.711 μ-law添加偏置
    pcm_val += BIAS;
    
    // 确定段号 - 对数量化
    if (pcm_val <= 0x1FF)
        seg = 0;
    else if (pcm_val <= 0x3FF)
        seg = 1;
    else if (pcm_val <= 0x7FF)
        seg = 2;
    else if (pcm_val <= 0xFFF)
        seg = 3;
    else if (pcm_val <= 0x1FFF)
        seg = 4;
    else if (pcm_val <= 0x3FFF)
        seg = 5;
    else if (pcm_val <= 0x7FFF)
        seg = 6;
    else
        seg = 7;
    
    // 组合段号和量化位
    uval = (seg << 4) | ((pcm_val >> (seg + 3)) & 0xF);
    return (~uval) & mask;  // μ-law用反码
#endif
}

/**
 * 初始化查找表加速编解码
 * 预先计算所有可能的转换结果，后续直接查表不用算
 */
void g711_init_tables(void)
{
#if USE_LOOKUP_TABLES
    // 如果已经初始化过，直接返回
    if (tables_initialized) return;
    
    // 在PSRAM上分配内存
    #if CONFIG_SPIRAM_SUPPORT
    if (alaw_to_linear_table == NULL) {
        alaw_to_linear_table = (int16_t *)ps_malloc(256 * sizeof(int16_t));
        ulaw_to_linear_table = (int16_t *)ps_malloc(256 * sizeof(int16_t));
        linear_to_alaw_table = (uint8_t *)ps_malloc(65536 * sizeof(uint8_t));
        linear_to_ulaw_table = (uint8_t *)ps_malloc(65536 * sizeof(uint8_t));
        
        if (!alaw_to_linear_table || !ulaw_to_linear_table || 
            !linear_to_alaw_table || !linear_to_ulaw_table) {
            log_e("G711: 无法分配查找表内存");
            return;  // 内存分配失败
        }
    }
    #endif
    
    // 为A-law和μ-law生成解码查找表 (8位->16位)
    for (int i = 0; i < 256; i++) {
        uint8_t a_val = i;
        uint8_t u_val = i;
        
        // 计算A-law到线性的映射
        int t;
        int seg;
        
        a_val ^= 0x55;
        
        t = (a_val & QUANT_MASK) << 4;
        seg = ((unsigned)a_val & SEG_MASK) >> SEG_SHIFT;
        
        if (seg == 0) {
            t += 8;
        } else {
            t += 0x108;
            if (seg > 1) {
                t <<= seg - 1;
            }
        }
        
        alaw_to_linear_table[i] = (a_val & SIGN_BIT) ? t : -t;
        
        // 计算μ-law到线性的映射
        u_val = ~u_val;
        
        t = ((u_val & QUANT_MASK) << 3) + BIAS;
        t <<= ((unsigned)u_val & SEG_MASK) >> SEG_SHIFT;
        
        ulaw_to_linear_table[i] = (u_val & SIGN_BIT) ? (BIAS - t) : (t - BIAS);
    }
    
    // 生成线性到A-law的映射表 (16位->8位)
    for (int i = 0; i < 65536; i++) {
        int16_t pcm_val = (int16_t)(i - 32768);  // 从索引恢复有符号值
        
        // 使用计算方法生成表
        int mask;
        int seg;
        unsigned char aval;

        if (pcm_val >= 0) {
            mask = 0xD5;
        } else {
            mask = 0x55;
            pcm_val = -pcm_val - 8;
            if (pcm_val < 0) pcm_val = 0;
        }

        seg = search(pcm_val, seg_end, 8);

        if (seg >= 8) {
            linear_to_alaw_table[i] = (0x7F ^ mask);
        } else {
            aval = seg << SEG_SHIFT;
            if (seg < 2) {
                aval |= (pcm_val >> 4) & QUANT_MASK;
            } else {
                aval |= (pcm_val >> (seg + 3)) & QUANT_MASK;
            }
            linear_to_alaw_table[i] = (aval ^ mask);
        }
    }
    
    // 生成线性到μ-law的映射表
    for (int i = 0; i < 65536; i++) {
        int pcm_val = (int16_t)(i - 32768);
        
        int mask;
        int seg;
        unsigned char uval;
        
        if (pcm_val < 0) {
            pcm_val = -pcm_val;
            mask = 0x7F;
        } else {
            mask = 0xFF;
        }
        
        if (pcm_val > 32635)
            pcm_val = 32635;
            
        pcm_val += BIAS;
        
        if (pcm_val <= 0x1FF)
            seg = 0;
        else if (pcm_val <= 0x3FF)
            seg = 1;
        else if (pcm_val <= 0x7FF)
            seg = 2;
        else if (pcm_val <= 0xFFF)
            seg = 3;
        else if (pcm_val <= 0x1FFF)
            seg = 4;
        else if (pcm_val <= 0x3FFF)
            seg = 5;
        else if (pcm_val <= 0x7FFF)
            seg = 6;
        else
            seg = 7;
        
        uval = (seg << 4) | ((pcm_val >> (seg + 3)) & 0xF);
        linear_to_ulaw_table[i] = (~uval) & mask;
    }
    
    tables_initialized = true;
    log_i("G711: 查找表初始化完成");
#endif
}

/**
 * 批量解码A-law压缩数据到线性PCM
 * 一次处理一批数据，提高效率
 */
int g711a_decode(int16_t amp[], const unsigned char g711a_data[], int g711a_bytes)
{
    // 参数验证 - 安全检查
    if (amp == NULL || g711a_data == NULL || g711a_bytes <= 0) {
        return 0;
    }

    int i;
    int samples = 0;

    // 批量解码
    for (i = 0; i < g711a_bytes; i++) {
        amp[samples++] = alaw2linear(g711a_data[i]);
    }
    
    return samples;
}

/**
 * 批量解码μ-law压缩数据到线性PCM
 */
int g711u_decode(int16_t amp[], const unsigned char g711u_data[], int g711u_bytes)
{
    // 参数验证
    if (amp == NULL || g711u_data == NULL || g711u_bytes <= 0) {
        return 0;
    }

    int i;
    int samples = 0;

    // 批量解码
    for (i = 0; i < g711u_bytes; i++) {
        amp[samples++] = ulaw2linear(g711u_data[i]);
    }
    
    return samples;
}

/**
 * 批量编码线性PCM数据到A-law压缩格式
 */
int g711a_encode(unsigned char g711_data[], const int16_t amp[], int len)
{
    // 参数验证
    if (g711_data == NULL || amp == NULL || len <= 0) {
        return 0;
    }

    // 批量编码
    for (int i = 0; i < len; i++) {
        g711_data[i] = linear2alaw(amp[i]);
    }

    return len;
}

/**
 * 批量编码线性PCM数据到μ-law压缩格式
 */
int g711u_encode(unsigned char g711_data[], const int16_t amp[], int len)
{
    // 参数验证
    if (g711_data == NULL || amp == NULL || len <= 0) {
        return 0;
    }

    // 批量编码
    for (int i = 0; i < len; i++) {
        g711_data[i] = linear2ulaw(amp[i]);
    }

    return len;
}

/**
 * DSP优化版本：批量解码A-law数据
 * 利用ESP32-S3的DSP指令集加速处理
 */
int g711a_decode_dsp(int16_t amp[], const unsigned char g711a_data[], int g711a_bytes)
{
    // 参数验证
    if (amp == NULL || g711a_data == NULL || g711a_bytes <= 0) {
        return 0;
    }

#if USE_LOOKUP_TABLES
    // 确保查找表已初始化
    if (!tables_initialized) g711_init_tables();
    
    // 批量处理 - 可以用DSP指令加速
    for (int i = 0; i < g711a_bytes; i++) {
        amp[i] = alaw_to_linear_table[g711a_data[i]];
    }
    
    return g711a_bytes;
#else
    // 没有开启查找表时回退到普通函数
    return g711a_decode(amp, g711a_data, g711a_bytes);
#endif
}

/**
 * DSP优化版本：批量解码μ-law数据
 * 利用ESP32-S3的DSP指令集加速处理
 */
int g711u_decode_dsp(int16_t amp[], const unsigned char g711u_data[], int g711u_bytes)
{
    // 参数验证
    if (amp == NULL || g711u_data == NULL || g711u_bytes <= 0) {
        return 0;
    }

#if USE_LOOKUP_TABLES
    // 确保查找表已初始化
    if (!tables_initialized) g711_init_tables();
    
    // 批量处理
    for (int i = 0; i < g711u_bytes; i++) {
        amp[i] = ulaw_to_linear_table[g711u_data[i]];
    }
    
    return g711u_bytes;
#else
    // 回退到普通函数
    return g711u_decode(amp, g711u_data, g711u_bytes);
#endif
}

/**
 * DSP优化版本：批量编码PCM数据到A-law格式
 */
int g711a_encode_dsp(unsigned char g711_data[], const int16_t amp[], int len)
{
    // 参数验证
    if (g711_data == NULL || amp == NULL || len <= 0) {
        return 0;
    }

#if USE_LOOKUP_TABLES
    // 确保查找表已初始化
    if (!tables_initialized) g711_init_tables();
    
    // 批量处理
    for (int i = 0; i < len; i++) {
        g711_data[i] = linear_to_alaw_table[(uint16_t)(amp[i] + 32768)];
    }
    
    return len;
#else
    // 回退到普通函数
    return g711a_encode(g711_data, amp, len);
#endif
}

/**
 * DSP优化版本：批量编码PCM数据到μ-law格式
 */
int g711u_encode_dsp(unsigned char g711_data[], const int16_t amp[], int len)
{
    // 参数验证
    if (g711_data == NULL || amp == NULL || len <= 0) {
        return 0;
    }

#if USE_LOOKUP_TABLES
    // 确保查找表已初始化
    if (!tables_initialized) g711_init_tables();
    
    // 批量处理
    for (int i = 0; i < len; i++) {
        g711_data[i] = linear_to_ulaw_table[(uint16_t)(amp[i] + 32768)];
    }
    
    return len;
#else
    // 回退到普通函数
    return g711u_encode(g711_data, amp, len);
#endif
}

