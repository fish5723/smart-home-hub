/**
 * @file app_mq_calib.h
 * @brief MQ 传感器标定模块 — 状态机 + Rs/R0 + 浓度估算
 *
 * 阶段 1（T-02）：替换原无物理意义的 `adc*100/4095`，输出标定后的 Rs/R0 与浓度。
 *
 * ## 标定流程
 *   上电 → WARMUP(60s) → AGING(24h) → R0_SAMPLING → READY
 *
 * ## 物理模型
 *   MQ 系列为化学电阻式传感器，敏感体电阻 Rs 随气体浓度升高而降低。
 *   模块内部有负载电阻 RL（典型 10kΩ）与 Rs 串联分压：
 *
 *     Vout = Vc × RL / (Rs + RL)   →   Rs = RL × (Vc / Vout - 1)
 *
 *   其中 Vc = 5V（传感器供电），Vout 为模块输出（经分压后到 ADC 引脚）。
 *   R0 = 洁净空气中的 Rs 值（需 24h 老化稳定后采样）。
 *   Rs/R0 与气体浓度在 log-log 坐标下近似线性（查表插值）。
 *
 * ## 校准要求
 *   1. 24h 连续通电（老化）
 *   2. 洁净空气环境中采样 R0（通风良好，无目标气体泄漏）
 *   3. 若环境不符合"洁净空气"条件，标定结果会偏大或偏小
 *
 * 来源：本工程新建
 */
#ifndef APP_MQ_CALIB_H
#define APP_MQ_CALIB_H

#include "main.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  标定状态机
 *  ════════════════════════════════════════════════════ */
typedef enum {
    MQ_CALIB_WARMUP   = 0,    /**< 上电预热，传感器尚未稳定           */
    MQ_CALIB_AGING    = 1,    /**< 24h 老化进行中，R0 未采集         */
    MQ_CALIB_R0_READY = 2,    /**< 老化完成，可开始洁净空气标定       */
    MQ_CALIB_READY    = 3,    /**< 标定完成，正常输出 Rs/R0 与浓度   */
} mq_calib_state_t;

/* ════════════════════════════════════════════════════
 *  传感器类型
 *  ════════════════════════════════════════════════════ */
typedef enum {
    MQ_TYPE_MQ4   = 0,        /**< MQ-4  可燃气体（CH4）              */
    MQ_TYPE_MQ135 = 1,        /**< MQ-135 空气质量（NH3 / CO2 / 烟雾）*/
} mq_type_t;

/* ════════════════════════════════════════════════════
 *  单个 MQ 传感器的标定上下文
 *  ════════════════════════════════════════════════════ */
typedef struct {
    /* —— 配置（外部注入） —— */
    mq_type_t        type;             /**< 传感器型号                    */
    float            divider_ratio;    /**< 硬件分压比（引脚电压/传感器输出）*/
    uint16_t         load_resistor_ohm;/**< RL 负载电阻 (Ω)，默认 10000  */

    /* —— 运行时状态 —— */
    mq_calib_state_t state;            /**< 当前标定阶段                  */
    uint16_t         r0_adc;           /**< 洁净空气中 R0 对应的 ADC 原始值*/
    float            r0_mv;            /**< 洁净空气中 R0 对应的传感器输出(mV)*/
    float            r0_ohm;           /**< R0（Ω）：社区口径下 = 空气基线 Rs / 洁净空气比值，
                                        *   MQ-4 即 1000ppm CH4 下的 Rs               */
    float            rs_peak;          /**< 社区口径的空气基线：Rs 历史峰值
                                        *   （缓慢衰减），用于自动跟踪器件漂移 */
    uint8_t          r0_community;     /**< 1=使用社区口径（MQ-4 / MQ-135）：跳过
                                        *   24h 老化，R0 由基线自跟踪得到           */
    float            rs_r0;            /**< 当前 Rs/R0 比值               */
    float            concentration;    /**< 估算浓度 (ppm)，标定完成后有效 */
    uint32_t         power_on_ms;      /**< 累计通电时间 (ms)             */
    uint8_t          sample_count;     /**< R0 采样计数器                 */
    uint32_t         r0_sum_adc;       /**< R0 采样累加和（ADC 原始值）   */

    /* —— 上电预热标记 —— */
    uint8_t          warmup_done;      /**< 预热阶段是否已完成            */
    uint8_t          r0_loaded;        /**< R0 是否已可用（EEPROM 恢复或
                                        *   预设参数），=1 时预热结束直接
                                        *   进 READY，跳过老化与采样      */
} mq_calib_t;

