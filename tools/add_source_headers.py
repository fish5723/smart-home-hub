"""为缺少「来源 + 本工程修改」文件头的源文件补注释（阶段 1 验收报告 §0.2）。

用法：python tools/add_source_headers.py [--dry-run]
在 firmware/f103-node 目录下执行。

规则：
- 若前 25 行已含「来源」→ 跳过
- 若文件以 /** 开头 → 在该注释块结束前插入「来源 / 本工程修改」段落
- 否则 → 在文件最前插入完整 doxygen 头
"""
import io
import os
import sys

DRY = '--dry-run' in sys.argv

# file -> (brief, 来源, 本工程修改)
M = {
    # ── BSP ──
    'BSP/bsp_adc.h': ('ADC 采集统一层 — 通道表 + 多次采样 + 中值滤波（接口）',
        '本工程新建（阶段 1 合并前序工程 4 份重复 ADC 驱动）',
        '—'),
    'BSP/bsp_beep.h': ('蜂鸣器接口 — 基于 TIM4 的多音调驱动',
        '前序工程 BSP/BEEP（原为纯 GPIO 翻转，无音调能力）',
        '阶段 1（T-08）：改为 TIM 驱动，支持频率设定以区分告警级别'),
    'BSP/bsp_beep.c': ('蜂鸣器实现 — TIM 驱动，多音调',
        '前序工程 BSP/BEEP/beep.c（原为 GPIO 直接翻转）',
        '阶段 1（T-08）：改为 TIM 更新中断产生方波；保留 BEEP_On/Off/Toggle 兼容旧调用'),
    'BSP/bsp_bmp280.h': ('BMP280 气压/温度传感器接口（I2C）',
        '前序工程 BSP/BMP-280（质量最好的一支驱动）',
        '阶段 1：引脚与总线句柄改从 bsp_pins.h / hi2c1 取；错误码沿用 0=成功'),
    'BSP/bsp_bmp280.c': ('BMP280 气压/温度传感器实现（I2C，含校准补偿）',
        '前序工程 BSP/BMP-280/bsp_bmp280.c',
        '阶段 1：仅重命名前缀与整理头文件，补偿算法保持原样（含 int64 防溢出）'),
    'BSP/bsp_dht11.h': ('DHT11 温湿度传感器接口（单总线）',
        '前序工程 BSP/DHT11/driver_dht11.h（原文件名 driver_dht11，本工程改前缀）',
        '阶段 1：数据只取整数部分（小数位丢弃），故对外用整数语义；'
        '已知问题见 bsp_dht11.c 头部（关中断窗口过大 + 无重试）'),
    'BSP/bsp_dht11.c': ('DHT11 温湿度传感器实现（单总线，DWT 微秒延时）',
        '前序工程 BSP/DHT11/driver_dht11.c',
        '阶段 1：引脚改从 bsp_pins.h 取；读取加 __disable_irq 临界区（但窗口过大，'
        '含 20ms 起始脉冲 —— 阶段 1 验收报告 §3 已列为必修项）；'
        '起始信号从 HAL_Delay(20) 改为 delay_us(20000)'),
    'BSP/bsp_lock.h': ('电子锁接口（PB8）',
        '前序工程 BSP/LOCK/bsp_lock.h',
        '阶段 1：引脚改从 bsp_pins.h 取。⚠️ 未实现脉冲限时保护 —— 本工程已决定'
        '暂不使用该锁，任务 T-07 未做，详见 docs/11-阶段1-验收报告.md'),
    'BSP/bsp_lock.c': ('电子锁实现（PB8，直接写电平）',
        '前序工程 BSP/LOCK/bsp_lock.c',
        '阶段 1：仅把引脚改从 bsp_pins.h 取，**未实现脉冲限时**。'
        '⚠️ 若将来启用：SMA 钛丝锁持续通电会永久损坏，必须补 MOSFET 驱动 + '
        '硬件定时器兜底断电 + 状态回读（原计划 T-07，当前未做）'),
    'BSP/bsp_ssd1306.h': ('SSD1306 OLED 驱动接口（I2C，128x64）',
        '第三方库 afiskon/stm32-ssd1306（前序工程 BSP/OLED），本工程重命名为 bsp_ 前缀',
        '阶段 1（T-11）：I2C 超时从 HAL_MAX_DELAY 改为 50ms；'
        '新增脏页增量刷新（SSD1306_LastSent + UpdateScreen）'),
    'BSP/bsp_ssd1306.c': ('SSD1306 OLED 驱动实现（I2C）',
        '第三方库 afiskon/stm32-ssd1306（前序工程 BSP/OLED/ssd1036.c，原文件名拼写有误）',
        '阶段 1（T-11）：① 超时改有限值（SSD1306_I2C_TIMEOUT_MS=50）；'
        '② 新增 ssd1306_InvalidateCache() 与脏页比对式 UpdateScreen；'
        '③ ssd1306_Fill() 会置 ForceFull（会绕过脏页机制，见验收报告 §2.3）'),
    'BSP/bsp_ssd1306_conf.h': ('SSD1306 库配置（本工程选 I2C，仅启用 7x10 字体）',
        '第三方库 afiskon/stm32-ssd1306 的 ssd1306_conf.h（前序工程 BSP/OLED）',
        '阶段 1：本工程只用 I2C 分支；字体仅启用 Font_7x10，其余 5 套保持注释状态'
        '（未启用的字体被 #ifdef 排除，最终固件里 Font7x10 占 1900 B）'),
    'BSP/bsp_ssd1306_fonts.c': ('SSD1306 字库（6 套字体，本工程仅启用 7x10）',
        '第三方库 afiskon/stm32-ssd1306',
        '阶段 1：未修改。注意本文件源码约 19KB，但未被引用的字体由 #ifdef 与 '
        '--gc-sections 剔除，最终固件仅含 Font7x10（1900 B）'),
    'BSP/bsp_ssd1306_fonts.h': ('SSD1306 字库声明',
        '第三方库 afiskon/stm32-ssd1306',
        '阶段 1：未修改'),
    'BSP/bsp_ssd1306_tests.c': ('SSD1306 库自带自测代码（本工程【不参与编译】）',
        '第三方库 afiskon/stm32-ssd1306',
        '阶段 1：CMakeLists 已用 FILTER 将其排除出构建（内含 HAL_Delay(5000) 等'
        '长阻塞测试，且无调用者）。保留文件仅作上游参考'),
    'BSP/bsp_ssd1306_tests.h': ('SSD1306 自测代码声明（本工程不参与编译）',
        '第三方库 afiskon/stm32-ssd1306',
        '阶段 1：随 bsp_ssd1306_tests.c 一起被排除出构建'),

    # ── APP ──
    'APP/app_data.c': ('系统数据中心 — 全局实例定义',
        '前序工程 APP/app_data.c（原为裸全局 g_sensor_data）',
        '阶段 1：结构体大幅扩展（raw/mv、Rs/R0、ppm、quality），本文件仅提供定义；'
        '⚠️ 仍无时间戳、无脏标记、无并发保护（验收报告 §3.4）'),
    'APP/comm/app_uart.h': ('串口汇总输出接口（USART3，调试用）',
        '前序工程 APP/uart/app_uart.h',
        '阶段 1：输出口定为 USART3；USART1 专供协议，不允许出现文本输出'),
    'APP/control/app_beep.h': ('蜂鸣器应用层接口 — 分级告警',
        '前序工程 APP/beep/app_beep.h',
        '阶段 1（T-08）：告警音改用 TIM 多音调（800/2400Hz 交替），不再阻塞主循环'),
    'APP/control/app_beep.c': ('蜂鸣器应用层实现 — 分级告警 + 多音调',
        '前序工程 APP/beep/app_beep.c（原用 HAL_Delay(200) 翻转 GPIO）',
        '阶段 1（T-08）：改由 BSP_TIM 驱动，主循环阻塞时音调不再失真；'
        '阈值仍为 gas/air 百分比，⚠️ 但 MQ 的 ppm 换算有缺陷（验收报告 §2.2）'),
    'APP/display/app_monitor.h': ('OLED 本地显示接口（F103 降级显示通道）',
        '前序工程 APP/app_monitor.h',
        '阶段 1：新增快照式脏标记；页切换 5s'),
    'APP/display/app_monitor.c': ('OLED 本地显示实现（200ms 刷新，5s 翻页）',
        '前序工程 APP/app_monitor.c（原为 200ms 整屏刷新 + 任务与页面函数各刷一次）',
        '阶段 1（T-11）：去掉重复刷屏（UpdateScreen 全工程只调一次）+ 加快照脏标记。'
        '⚠️ 但每次刷新前的 ssd1306_Fill(Black) 会置 ForceFull，导致 BSP 层脏页'
        '增量机制被绕过，单次仍是整屏 ~25ms（验收报告 §2.2）'),
    'APP/sensor/app_air.h': ('MQ-135 空气质量应用层接口（1Hz）',
        '前序工程 APP/mq-135/app_air.h',
        '阶段 1（T-02/T-03）：接入 MQ 标定模块输出 Rs/R0 与 ppm，新增 quality；'
        '⚠️ 旧 percent（adc*100/4095）在标定完成前仍在用（验收报告 §2.3）'),
    'APP/sensor/app_mq4.h': ('MQ-4 可燃气体应用层接口（1Hz）',
        '前序工程 APP/mq-4/app_mq4.h',
        '阶段 1（T-02/T-03）：接入 MQ 标定模块输出 Rs/R0 与 ppm，新增 quality；'
        '⚠️ ppm 换算存在取值域缺陷，当前恒为 100.0（验收报告 §2.2）'),
    'APP/sensor/app_mq_calib.h': ('MQ 传感器标定模块接口（R0 标定 + Rs/R0 + log-log 浓度）',
        '本工程新建（阶段 1 T-02）',
        '—　⚠️ 已知缺陷：ppm 换算恒为 100.0；老化时间默认 60s 而非 24h；'
        'R0 不持久化；分压比为 1.0 占位（详见验收报告 §2.2/§2.3）'),
    'APP/sensor/app_rain.h': ('雨滴传感器应用层接口（1Hz）',
        '前序工程 APP/rain/app_rain.h',
        '阶段 1：新增 raw/mv 与 quality；raining 阈值判定保留'),
    'APP/sensor/app_soil.h': ('土壤湿度应用层接口（1Hz）',
        '前序工程 APP/soil/app_soil.h',
        '阶段 1：新增 raw/mv 与 quality；'
        '⚠️ 探头断开时仍置 valid=1 且 percent=0（误判"极干"），验收报告 §2.2'),
    'APP/sensor/app_human.h': ('PIR 人体检测应用层接口（100ms）',
        '前序工程 APP/hc-sr501/app_human.h',
        '阶段 1（T-13）：优先消费 EXTI 中断事件，无事件时回落轮询；'
        '⚠️ 尚无计数与去抖（验收报告 §2.3）'),
    'APP/sensor/app_bmp280.h': ('BMP280 应用层接口（1Hz）',
        '前序工程 APP/bmp-280/app_bmp280.h',
        '阶段 1：新增 quality；⚠️ fault_cnt 成功后未清零（验收报告 §3.2）'),
    'APP/sensor/app_bmp280.c': ('BMP280 应用层实现（1Hz）',
        '前序工程 APP/bmp-280/app_bmp280.c',
        '阶段 1：接入 quality 与 fault_cnt；⚠️ fault_cnt 成功后未清零'),
    'APP/sensor/app_dht11.h': ('DHT11 应用层接口（2s）',
        '前序工程 APP/dht11/app_dht11.h',
        '阶段 1：新增 quality 与 g_sched_trace 标记；'
        '⚠️ 无失败重试（验收报告 §2.3）'),
}


