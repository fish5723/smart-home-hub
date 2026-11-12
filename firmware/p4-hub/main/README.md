# firmware/p4-hub/main — 模块说明与操作手册

> 主控：ESP32-P4（JC-ESP32P4-M3-DEV）+ 板载 ESP32-C6（esp_hosted / esp_wifi_remote）
> 显示：JD9165BA MIPI-DSI 1024×600 ｜ 触摸：GT911 ｜ GUI：ESP-IDF v6.1 + LVGL 9.4

## 1. 目录与模块地图

| 路径 | 职责 | 备注 |
|---|---|---|
| `app_main.c` | 启动顺序编排 + 主循环（协议轮询/喂狗） | 见 §3 |
| `ui/ui_pages.[ch]` | **既有六页**智能家居 UI（概览/环境/设备/自动化/告警/设置） | 不重写，仅新增 `ui_pages_font()` 供复用 |
| `ui/ui_tokens.h` | 设计 token（配色/圆角/画布尺寸） | 三端同步（Penpot / HTML 原型 / 本文件） |
| `ui/fonts/` | 内置中文 fallback 字体 `ui_font_cjk_18/24/48.c` | SD 挂载时改加载 SD `fontcn{14,16,18,24,32,48}.bin`（仓库源 `ui/assets/generated_cn_font/`），缺失则回退内置 |
| `bsp/lcd` `bsp/touch` `bsp/lvgl` | 点屏、GT911、`esp_lvgl_adapter` 封装 | 本次未改动 |
| `bsp/storage` | NVS 键值封装（统一入口，禁止各模块自开 NVS） | 统一 namespace `"storage"`（rule_runtime 的 r0/r1/active 双槽自 2026-10-18 起也在此）；天气 Key 存这里 |
| `bsp/wifi` | esp_hosted SDIO + esp_wifi_remote | 本次未改动 |
| **`standby/`** | **待机时钟层 + 动态天气壁纸（本次新增）** | 详见 §2 |
| `manager` `vision` `common` `camera` | 摄像头/视觉平台/JSON 工具 | 与本次无关 |

## 2. `standby/` 四个模块

| 文件 | 职责 | 关键约定 |
|---|---|---|
| `jwp_reader.[ch]` | JWP 容器解析（32B 头校验、顺序读帧、`transition_offset` 循环回跳、`jwp_crc32`） | **不依赖 LVGL/ESP 专有 API**，可 PC 单测（`tests/jwp-reader`） |
| `wallpaper_player.[ch]` | SD 流式读帧 → JPEG 解码 → RGB565 → 帧回调；3 缓冲轮转、暂停/恢复/循环、fps 统计与错误恢复 | **不 include LVGL**；帧回调内由上层 `lv_async_call` 切到 LVGL 上下文 |
| `standby_ui.[ch]` | 全屏待机层（时钟/日期/Wi-Fi/天气/壁纸）、双击唤醒、空闲超时 | LVGL 对象只在 LVGL 任务（1s 定时器 / async 回调）里操作 |
| `weather_service.[ch]` | SNTP 授时 + uapis.cn 天气 HTTPS + NVS 缓存 + 场景映射 + 指数退避 | 独立任务；**不触碰 LVGL**；不占用 `wifi.c` 的単槽回调 |

场景枚举 `weather_scene_t` ↔ 目录名由 `weather_scene_to_string()` 唯一映射：
`sunny / cloudy / overcast / rain / storm(雷暴) / snow / fog / haze / night / unknown`。

## 3. 启动顺序（`app_main.c`）

```
NVS → storage → crash_log → SD 卡 → Wi-Fi(+自动连接) → [vision] → 协议栈
    → LCD（含 3 次重试；失败转 headless）→ 触摸 → LVGL(adapter)
    → 壁纸播放器（纯内存，必须先于 ui_create）
    → lv_async_call(ui_create)  ← 主六页 UI + 待机层 + 壁纸 attach
    → 天气服务（后台任务）→ 主循环(AppProto_Poll + 喂狗)
```