/* ════════════════════════════════════════════════════
 *  API
 *  ════════════════════════════════════════════════════ */

/**
 * @brief 初始化标定上下文
 * @param calib      上下文指针
 * @param type       传感器型号
 * @param divider    硬件分压比（引脚电压/传感器实际输出电压）
 *                   若未分压直连设为 1.0f，2:1 分压设为 0.5f
 * @param rl_ohm     模块负载电阻 (Ω)，MQ 系列典型值 10000
 */
void APP_MQ_Calib_Init(mq_calib_t *calib, mq_type_t type,
                       float divider, uint16_t rl_ohm);

/**
 * @brief 周期调用（推荐 1Hz）
 *
 * 内部状态机：WARMUP(60s) → AGING(24h) → R0_SAMPLING → READY
 * @param calib   上下文指针
 * @param raw_adc ADC 原始值（0~4095）
 * @param pin_mv  ADC 引脚电压 (mV)，即 BSP_ADC_RawToMv(raw_adc)
 */
void APP_MQ_Calib_Process(mq_calib_t *calib, uint16_t raw_adc, uint16_t pin_mv);

/**
 * @brief 强制开始 R0 采样（跳过剩余老化时间，用于调试/快速验证）
 *
 * 调用后将结束老化阶段，下次 Process 调用时开始采样 R0。
 * 注意：跳过老化会导致 R0 值偏移，浓度精度下降。
 */
void APP_MQ_Calib_ForceCalibrate(mq_calib_t *calib);

/**
 * @brief 手动设置 R0（用于 EEPROM 恢复持久化的标定值）
 * @param calib   上下文指针
 * @param r0_adc  洁净空气中 R0 的 ADC 原始值
 */
void APP_MQ_Calib_SetR0(mq_calib_t *calib, uint16_t r0_adc);

/**
 * @brief 获取状态文本描述
 */
const char *APP_MQ_Calib_StateStr(mq_calib_state_t state);

/* ════════════════════════════════════════════════════
 *  标定时间参数（阶段 1 验收整改：报告 §2.3）
 *
 *  MQ 系列化学电阻式传感器需要长通电老化才能得到稳定的 R0。
 *  但开发联调阶段等 24h 不现实，故用编译期开关区分：
 *      Release（出货/长时间测试）= 生产参数：预热 3 分钟 + 老化 24 小时
 *      Debug  （开发联调）        = 快速参数：预热 60 秒 + 老化 60 秒
 *  由 CMakeLists 用 $<CONFIG:Debug> 生成式自动定义 MQ_CALIB_FAST_AGING，
 *  不需要改源码。启动时会打印当前生效的老化时长，避免"以为在老化其实没在"。
 * ════════════════════════════════════════════════════ */
#ifdef MQ_CALIB_FAST_AGING
  #define MQ_CALIB_WARMUP_MS     (60UL * 1000UL)            /**< 快速：预热 60s   */
  #define MQ_CALIB_AGING_MS      (60UL * 1000UL)            /**< 快速：老化 60s   */
  #define MQ_CALIB_FAST_MODE     1
