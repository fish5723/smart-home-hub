"""把周期性调试打印包进 debug_config.h 的开关（阶段 1 验收整改 §2.3 T-14）。

用法：在 firmware/f103-node 目录下执行  python ../../tools/gate_periodic_prints.py

做法说明：不匹配含 "\r\n" 的整段字符串（避免转义地狱），
而是**按行定位 printf 语句的起止行**再插入 #if / #endif。
"""
import io
import os
import sys

# (文件, printf 起始行的唯一标识片段)
TARGETS = [
    ('APP/sensor/app_soil.c',  'printf("[ADC] SOIL'),
    ('APP/sensor/app_rain.c',  'printf("[ADC] RAIN'),
    ('APP/sensor/app_mq4.c',   'printf("[ADC] MQ4'),
    ('APP/sensor/app_air.c',   'printf("[ADC] AIR'),
]

# 需要先补 include 的文件
NEED_INC = [
    'APP/sensor/app_soil.c',
    'APP/sensor/app_rain.c',
    'APP/sensor/app_mq4.c',
    'APP/sensor/app_air.c',
]

INC_LINE = '#include "debug_config.h"'


def add_include(lines, path):
    if any(INC_LINE in l for l in lines):
        return
    # 插在最后一个 #include 之后
    last = max(i for i, l in enumerate(lines) if l.lstrip().startswith('#include'))
    lines.insert(last + 1, INC_LINE)


def gate(path, marker):
    lines = io.open(path, encoding='utf-8').read().split('\n')

    add_include(lines, path)

    start = None
    for i, l in enumerate(lines):
        if marker in l:
            start = i
            break
    if start is None:
        raise SystemExit('!! %s 未找到 %s' % (path, marker))

    # 找语句结束行（以 ); 结尾）
    end = None
    for j in range(start, min(start + 20, len(lines))):
        if lines[j].rstrip().endswith(');'):
            end = j
            break
    if end is None:
        raise SystemExit('!! %s 未能定位 printf 语句结尾' % path)

    if start > 0 and '#if DBG_SENSOR_PERIODIC' in lines[start - 1]:
        print('  skip(已包裹)', path)
        return

    lines.insert(start, '#if DBG_SENSOR_PERIODIC')
    lines.insert(end + 2, '#endif')   # +2 因为上面插入了一行

    io.open(path, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
    print('  已包裹', path, '（行 %d-%d）' % (start + 1, end + 2))


for path, marker in TARGETS:
    if not os.path.exists(path):
        raise SystemExit('!! 文件不存在: ' + path)
    gate(path, marker)

print('完成')
