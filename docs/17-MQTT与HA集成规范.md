# MQTT 与 Home Assistant 集成规范（阶段 5）

> 版本 v1.0 ｜ 2026-10-19 ｜ 状态：**代码已实现、编译通过；实机验证待 P4 USB 数据线接回 PC 后进行**
> 本文是阶段 5 的核心交付物之一（Topic 命名规范 + HA Discovery 约定 + 仲裁规则）。

---

## 1. 组件与依赖

- **MQTT 客户端**：`espressif/mqtt`（registry 组件，构建名 `espressif__mqtt`，头文件 `mqtt_client.h`）。
  ⚠️ **ESP-IDF 6.1 已把 esp-mqtt 移出核心组件**（`$IDF_PATH/components` 下无 `mqtt`），
  必须经 component manager 引入。`main/idf_component.yml` 锁定 `espressif/mqtt: ^1.1.0`（实测解析 1.1.0）。
- 传输：**默认 `mqtts://<PC-IP>:8884`**（TLS，服务器证书由项目内 `certs/ca.pem` 自签 CA 校验，
  见 ADR-027）。明文 `mqtt://…:1884` 仍保留，用于回退排查与 HA 侧过渡。
  证书生成：`python tools/gen_mqtt_certs.py --ip <当前热点 IP>`（默认复用已有 CA，换 IP 只重签服务器证书）。
- 目录：`firmware/p4-hub/main/net/`
  - `topic.h/.c`　主题命名
  - `ha_entities.h/.c`　实体映射表（「加设备只改表」）
  - `ha_discovery.h/.c`　Discovery JSON 生成（cJSON）
  - `mqtt_service.h/.c`　客户端 / 后台发布任务 / 云命令队列

---

## 2. Topic 命名规范

前缀：`base = smart_home/<node_id>`，`node_id` 默认 `p4hub`（Kconfig `SMART_HOME_MQTT_NODE_ID`，可被 NVS `mqtt_node` 覆盖）。

| 方向 | Topic | QoS | Retain | 说明 |
|---|---|---|---|---|
| 订阅 | `homeassistant/status` | 1 | — | HA 重启时广播 `online` → 触发重发 discovery |
| 发布 | `homeassistant/<component>/<node_id>/<obj_id>/config` | 1 | ✔ | HA Discovery 配置 |
| 发布 | `<base>/availability` | 1 | ✔ | 设备级可用性；LWT 遗嘱 = `offline` |
| 发布 | `<base>/state/<obj_id>` | 1 | ✔ | 实体状态 |
| 订阅 | `<base>/cmd/<obj_id>` | 1 | — | 云端控制指令（`ON`/`OFF`） |

约定：状态 / discovery / availability 一律 **QoS 1 + retain**（断线后收敛、HA 重启可自恢复）。
switch 的状态载荷为纯文本 `ON`/`OFF`；sensor / binary_sensor 为 JSON `{"v":<数值>}`。

---

## 3. 实体映射表（`net/ha_entities.c`）

来源：`protocol.h` 的 `PROTO_DEV_xxx` / `PROTO_ALARM_xxx`，不另造设备模型（ADR-005）。

| HA 组件 | obj_id | dev_id | device_class | unit |
|---|---|---|---|---|
| switch | relay1 | `PROTO_DEV_RELAY1` (0x01) | — | — |
| switch | relay2 | `PROTO_DEV_RELAY2` (0x02) | — | — |
| switch | beeper | `PROTO_DEV_BEEPER` (0x04) | — | — |
| sensor | dht11_temp | `PROTO_DEV_DHT11_TEMP` (0x10) | temperature | °C |
| sensor | dht11_hum | `PROTO_DEV_DHT11_HUM` (0x11) | humidity | % |
| sensor | bmp280_temp | `PROTO_DEV_BMP280_TEMP` (0x12) | temperature | °C |
| sensor | bmp280_press | `PROTO_DEV_BMP280_PRESS` (0x13) | pressure | Pa |
| sensor | mq135 | `PROTO_DEV_MQ135` (0x14) | — | ppm |
| sensor | mq4 | `PROTO_DEV_MQ4` (0x15) | — | ppm |
| sensor | soil | `PROTO_DEV_SOIL` (0x16) | moisture | % |
| sensor | rain | `PROTO_DEV_RAIN` (0x17) | moisture | % |
| binary_sensor | human | `PROTO_DEV_HUMAN` (0x18) | motion | — |
| binary_sensor | alarm_gas | `PROTO_ALARM_GAS_MQ4` (0x01) | gas | — |
| binary_sensor | alarm_air | `PROTO_ALARM_AIR_MQ135` (0x02) | problem | — |
| binary_sensor | alarm_pump | `PROTO_ALARM_PUMP` (0x03) | problem | — |
| binary_sensor | alarm_sensor | `PROTO_ALARM_SENSOR` (0x05) | problem | — |
| binary_sensor | alarm_link | `PROTO_ALARM_LINK` (0x06) | problem | — |
| binary_sensor | alarm_storage | `PROTO_ALARM_STORAGE` (0x07) | problem | — |