#else
  #define MQ_CALIB_WARMUP_MS     (3UL * 60UL * 1000UL)      /**< 生产：预热 3 分钟 */
  #define MQ_CALIB_AGING_MS      (24UL * 3600UL * 1000UL)   /**< 生产：老化 24 小时*/
  #define MQ_CALIB_FAST_MODE     0
#endif

#define MQ_CALIB_R0_SAMPLES     20U                   /**< R0 采样次数   */
#define MQ_CALIB_RL_DEFAULT     10000U                /**< 默认负载电阻  */
#define MQ_CALIB_VC_MV          5000U                 /**< MQ 供电电压   */

/* ════════════════════════════════════════════════════
 *  百分比量程统一（用户拍板：「统一量程」）
 *
 *  gas / air 的上报与显示百分比统一以 MQ_PCT_FULL_SCALE_PPM 为满量程：
 *      percent = ppm * 100 / MQ_PCT_FULL_SCALE_PPM   （钳到 100）
 *  这样同一个百分比在两条通道上代表同一个 ppm —— 屏上「优/良/中/差」
 *  阶梯与 60% 告警阈值对两者的含义才一致（此前 5000 vs 1000 各算各的）。
 *
 *  为什么选 1000 而不是 5000：MQ-135 的曲线端点是 100~1000 ppm
 *  （见 app_mq_calib.c 的 g_mq_sens），取 5000 会让空气质量永远到不了
 *  60%（阶梯恒「优」、告警恒不触发）。1000 是两条曲线都能覆盖的最大上限。
 *  代价：可燃气告警点随之降到 600 ppm（≈1.2% LEL），偏保守；若实机误报，
 *  单独上调 app_beep.c 的 BEEP_GAS_ALARM_PERCENT（阈值本就独立于量程）。
 * ════════════════════════════════════════════════════ */
#define MQ_PCT_FULL_SCALE_PPM   1000U

/* ════════════════════════════════════════════════════
 *  硬件分压比（**必须实测后填写** —— 阶段 1 验收整改：报告 §2.3）
 *
 *  MQ 模块 5V 供电，模拟输出在高浓度下接近 5V；而 F103 的 ADC 满量程 3.3V，
 *  多数引脚也不是 5V 容忍 —— 因此**必须**在模块输出与 ADC 引脚之间加分压。
 *  分压比定义为：  divider_ratio = 引脚电压 / 模块输出真实电压
 *
 *       R1=10k(信号侧) + R2=20k(到地) → 引脚拿到 2/3 → divider_ratio ≈ 0.6667
 *       两枚 10k 对分                 → 引脚拿到 1/2 → divider_ratio = 0.5
 *       未分压直连                     → divider_ratio = 1.0（不安全）
 *
 *  ⚠️ 填错会让 Rs / Rs-R0 / ppm 全部偏移，而且**不是等比缩放**：
 *     Rs = RL × (Vc/Vout − 1) 里的 −1 项使换算非线性，所以必须准确。
 *     测法：万用表同时量"模块输出点"与"ADC 引脚点"，两者之比即为该值。
 *
 *  当前 1.0 是**占位值**（表示尚未分压或尚未实测），启动时会打印告警。
 * ════════════════════════════════════════════════════ */
/* ── 实测分压比（2026-10-19 用户万用表实测，替换原占位值 1.0f） ──
 * 实测方法：万用表量模块 AO 对地 = 传感器真实输出；同时读串口 [ADC] 行的 mv= 引脚电压；
 *          divider_ratio = 引脚电压 / 真实输出。
 *   MQ-4  : AO 0.130V，串口 81mV（raw=101）→ 81/130  ≈ 0.623
 *   MQ-135: AO 0.770V，串口 586mV（raw=727）→ 586/770 ≈ 0.761
 * 两通道比例不同属正常（各自分压网络不同）。
 * ⚠️ **这两个值是暂定的**：测量发生在传感器漂移期、且只取了单点采样。后续实测发现
 * DMM 读 AO 100~200mV 而 ADC 只有 1~40mV（差约 3 倍），用 0.623 解释不通 ——
 * 说明该比例不准。**等 MQ-4 硬件修好、两通道输出稳定后必须重测并更新。**
 * ⚠️ 改完必须重新上电跑一次完整标定：R0 也按新 ratio 换算（app_mq_calib.c 的 LoadR0），
 * 旧 R0 不能沿用。 */