硬约束：
1. **SD 必须在 Wi-Fi 之前**（SDMMC Slot0/Slot1 共享时钟分频器）。
2. LCD 未接时不要初始化（`esp_lcd` DCS 忙等会挂死）→ 用 `SMART_HOME_LCD_ENABLED=n` headless。
3. 所有 LVGL 对象创建走 `lv_async_call`，**网络回调/后台任务里绝不调 LVGL**。
4. 壁纸播放器必须先于 `ui_create` 初始化，否则 `standby_ui_attach_wallpaper()` 拿不到缓冲。
5. **console 为忙等写**（`nb_console_vprintf` 非阻塞方案已回滚，完整实现在 git `cdbe42b`）：
   联调期 **P4 串口工具必须保持打开**，关闭/切换串口会导致 `app_main` 卡死在日志写入上
   （任务看门狗触发）；P4 + F103 两个串口窗口都常开即可。协议组件里"console 已非阻塞"的
   旧注释已于 2026-10-18 勘误（以 `app_main.c` 为准）。

## 4. Kconfig 开关（隔离测试用）

menuconfig → `Smart Home Hub — 硬件配置` → **待机层 / 动态天气壁纸**、**天气服务**

| 配置项 | 默认 | 作用 |
|---|---|---|
| `SMART_HOME_STANDBY_ENABLED` | y | 待机层总开关（n → 模块编译为空实现，只用既有六页 UI） |
| `SMART_HOME_STANDBY_IDLE_SEC` | 60 | 主 UI 空闲多久回待机 |
| `SMART_HOME_STANDBY_DOUBLE_CLICK_MS` | 400 | 双击时间窗口 |
| `SMART_HOME_WALLPAPER_ENABLED` | y | 壁纸播放器总开关（n → 待机层只用渐变背景） |
| `SMART_HOME_WALLPAPER_DIR` | `/sdcard/wallpaper` | 壁纸根目录 |
| `SMART_HOME_WALLPAPER_FPS` | 30 | 目标帧率；显示链路无法跟上时由三缓冲背压丢旧帧，实际以 `SELFTEST` 为准 |
| `SMART_HOME_WALLPAPER_MAX_FRAME_KB` | 512 | 单帧上限，须与 `--max-frame` 一致 |
| `SMART_HOME_WALLPAPER_DECODER_HW / _SW` | HW | 硬件 `esp_driver_jpeg`（DMA2D）/ 软件 `esp_new_jpeg` |
| `SMART_HOME_WEATHER_ENABLED` | y | 天气服务（n → 只本地时间，不联网） |
| `SMART_HOME_WEATHER_KEY` | 空 | uapis.cn Bearer Key（**空 = 只授时不请求**）；只在 NVS/本地 sdkconfig 填（ADR-016） |
| `SMART_HOME_WEATHER_LOCATION` | 空 | 可选：空 = 按 IP 自动定位；或 6 位 adcode / 城市名（NVS `wx_loc`） |
| `SMART_HOME_WEATHER_REFRESH_SEC` | 900 | 成功时刷新周期 |
| `SMART_HOME_MQTT_ENABLED` | **n** | MQTT 客户端总开关（opt-in）；n → 只编译不启动 |
| `SMART_HOME_MQTT_BROKER_URI` | 空 | broker 地址（如 `mqtt://192.168.1.10:1883`）；**空 = 不启动**；NVS `mqtt_uri` 优先 |
| `SMART_HOME_MQTT_USERNAME` / `_PASSWORD` | 空 | broker 账号（**勿填真实值**，ADR-016；NVS `mqtt_user`/`mqtt_pass`） |
| `SMART_HOME_MQTT_NODE_ID` | p4hub | 主题前缀 `smart_home/<node_id>`；NVS `mqtt_node` 优先 |
| `SMART_HOME_MQTT_KEEPALIVE_S` | 30 | MQTT keepalive 秒 |
| `SMART_HOME_MQTT_ALLOW_CLOUD_CMD` | y | 是否允许云端下发执行器指令（仍受本地优先 1s 仲裁） |
| `SMART_HOME_STANDBY_SELFTEST` | n | 每 5s 打一条 `[SELFTEST]`（授时/天气/场景/Wi-Fi/可见性/解码帧率 + 显示侧 `sink`/`hold`/`dispfps`/`prep`/`draw`/`flush` 性能打点） |
| `SMART_HOME_WALLPAPER_TEST_FORCE` | n | **隔离测试开关**：打开后忽略天气，强制用下面指定的场景 |
| `SMART_HOME_WALLPAPER_TEST_SCENE` | sunny | 强制场景名（仅在上面的开关打开时生效） |
| `SMART_HOME_WALLPAPER_TEST_NIGHT` | n | 强制用 night 变体 |

