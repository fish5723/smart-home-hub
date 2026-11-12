/**
 * @file rule_engine.h
 * @brief 本地规则引擎 — 纯逻辑核心（ADR-022）
 *
 * 设计约束：
 *   - 不包含任何 ESP-IDF 头：时间(now_us)、传感器取值、命令下发全部经参数/回调注入，
 *     tests/ 下 PC 可直接编译单测（tests/rule-engine/，ctest 模式）
 *   - 数据结构固定布局、内联数组、无指针，可整体 memcpy 做 NVS blob 序列化
 *   - 复用协议组件的 dev_id / action 常量（protocol.h），不另造状态模型（ADR-005）
 *
 * 评估模型（每 tick，建议 200ms 周期）：
 *   对每条 enabled 规则：
 *     1. 求值全部条件（AND 语义）；quality_req=1 的传感器条件在 quality != VALID
 *        时不成立（继承 F103 灌溉的质量门控教训：断线探头不得触发动作）
 *     2. 条件持续成立 >= debounce_ms 才算"满足"（去抖）
 *     3. 满足且处于未触发态、且距上次触发 >= cooldown_ms → 触发（边沿语义）
 *     4. 同一 tick 多条规则触发时，priority 大者先执行
 *     5. 条件不再满足 → 自动回到未触发态（允许下次再触发）
 *
 * 与 F103 灌溉兜底的关系（ADR-021）：
 *   链路 LINK_UP 时由本引擎驱动执行器；LINK_DOWN 时 F103 灌溉逻辑本地接管。
 *   因此引擎在 link_up=false 时跳过全部评估（避免与兜底逻辑竞争）。
 *
 * 阶段: 阶段 4 — 本地场景引擎
 */
#ifndef RULE_ENGINE_H
#define RULE_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════
 *  容量上限
 *  ⚠️ 属于 NVS blob 格式的一部分：修改任何容量都需要升级 RULE_BLOB_VERSION
 *  ════════════════════════════════════════════════════ */

#define RULE_SET_MAX_RULES   8
#define RULE_MAX_CONDS       4
#define RULE_MAX_ACTS        2
#define RULE_NAME_LEN        16

/* ── 比较器 ── */
typedef enum {
    RULE_CMP_LT = 0,    /* value <  threshold */
    RULE_CMP_LE = 1,    /* value <= threshold */
    RULE_CMP_GT = 2,    /* value >  threshold */
    RULE_CMP_GE = 3,    /* value >= threshold */
} rule_cmp_t;

/* ── 条件源类型 ── */
typedef enum {
    RULE_SRC_SENSOR = 0,   /* 传感器值 vs threshold（dev_id = PROTO_DEV_xxx） */
    RULE_SRC_LINK   = 1,   /* 链路状态：link_up(0/1) vs threshold(0=要求断,1=要求通) */
    RULE_SRC_ALWAYS = 2,   /* 恒真（用于纯时间/手动场景占位，v2 扩展时间窗） */
} rule_src_t;

/* ── 条件 ── */
typedef struct {
    uint8_t src;           /* rule_src_t */
    uint8_t dev_id;        /* RULE_SRC_SENSOR 时有效 */
    uint8_t cmp;           /* rule_cmp_t */
    uint8_t quality_req;   /* 1 = 要求 PROTO_QUAL_VALID 才成立 */
    float   threshold;
} rule_cond_t;

/* ── 动作 ── */
typedef struct {
    uint8_t  dev_id;       /* PROTO_DEV_xxx */
    uint8_t  action;       /* PROTO_ACTION_xxx */
    uint8_t  value;        /* 保留（SET 类动作用） */
    uint8_t  reserved;
    uint16_t ttl_ms;       /* 0 = 持久命令（ADR-021 语义） */
} rule_act_t;