#define MQ_DIVIDER_MQ4          0.623f
#define MQ_DIVIDER_MQ135        0.761f

/* ════════════════════════════════════════════════════
 *  MQ 社区口径（替代 24h 老化标定）
 *
 *  来源（社区广泛使用的 MQUnifiedsensor 实现 + 数据手册曲线）：
 *   MQ-4：
 *    - R0 定义为 1000ppm CH4 下的敏感体电阻；
 *    - 洁净空气中 Rs/R0 ≈ 4.4；
 *    - CH4 回归：ppm = 1012.7 × (Rs/R0)^-2.786。
 *   MQ-135：
 *    - R0 定义为 100ppm NH3 下的敏感体电阻（曲线 Rs/R0 = 1 处）；
 *    - 洁净空气中 Rs/R0 ≈ 3.6；
 *    - 等效 NH3 回归：ppm = 116.6 × (Rs/R0)^-2.769。
 *
 *  做法：以 Rs 的历史峰值作为空气基线（R0 = 基线/洁净空气比值），持续缓慢衰减，
 *  不再依赖某一次“洁净空气采样”。气体出现时 Rs 下降 → 比率下降 → ppm 上升；
 *  加热导致的 Rs 整体抬升会被基线自动吸收。上电即可出读数，无需等 24h。
 *
 *  精度限制：器件个体差异与硬件分压比的误差仍在；洁净空气读数不是 0，
 *  而是社区曲线的固有本底（MQ-4 约 15~20ppm，MQ-135 约 3~4ppm），不是故障。
 *
 *  设为 0 可单独让某一路退回原来的 24h 老化 + 两点插值流程。
 * ════════════════════════════════════════════════════ */
#define MQ4_USE_COMMUNITY_CURVE   1
#define MQ4_CURVE_A               1012.7f
#define MQ4_CURVE_B               (-2.786f)
#define MQ4_CLEAN_AIR_RATIO       4.4f

#define MQ135_USE_COMMUNITY_CURVE 1
#define MQ135_CURVE_A             116.602f
#define MQ135_CURVE_B             (-2.769f)
#define MQ135_CLEAN_AIR_RATIO     3.6f

/** 分压比是否仍是未实测的占位值 */
#define MQ_DIVIDER_IS_PLACEHOLDER(d)   ((d) >= 0.999f)

/* ════════════════════════════════════════════════════
 *  R0 持久化（阶段 1 验收整改：报告 §2.3）
 *
 *  24h 老化得到的 R0 是传感器固有属性，不应该每次上电重来。
 *  现在标定完成时写入 EEPROM，上电时读回；但**上电预热仍然执行**
 *  （R0 是器件属性，预热是每次上电的物理需求，两者不是一回事）。
 * ════════════════════════════════════════════════════ */

/**
 * @brief 从 EEPROM 恢复 R0；成功后跳过老化与采样阶段
 * @return 1=已恢复（state 仍从 WARMUP 开始，预热结束直接进 READY）
 *         0=无有效记录（需重新走完整标定流程）
 */
uint8_t APP_MQ_Calib_LoadR0(mq_calib_t *calib);

/**
 * @brief 把当前 R0 保存到 EEPROM（标定完成时自动调用一次）
 * @return 1=成功，0=失败（EEPROM 未接或写失败，仅影响持久化）
 */
uint8_t APP_MQ_Calib_SaveR0(const mq_calib_t *calib);

#ifdef __cplusplus
}
#endif

#endif /* APP_MQ_CALIB_H */