def build_block(brief, origin, changes):
    lines = [' * 来源：%s' % origin]
    if changes and changes != '—':
        lines.append(' * 本工程修改：')
        for ln in changes.split('\n'):
            lines.append(' *   - %s' % ln)
    return lines


n_done = 0
n_skip = 0
for path, (brief, origin, changes) in sorted(M.items()):
    if not os.path.exists(path):
        print('  [缺失] %s' % path)
        continue
    txt = io.open(path, encoding='utf-8').read()
    if '来源' in '\n'.join(txt.split('\n')[:25]):
        n_skip += 1
        continue

    blk = build_block(brief, origin, changes)

    if txt.lstrip().startswith('/**'):
        # 在首个注释块结束前插入
        idx = txt.index('*/')
        head = txt[:idx].rstrip('\n')
        tail = txt[idx:]
        new = head + '\n *\n' + '\n'.join(blk) + '\n ' + tail
    else:
        new = ('/**\n * @file %s\n * @brief %s\n *\n' % (os.path.basename(path), brief)
               + '\n'.join(blk) + '\n */\n' + txt)

    if not DRY:
        io.open(path, 'w', encoding='utf-8', newline='').write(new)
    n_done += 1

print('已补写 %d 个文件，跳过 %d 个（已有来源注释）' % (n_done, n_skip))