/* ── 单条规则 ── */
typedef struct {
    /* ── 持久化字段 ── */
    uint8_t  id;               /* 1..255，0 = 空槽 */
    uint8_t  enabled;
    uint8_t  priority;         /* 0 = 最低，大者先执行 */
    uint8_t  cond_count;
    uint8_t  act_count;
    uint8_t  reserved0;
    uint16_t debounce_ms;      /* 条件需持续成立时长 */
    uint16_t cooldown_ms;      /* 触发后冷却 */
    char     name[RULE_NAME_LEN];
    rule_cond_t conds[RULE_MAX_CONDS];
    rule_act_t  acts[RULE_MAX_ACTS];

    /* ── 运行时状态（不持久化，加载/重启后由 rule_engine_reset 清零）── */
    uint8_t  cond_met;         /* 0=未满足, 1=去抖中, 2=本次边沿已触发 */
    int64_t  cond_since_us;    /* 条件开始持续成立的时刻 */
    int64_t  last_fire_us;     /* 上次触发时刻（冷却计时） */
} rule_t;

/* ── 规则集 ── */
typedef struct {
    uint8_t count;             /* 有效规则数（<= RULE_SET_MAX_RULES） */
    rule_t  rules[RULE_SET_MAX_RULES];
} rule_set_t;

/* ════════════════════════════════════════════════════
 *  注入回调（硬件/系统边界，PC 单测时替换为桩）
 *  ════════════════════════════════════════════════════ */

/** 读传感器影子值：设备不存在返回 NaN；*quality 输出 PROTO_QUAL_xxx */
typedef float (*rule_sensor_fn)(uint8_t dev_id, uint8_t *quality);

/** 下发执行器命令（dev_id/action 用 protocol.h 常量；ttl_ms=0 表示持久命令） */
typedef void (*rule_cmd_fn)(uint8_t dev_id, uint8_t action, uint16_t ttl_ms);

/* ════════════════════════════════════════════════════
 *  核心 API（纯逻辑）
 *  ════════════════════════════════════════════════════ */

/** 清空全部规则与运行时状态 */
void rule_engine_clear(rule_set_t *set);

/** 清运行时状态（NVS 加载 / 重启后调用；不动规则配置本身） */
void rule_engine_reset_runtime(rule_set_t *set);

/**
 * 评估一帧。
 * @param set      规则集（读写运行时状态）
 * @param link_up  P4↔F103 链路是否 UP；false 时跳过评估（F103 兜底接管）
 * @param now_us   当前时间戳（微秒，单调递增）
 * @param get      传感器取值回调（可为 NULL：所有 SENSOR 条件不成立）
 * @param cmd      命令下发回调（可为 NULL：触发时只记日志不执行）
 * @return 本次 tick 实际下发的命令条数（观测/延迟统计用）
 */
int rule_engine_tick(rule_set_t *set, bool link_up, int64_t now_us,
                     rule_sensor_fn get, rule_cmd_fn cmd);

/* ════════════════════════════════════════════════════
 *  序列化（NVS blob，ADR-022）
 *  布局: magic(4B) + version(2B) + reserved(2B) + crc32(4B) + rule_set_t
 *  crc32 覆盖 rule_set_t 全部字节；多字节字段小端（xtensa/arm 原生）
 *  ════════════════════════════════════════════════════ */

#define RULE_BLOB_MAGIC    0x31545352UL   /* "RST1" 小端读出 */
#define RULE_BLOB_VERSION  1UL

/** 序列化；返回字节数，buf 太小返回 0 */
size_t rule_set_serialize(const rule_set_t *set, uint8_t *buf, size_t cap);

/**
 * 反序列化：校验 magic / version / crc32 / count 边界。
 * 成功返回 true（out 已清运行时态）；任何失败返回 false 且不动 out。
 */
bool rule_set_deserialize(const uint8_t *buf, size_t len, rule_set_t *out);

/** 序列化后 blob 的固定大小（NVS 预留空间用） */
size_t rule_set_blob_size(void);

#ifdef __cplusplus
}
#endif

#endif /* RULE_ENGINE_H */