> `PROTO_DEV_LOCK`(0x03) / `PROTO_ALARM_LOCK_FAIL`(0x04) **不接入**：实装门锁模块与设计不符，2026-10-18 已删除锁相关代码（ADR-010/023）。
> 单位取「F103 上报给 P4 的原始单位」，不做换算（如 BMP280 压力为 Pa）。**实机复核单位后如与 HA 器件类不匹配再调整。**

---

## 4. HA Discovery JSON 约定

每个实体发布一份配置到 `homeassistant/<component>/<node_id>/<obj_id>/config`，关键键：

| 键 | 值 / 说明 |
|---|---|
| `name` / `unique_id` / `object_id` | `unique_id = <node_id>_<obj_id>` |
| `state_topic` | `<base>/state/<obj_id>` |
| `command_topic` | `<base>/cmd/<obj_id>`（仅 switch） |
| `availability_topic` + `payload_available`/`payload_not_available` | `<base>/availability` / `online` / `offline` |
| `value_template` | sensor：`{{ value_json.v }}`；binary：`{{ 'ON' if value_json.v \| int(0) > 0 else 'OFF' }}` |
| `qos` / `retain` | 1 / true |
| `expire_after` | **60**（见下） |
| `device` | 分组：`identifiers=<node_id>-hub`、`name=Smart Home Hub`、`mf/model/sw_version` |
| `device_class` / `unit_of_measurement` / `state_class` / `icon` | 按表 §3 填充 |

**`expire_after=60` 的用意（诚实呈现，不粉饰）**：实体 60s 内无更新，HA 自动标记 `unavailable`。
发布侧每 **20s 强制全量重发**一次，正常在线不会误判；而 **P4↔F103 链路断开 / 传感器 `quality != VALID` 时主动停发**，
到期后 HA 如实显示「设备在线但无数据」。这是当前「F103 未连」状态下唯一正确的对外表征。

---

## 5. 连接、遗嘱（LWT）与重连

- **LWT**：连接时向 broker 注册遗嘱 `topic=<base>/availability, payload=offline, qos=1, retain=1`。
  客户端非正常断开（断电、强杀、Wi-Fi 掉）时由 broker **代发 `offline`** → HA 依据 `availability_topic` 标记设备离线。
- **client_id** = `p4hub-<node_id>-<MAC 后 3 字节>`（唯一，避免同 id 互踢）。
- keepalive 30s；`reconnect_timeout_ms=2000`；clean_session=true。
- buffer 2048 B（discovery JSON ~600 B，默认 1024 偏小）。

### 重连后的全量同步（验收项 ⑤）
`MQTT_EVENT_CONNECTED` → 后台任务执行：发 `availability=online` → **重发全部 discovery** → **全量状态发布**。
增量：`mqtt_pub_task` 每 500 ms 轮询协议影子（`AppProto_GetSensorShadow/GetActuatorShadow/GetLinkState`），
与上次已发布缓存做带死区（`STATE_DEADBAND=0.05`）的 diff，仅变化项发布；另每 20s 强制全量一次。

### HA 重启
订阅 `homeassistant/status`；收到 `online` 即置「重发 discovery」标志 → 后台任务重新发布全部配置。

---

## 6. 本地优先仲裁（ADR-004）

> ADR-004：控制走本地闭环，MQTT/HA 只做状态上报与远程访问；**本地操作后 1s 内的云指令不生效**。

实现（最小改动，UI / 规则引擎的既有调用点零改动）：

- `app_proto.h` 新增 `proto_cmd_src_t { LOCAL, CLOUD }` 与
  `AppProto_SendActuatorCmdEx(dev_id, action, ttl_ms, src)`；
  原 `AppProto_SendActuatorCmd()` 降级为 `src=LOCAL` 的薄包装。
- 来源 = LOCAL 时记录 `s_last_local_cmd_ms[dev_id]`（esp_timer 毫秒，静态数组，**不落 NVS**）。
- 来源 = CLOUD 时，若 `now - last_local < PROTO_CLOUD_ARB_WINDOW_MS(=1000)` → **丢弃**并打
  `[ARB] cloud cmd dropped: local op Nms ago`。

