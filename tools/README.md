# tools 目录说明

PC 端工具与环境/构建脚本。这些不是"顺手做的"——没有它们，协议调试只能靠盲猜字节，构建环境也无法复现。

## 构建 / 环境

| 文件 | 作用 |
|---|---|
| `p4build.bat` | P4 固件一键构建（固化 ESP-IDF v6.1 环境；唯一构建目录 `build/`），从 Git Bash 经 cmd 调用 |
| `p4build_log.bat` | 调用 `p4build.bat` 并把输出重定向到根目录 `build_p4.log` |
| `p4debug.bat` | P4 工具链自检（打印当前 IDF 路径与版本） |
| `activate_idf.ps1` | PowerShell 里激活 ESP-IDF v6.1 环境（`. .\tools\activate_idf.ps1`），之后可直接 `idf.py build` |

## 协议调试（阶段 2）

| 工具 | 作用 |
|---|---|
| `protocol_sniffer.py` | 接串口录包 → 解析成统计/表格：帧数、CRC 错误数、重传数、去重命中数；支持 `--hex-only`、`--save capture.bin`。依赖 `pyserial` |
| `fake_peer.py` | 模拟对端（P4 主站）：发 PING 心跳 / 单条执行器指令 / `--interactive` 交互 / `--test` 自动化序列。依赖 `pyserial` |

```bash
python tools/protocol_sniffer.py COM3 115200
python tools/protocol_sniffer.py COM3 115200 --hex-only
python tools/protocol_sniffer.py COM3 115200 --save capture.bin

python tools/fake_peer.py COM3 115200              # 只发送 PING 心跳
python tools/fake_peer.py COM3 115200 --cmd relay1 on
python tools/fake_peer.py COM3 115200 --test       # 自动化测试序列
```

> 💡 **为什么一定要有这些工具**：协议一旦出问题，如果只能靠 MCU 打印调试，你会陷入
> "改一行、烧一次、看一次"的低效循环。有了 PC 端解析器，你能看到原始字节流，定位速度快一个数量级。

## 资源生成 / 静态检查

| 工具 | 作用 |
|---|---|
| `generate_runtime_fonts.mjs` | 生成运行时 CJK 字体 `ui/assets/generated_cn_font/fontcn{14,16,18,24,32,48}.bin`（同时刷新 `simulator/fonts/cn_font/`），需 Node + `lv_font_conv` |
| `add_source_headers.py` | 为 F103 源文件补「来源 + 本工程修改」文件头（阶段 1 验收 §0.2）；支持 `--dry-run` |
| `gate_periodic_prints.py` | 把 F103 传感器周期打印包进 `debug_config.h` 开关（阶段 1 整改 T-14） |
| `check_roadmap.py` | 校验 `roadmap.html` 的 JS/HTML 结构（防止中文引号截断 JS 字符串）；**该 HTML 当前不在仓库内** |

## 单测 / 本地依赖

| 文件 | 作用 |
|---|---|
| `test_mq_math.c` | 宿主机单测：用 libm 对照验证 `firmware/f103-node/common/mq_math.h` 换算（阶段 1 验收 §2.2）。`gcc -O2 -Wall -o tools/test_mq_math tools/test_mq_math.c -lm && ./tools/test_mq_math` |
| `SDL2-2.30.10/` · `SDL2-devel-2.30.10-mingw.tar.gz` | PC 模拟器（`simulator/`）的 SDL2 依赖，本地解压使用 |