**隔离测试组合建议**
- 只验证待机层 UI + 双击/空闲：关 `WEATHER_ENABLED` + 开 `TEST_FORCE`。
- 只验证壁纸解码：开 `SELFTEST` + `TEST_FORCE` + `TEST_SCENE="rain"`，看
  `[SELFTEST] ... frames=… avg=…us target=…fps sink=…/…us hold=…/…us dispfps=… prep=…us draw=…us flush=…us`。
  `sink` = 播放器在帧回调里的停留时间（≈ 等 LVGL 适配器锁）；`hold` = 缓冲从发布到释放的存活时间；
  `dispfps` = 显示侧真实刷新帧率（由 `CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS` 提供）。
  右侧三值把「一个显示帧周期」按 LVGL display 事件切成三段（三者之和 ≈ 1/dispfps）：
  `prep` = `REFR_START→RENDER_START`（layout / 失效区合并）；`draw` = `RENDER_START→RENDER_READY`（真正画像素）；
  `flush` = `RENDER_READY→REFR_READY`（提交 + 等面板帧缓冲切换完成）。用于判断瓶颈落在渲染还是刷屏。
- 只验证天气：`WALLPAPER_ENABLED=n`（渐变背景），日志看 `weather updated: …`。
- 与 Wi-Fi 共存对照：`DECODER_SW` vs `DECODER_HW`（HW 走 2D-DMA，见 §7 风险）。

## 5. 天气配置（uapis.cn — ADR-020）

天气源：uapis.cn 单端点 `GET /api/v1/misc/weather`，认证 `Authorization: Bearer <wx_key>`。
**未配置 Key 时只授时、不发起天气请求**（日志会明确提示）；定位默认按设备出口 IP 自动定位。

```bash
# 方式 A（推荐）：运行时写入 NVS（namespace "storage"）
#   在设备侧代码/串口命令里调用：
#     storage_set_str("wx_key", "<uapis.cn Bearer Token>");
#     storage_set_str("wx_loc", "440300");   # 可选：6 位 adcode（如深圳）
#     storage_set_str("wx_loc", "深圳");      # 可选：城市名（自动 URL 编码）
# 方式 B：idf.py menuconfig → 天气服务（写入本地 sdkconfig，该文件已被 .gitignore 忽略）
```
解析顺序：NVS → Kconfig → 无 Key 则只授时、不发请求。
若配置了无效 Key 会收到 401（日志有提示），修正或清空 `wx_key` 即可。
缓存：`wx_cache`（cJSON 串：温度/icon 码/昼夜/时间戳/现象/城市/日出日落分钟数，日出日落取自 `forecast[0]`）。

## 5.1 MQTT / Home Assistant（阶段 5）

客户端为 registry 组件 `espressif/mqtt`（**IDF 6.1 核心已移除 esp-mqtt**）。完整规范见 `docs/17-MQTT与HA集成规范.md`。

- 主题：`homeassistant/<component>/<node_id>/<obj_id>/config`（Discovery，retain）、
  `smart_home/<node_id>/availability`（LWT 遗嘱）、`.../state/<obj_id>`、`.../cmd/<obj_id>`。
- 连接建立 → 发 `availability=online` → 重发全部 discovery → 全量状态；另订阅 `homeassistant/status` 应对 HA 重启。
- `expire_after=60` + 每 20s 强制重发：链路断/数据不可信时让 HA 如实显示 `unavailable`。
- 本地优先（ADR-004）：云指令经队列由主循环下发；本地操作后 1s 内云指令被丢弃（`[ARB]` 日志）。

**配置**（NVS 优先，Kconfig 兜底；真实值不入库）：