### 云命令的线程边界（重要）
协议层 `ProtoTx` 是**单发送槽、无锁**实现。云命令若在 MQTT 事件任务里直接调用 `ProtoTx`，
会与主任务/协议轮询任务并发写同一发送槽。因此：
**MQTT 事件处理只把指令投进 `CLOUD_CMD_Q_LEN=4` 的队列**，
由 `app_main` 主循环在 `AppProto_Poll()` 之后取出并 `AppProto_SendActuatorCmdEx(..., CLOUD)` 统一下发。
（队列实现见 `mqtt_service_take_cloud_cmd()`。）
> 注：UI（LVGL 任务）与规则引擎（主任务）本就直接调用协议层，属既有行为；本阶段不对其做改动，
> 但云命令这条**新增**路径已收敛到主任务上下文，避免新增竞争。

---

## 7. 配置与凭据（ADR-016）

真实 broker 地址 / 账号 / 密码**绝不入库**。

| 项 | Kconfig | NVS 键 | 默认 |
|---|---|---|---|
| 总开关 | `SMART_HOME_MQTT_ENABLED` | — | **n**（opt-in） |
| broker URI | `SMART_HOME_MQTT_BROKER_URI` | `mqtt_uri` | 空 |
| 用户名 | `SMART_HOME_MQTT_USERNAME` | `mqtt_user` | 空 |
| 密码 | `SMART_HOME_MQTT_PASSWORD` | `mqtt_pass` | 空 |
| 节点 ID | `SMART_HOME_MQTT_NODE_ID` | `mqtt_node` | `p4hub` |
| keepalive | `SMART_HOME_MQTT_KEEPALIVE_S` | — | 30 s |
| 允许云端控制 | `SMART_HOME_MQTT_ALLOW_CLOUD_CMD` | — | y |

- 运行时写 NVS：`storage_save_mqtt_cred(uri, user, pass)`（`main/bsp/storage`，namespace 固定 `storage`）。
- `uri` 为空 → `mqtt_service_start()` 只打一条日志并返回，**不启动、不影响系统启动**。

---

## 8. 验证清单

### 已完成的验证
- ✅ P4 全量编译通过（含 `espressif__mqtt` 组件、`net/*` 四个源文件）：
  - **MQTT 关闭态**（默认 `n`）：`p4-hub.bin = 0x23be60`，分区空闲 63%。
  - **MQTT 启用态**（`SMART_HOME_MQTT_ENABLED=y` + 临时 broker URI）：`p4-hub.bin = 0x246920`，分区空闲 62%；
    该次构建覆盖了 `app_main` 的启用分支与「云命令主循环 drain」代码路径。
- ✅ broker 侧自测（本机）：Mosquitto 2.1.2 已安装并验证 pub/sub 回环正常。
- ✅ 静态审查：云命令路径已收敛到主任务上下文（见 §6）。

### 2026-10-24 实机实测结果

测试拓扑：P4（USB-Serial/JTAG，`COM7`，MAC `80:f1:b2:d3:99:1d`）↔ F103（UART，`link=UP`）；
P4 与 PC 同连手机热点 `iqoo`：P4 `10.48.69.243`、PC `10.48.69.2`；
broker = 用户态 Mosquitto 2.1.2 监听 `0.0.0.0:1884`。