```text
storage_save_mqtt_cred("mqtt://192.168.1.10:1883", "", "");
# 或 menuconfig → Smart Home Hub — 硬件配置 → MQTT / Home Assistant 集成
```

未配置 uri 时启动只打印 `no broker configured … MQTT disabled`，不影响其它功能。

## 6. 壁纸制作与部署

```bash
# 视频 → JWP（需要 ffmpeg；环境里若无则用 --ffmpeg 指定）
python ui/assets/scripts/make_wallpaper_jwp.py \
    --input rain.mp4 --output ui/wallpaper/rain/day.jwp \
    --width 1024 --height 600 --fps 15 --quality 4 --duration 10 --max-frame 512
# 图片序列 → JWP（不需要 ffmpeg）
python ui/assets/scripts/make_wallpaper_jwp.py --frames-dir /tmp/frames --output out.jwp
# 校验已有 JWP（逐帧尺寸 / baseline 检查 / CRC32 / 上限）
python ui/assets/scripts/make_wallpaper_jwp.py --inspect out.jwp
```
运行时从 SD 卡加载（Kconfig `SMART_HOME_WALLPAPER_DIR` 默认 `/sdcard/wallpaper`；FAT32，建议 ≥1GB Class10）：
```
/sdcard/wallpaper/<scene>/{day,night}.jwp     # 场景名见 §2
/sdcard/wallpaper/test/sample.jwp             # 仓库自带 18KB 样例，用于无素材时点屏验证
```

**同一场景的多段变体**：`<scene>/day_02.jwp … day_09.jwp`（及 `night_NN.jwp`）与正典
`day.jwp`/`night.jwp` 同目录。选片在“**实际存在**的候选里等概率挑一个”（槽位 0 是正典，
1..8 对应 `_02.._09`），并在**每播完一轮**（JWP 循环回跳）时重掷、且尽量避开上一段 ——
所以同一场景不会一整天锁死同一段。只有一段可用时不做重开。
开机兜底顺序：`<dir>/sunny/day.jwp` → `<dir>/test/sample.jwp`；SD 未挂载或文件缺失时保留默认渐变背景。
**仓库侧壁纸源**在根 `ui/wallpaper/`（9 场景 `<day,night>` + `_02/_03` 变体，共 57 个 `.jwp`）—— **大素材不入库**（单个实测最大 82 MB、全量约 1.4 GB），属本地素材、自行拷到 SD 卡使用；仓库只保留极小样例 `test/sample.jwp`（18 KB / 3 帧）。转换脚本 `ui/assets/scripts/make_wallpaper_jwp.py`（入库）。

**本轮已生成可直接拷卡的 4 段**（本地生成于 `ui/wallpaper/`，**不入库**；素材映射与命令见
`ui/assets/README.md` §4.1–4.3）：

| 文件 | 源素材 | 体积 | 说明 |
|---|---|---|---|
| `sunny/day.jwp` | `Weather/clear_02.mp4` | 5.5MB | 晴天白天（`--grade day`） |
| `sunny/night.jwp` | `Weather/night_18.mp4` | 7.9MB | 夜景（`--grade night`；试过 `night_12` 太黑已弃用） |
| `rain/day.jwp` | `Weather/rain_11.mp4` | 2.7MB | 雨天（`--grade day`） |
| `rain/night.jwp` | `Weather/rain_11.mp4` | 2.2MB | 同素材夜色化（`--grade nightdark`），昼夜成对 |

其余场景（cloudy/overcast/storm/snow/fog/haze/night）同法一条命令即可生成；
源视频为本地素材（不在仓库内），场景命名映射见 `ui/assets/README.md` §4.2。

## 7. 性能与已知风险

- **解码余量**：IDF 自测下限为 P4 硬件解码 1080p→RGB565 ≥40fps，
  1024×600 像素量仅其 ~30%，因此默认目标 30fps 余量充足（**已实机跑通**；效果改进列入「阶段四补完」）。
- ⚠️ **HW 解码走 2D-DMA(DMA2D)**：本工程历史上观察过 DMA2D ISR 与 esp_hosted SDIO 冲突
  （见 `sdkconfig.defaults` 的 `SMART_HOME_DMA2D_ENABLE` 注释）。上板必须做
  "壁纸连续播放 + Wi-Fi 长连接"并发验证；异常时切 `SMART_HOME_WALLPAPER_DECODER_SW`。