| 验收项 | 实测证据 | 结论 |
|---|---|---|
| ① 自动发现 | 订阅 `#` 收到 **18 条** `homeassistant/<component>/p4hub/<obj_id>/config`，字段完整（`avty_t`/`exp_aft=60`/`value_template`/`device`）；真实 HA（HA OS 17.3 / Core 2026.6.1）中添加 MQTT 集成后，**18 个实体全部自动出现** | ✅ broker 侧 + **真实 HA 均通过** |
| ② 断网重连 <10s | 杀 broker → 隔 10s 重启：P4 检测断开于 `176009 ms`，`187080 ms` 打出 `broker connected`；broker 启动于 `16:47:14.211`，换算重连耗时 **≈1.1s** | ✅ 远小于 10s |
| ② LWT | broker 日志 `Will message specified (7 bytes) (r1, q1)`；复位后新连接接管旧会话时 broker 立即发 `offline`（新会话 `online` 紧随其后） | ⚠️ **原「强杀 3s 内」标准不成立**，见下 |
| ③ 本地 vs MQTT 延迟 | 见下方「时延分解」 | ✅ 已完成对照：本地 **≈19ms**；云指令 P4 内 **22～67ms**（优化前受 500ms 上报节拍限制为 216～563ms） |
| ④ 云指令 + 仲裁 | `pub cmd/relay1 ON/OFF` 闭环；本地操作后连续下发 18 条，串口出现 **4 条 `[ARB] cloud cmd dropped: local op 38 / 356 / 83 / 479 ms ago (<1000ms)`**，窗口外指令正常下发 | ✅ 云指令通路 + **1s 拒绝分支实测触发** |
| ④+ 真实 HA 控制闭环 | 在 HA 设备页点击 `Living Light`，broker 抓包记录 4 组回读（`18:23:44.954 ON → 46.012 OFF`、`47.205 ON → 48.430 OFF`、`55.803 ON → 56.213 OFF`、`18:24:16.893 ON → 17.714 OFF`） | ✅ HA→P4→F103→回读→HA 闭环；状态是 **ACK 回读**而非乐观显示 |
| ⑤ 重连后全量收敛 | 重连后串口 `discovery published (18 entities)`，broker 随即收到 availability + 全部 state 主题 | ✅ |
| 链路健康 | `link=UP`、`HELLO_RSP ver=2 caps=0xFFF name=F103-HUB`；启动时 2 个 `crc_err`，之后恒定不增 | ✅ |

**时延分解（③ 对照表，2026-10-24 实测）**

| 路径 | 时延 | 数据来源 |
|---|---|---|
| **本地**：触摸点击 → 继电器 ACK（端到端） | **18.87ms**（本次）；`min 16.47 / avg 19.63 / max 51.80`，n=71 | P4 `LATENCY` 汇总日志 |
| 本地：点击回调入口 → `ACTUATOR_CMD` 发出 | **7.99ms**（含 `ESP_LOGI` 串口开销） | 新增 `UI_LAT` 打点 |
| 本地：F103 收到 → GPIO 动作 | **0.29ms** | 同上 `LATENCY` 日志 |
| 云指令：P4 收到 → 下发 `ACTUATOR_CMD` 到 F103 | **0～7ms**（`269413→269417`=4ms、`270084→270091`=7ms） | 串口 ESP 时间戳 |
| 云指令：P4 收到 → 状态实际上报（优化后） | **22 / 67 / 23 / 33 / 30 / 33 ms**（`MQTT_LAT` 打点，6 次） | 串口 `[LAT] cloud cmd -> state pub` |
| 云指令端到端（优化前）：PC 发布 → PC 收 `state` | **216 / 483 / 563ms**（3 次） | PowerShell 订阅作业完成时间差 |

**对照结论**：本地操作约 **19ms**；云端指令在 P4 内部与本地同量级。
优化前「PC 发布 → PC 看到状态」被 **500ms 状态上报节拍**放大到 200～560ms。

**2026-10-24 优化（已完成并烧录验证）**：`mqtt_pub_task` 的轮询粒度从 500ms 改为 **25ms**
（`PUB_TICK_MS=25`），每个 tick 只轮询执行器影子（`publish_actuators()`），
全量增量仍保持 500ms、强制全量仍 20s。改动后 P4 侧「收到云指令 → 状态入发送队列」实测 **22～67ms**。

**诚实说明（未把它算成 <100ms 验收）**：在本机用 Node 原生 socket 做 PC 侧端到端复测，
同一次运行的 PC 侧读数在 **56～2128ms** 之间大幅抖动，而 P4 侧同批只有 22～67ms；
抖动来源是 Windows 侧 TCP（Nagle / 延迟 ACK）与测量客户端调度，不是 P4 应用层。
因此：
- **设备侧确定结论**：应用层改造确实去掉了 500ms 节拍，P4 内部 ≤ 67ms。
- **端到端 <100ms 未取得可信证据**，需在真实 HA（Linux）或独立测量主机上复测后再下结论。



**LWT 时延的诚实结论**：强杀（断电/复位）时 Wi-Fi 侧不会发 TCP RST，
broker 只能等 keepalive 超时（`1.5 × 30s = 45s`）才判定离线。
本次观察到的「秒级 offline」是**新连接用同一 client_id 接管**触发的遗嘱下发，不是链路检测。
因此 3s 目标在当前 `keepalive=30` 下不可达；实际兜底是 HA 的 `expire_after=60`。
若要压缩，可把 `SMART_HOME_MQTT_KEEPALIVE_S` 降到 5～10（代价：PING 频率上升）。

**其它实测发现**：
- 云指令日志里 `→`/`—` 等非 ASCII 字符在串口显示为 `???`（`[CMD] cloud ??? relay1 ON`），
  仅影响可读性，不影响功能。
- 本机防火墙**未拦截** P4 对 `1884` 的入站连接（原「待验证」项已闭环）。
- P4 的 `mqtt_uri` 走 Kconfig 编译期值；换热点后 IP 变了需改 `sdkconfig` 重编（NVS 键 `mqtt_uri` 可覆盖）。

### 测试环境现状（本机侦察）
- 本机**无 Docker**；`winget` 可用。
- **Mosquitto 2.1.2 已安装**，作为 Windows 服务（`Automatic`）运行，默认**只绑 localhost:1883**
  （LAN IP 连接被拒 —— 已实测确认）。要暴露到 LAN 需改 `C:\Program Files\Mosquitto\mosquitto.conf`
  （`listener 1883 0.0.0.0` + `allow_anonymous true`）并重启服务，**需要管理员权限**（当前会话非提权）。
- **已用免管理员方案起了一个用户态 broker**：`mosquitto.exe -c %TEMP%\mqtt-broker\mosquitto.conf`
  监听 `0.0.0.0:1884`。当前验证用地址：**PC `10.48.69.2:1884`**（PC 与 P4 同连 `iqoo` 热点后），
  P4 的 `CONFIG_SMART_HOME_MQTT_BROKER_URI` 已改为该地址。
- ✅ **Windows 防火墙未拦截** P4（`10.48.69.243`）连入 `10.48.69.2:1884`（2026-10-24 实测）。
- 本机**无 Home Assistant**（HA 通常需 Docker/HA OS/独立主机，本机不具备）。
  → 因此「HA 中自动发现实体」这条**严格意义上的验收需要一台真实 HA**（用户侧是否有独立 HA 待确认）。

---

## 9. 未闭环项（阶段 5 内部 TODO）

1. ~~**实机验证剩余项**：③ 本地 vs MQTT 延迟对照、④ 仲裁 reject 分支、① 真实 HA 中的实体呈现。~~
   → 2026-10-24 全部完成；① 已在真实 HA OS 17.3 / Core 2026.6.1（VMware 虚拟机，`192.168.130.128`）中验证：
   MQTT 集成（broker `192.168.130.1:1884`）添加后，**Smart Home Hub 设备下 18 个实体全部出现**
   （3 switch / 8 sensor / 7 binary_sensor，名称、单位、中文状态均按 discovery 渲染）。
2. **Broker 环境**：已闭环（PC 与 P4 同热点 + 用户态 broker `:1884`）；HA 已用真实实例（VM 内 HA OS）。
3. **LWT 时延**：`keepalive=30` 下强杀最差 45s；若产品要求更短需下调 keepalive（见 §8 诚实结论）。
4. **设备侧数据质量问题（HA 首屏暴露）——2026-10-24 已修并烧录验证**：
   - `bmp280_temp` 曾上报 **0.000 且 quality=VALID**：根因是 F103 的 `app_proto.c` 里
     `APPEND_SENSOR(PROTO_DEV_BMP280_TEMP, 0, 0.0f)` 写死 0，且 `BSP_BMP280_ReadData`
     把 `CompensateTemp()` 的返回值直接丢弃（`bmp280_data_t` 里根本没有温度字段）。
     已补齐：驱动保存温度 → `g_sensor_data.bmp280_temp` → 协议帧真实上报。
     实测：串口 `BMP280: 103425Pa 29.4C [OK]`，broker `bmp280_temp={"v":29.350}`。
   - `soil` 曾在湿度过渡区**每秒在 74~100% 间跳变**（adc 摆动约 600 counts）；
     静置稳定湿度时实测 60s 仅 49~50%（±1%）。已加 **5 点时间中值 + ≥20 点跳变日志**；
     实测平滑后 `raw 43% → 44%`，broker `soil=41~44`。
     诚实边界：这是显示层稳定化，"探头半湿时接触电阻不稳"这一物理因素未消除，
     不复现也不代表根治（见 `app_soil.c` 注释）。
5. **TLS**：已于 2026-10-30 落地（ADR-027）。设备默认连 `mqtts://<PC-IP>:8884`，
   服务器证书由自签 CA 校验；CA 证书以 `certs/ca.pem` 打进 mbedtls custom bundle。
   运维规则：换热点 IP 只需重跑 `tools/gen_mqtt_certs.py --ip <新IP>`（脚本复用原 CA，
   仅重签服务器证书），设备**无需重烧**；明文 `1884` 保留作回退，生产应关闭。