- **帧率不匹配**：播放器按自身目标帧率顺序取帧。JWP 的 `--fps` 与
  `SMART_HOME_WALLPAPER_FPS` 不一致会改变播放快慢，转换脚本会给出提示。
- **内存**：3 个 1024×600 RGB565 缓冲 ≈ 3.6MB（PSRAM）+ 512KB JPEG 输入缓冲。
- ⚠️ **不要给待机媒体卡/视频窗口加圆角**（`radius` 或 `clip_corner`）：圆角会让 LVGL
  每帧建 ARGB8888 图层 + 生成 mask，适配器 PPA 因"不支持复杂 mask"且要求 RGB565 目标
  图层而整块退回 CPU 软件混合 —— 实测 `draw` 48ms / 17fps → 归零后 13ms / 30fps。
  详见 `docs/16` §7.6，对照数据与理由也锁在 `standby_ui.c` 文件头注释。
- ⚠️ **NVS 落盘会让面板闪一帧（平台级，已降频、未根除）**：`nvs_commit()` 期间的 flash 操作
  占用 MSPI，而 DSI 正从 PSRAM 依次拉帧缓冲 → 那一帧面板输出默认色（**整屏均匀亮蓝**）。
  与刷新模式无关，也与在哪个任务调用无关（总线物理争用）。已做降频：**规则集改为延迟合并落盘**
  （`NVS_DEBOUNCE_MS = 1500`，在 `rule_runtime_tick()` 里落一次），连拨 N 次只写 1 次 flash。
  **残留**：其它同步落盘点（如设置页「待机超时」）仍是每改一次闪一帧；语言开关因随后的大面积
  重绘把那一帧盖掉，看不见。完整证据链（含 5 个被否掉的假设）见 `docs/16` §7.8。
- **睡眠/息屏防残影**（面板规格书 9.2.3）尚未实现，属后续阶段。

## 8. 验证状态（诚实标注）

| 项目 | 状态 |
|---|---|
| ESP-IDF v6.1 编译 | 见本轮交付报告（`tools/p4build.bat`，构建目录 `build/`） |
| `jwp_reader` 宿主机单测 | 已通过（51 项检查，`ctest --test-dir tests/jwp-reader/build`） |
| 转换脚本 | 已自测（生成 18KB 样例、`--inspect` 校验、progressive 拒绝） |
| 待机层显示/双击/空闲 | **已实机验证**（docs/13 真机验收：六页 UI / 触摸 / 壁纸 / 天气 / 摄像头 / SD 字体均已跑通） |
| HW JPEG + DMA2D 解码帧率 | **已实机验证达 30fps**（目标 30；修掉圆角 mask 后 `draw` 13ms、`drop` 3/5s）。观感偏差：媒体卡/视频窗口为直角，见 §7 与 `docs/16` §7.6 |
| 天气 HTTPS/SNTP | **已实机验证**（Key 有效、功能已完成）；待机层中文文本布局待用词定稿（「阶段四补完」） |
| 阶段 5 MQTT / HA | **仅编译通过**（MQTT 关闭态 `p4-hub.bin=0x23be60` / 启用态 `0x246920`）；broker 侧本机自测通过。**实机验证受阻** —— P4 USB 数据线未接 PC，无法烧录/读日志。5 条验收（自动发现/重连/LWT/延迟对照/仲裁/全量同步）**均未实测**，见 `docs/17` §8 |

## 9. 参考与许可

JWP 容器格式与"顺序读帧/循环回跳"算法移植自
[ESP32-S3-LVGL-Clock](https://github.com/maxiumlee/ESP32-S3-LVGL-Clock)
（Apache-2.0，HEAD `299ab2e63ea85f3463edc8fd8fd75142573c50fa`）。
本工程**未引入 Arduino 框架**，未复制其工程结构/业务代码；解码使用 ESP-IDF 组件
（`esp_driver_jpeg` 硬件、`esp_new_jpeg` 软件）。详见 `docs/05-决策记录-ADR.md` ADR-018 / ADR-019。
