# 阶段三运行时资产说明

阶段三开发用的 HTML 原型、图片/字体母版已由 IDF/LVGL 缓存中的 Source Han Sans SC 重新生成当前文案子集。固件运行时不依赖这些源文件：

- UI 图标、首页插图和 CJK fallback 字体已固化为 `firmware/p4-hub/main/ui/` 下的 LVGL C 资源，并随固件编译。
- 真机 SD 卡只需要放置运行时字体（如启用 SD 字体加载）和天气壁纸 JWP 文件。
- 壁纸容器格式与转换工具保留在 `ui/assets/scripts/make_wallpaper_jwp.py`；仓库侧壁纸源在根 `ui/wallpaper/`，大体积源视频/ZIP 不入库。

## SD 卡目录

```text
/fonts/cn_font/fontcn14.bin
/fonts/cn_font/fontcn16.bin
/fonts/cn_font/fontcn18.bin
/fonts/cn_font/fontcn24.bin
/fonts/cn_font/fontcn32.bin
/fonts/cn_font/fontcn48.bin
/wallpaper/<scene>/day.jwp
/wallpaper/<scene>/night.jwp
```
场景目录为 `sunny`、`cloudy`、`overcast`、`rain`、`storm`、`snow`、`fog`、`haze`、`night`。缺失场景会回退到默认壁纸或渐变背景。

## JWP 校验

```powershell
python ui/assets/scripts/make_wallpaper_jwp.py --inspect <file>.jwp
```

JWP 目标为 1024×600、baseline JPEG；帧率必须与 `SMART_HOME_WALLPAPER_FPS` 一致。素材制作在仓库外完成，生成后仅将需要的 JWP 拷贝到 SD 卡。

> 母版（字体/图标/图片源与中间产物）与 HTML 原型**不在仓库内**；仓库只保留运行时需要的
> `generated_cn_font/` 与 `scripts/`。已固化到固件的资源见 `firmware/p4-hub/main/ui/`
> （`fonts/`、`images/` 的 LVGL C 资源）。

## 目录结构（仓库实际内容）

```
ui/assets/
├── generated_cn_font/  运行时 CJK 字体 fontcn{14,16,18,24,32,48}.bin（拷到 SD）
├── scripts/            make_wallpaper_jwp.py（视频/图片序列 → JWP，含 --inspect 校验）
└── README.md
```

## 1. 字体(双层机制,`tools/generate_runtime_fonts.mjs` 一键生成,已验证)

| 产物 | 源 | 字号 | 用途 |
|---|---|---|---|
| fontcn14~48.bin ×6 | 思源黑体 SC + 内置字体字符集 + ASCII/中文标点/°/·/→ | 14/16/18/24/32/48 | 页面实际中文与混排文本(SD 卡加载) |

可用 `node tools/generate_runtime_fonts.mjs` 重新生成工程内的 `generated_cn_font/`。脚本使用
`E:/SourceHanSansCN-Normal.otf`、4bpp、无压缩、无 kern 表，匹配当前 LVGL binfont loader；生成
后再将六个同名文件复制到 SD 卡 `/fonts/cn_font/`。脚本同时刷新模拟器的 `simulator/fonts/cn_font/`。
| fontlt_pjs24/32sb/44/64/84/96.bin ×6 | Plus Jakarta Sans(npmmirror @fontsource 5.2.5) | 24/32/44/64/84/96 | 数字/拉丁:卡片值、H1、Hero 84、壁纸钟 96 |

- 字重:PJS Light(300)=84/96,Regular(400)=24/64,Medium(500)=44,SemiBold(600)=32
- ⚠️ used_chars.txt 继承自学习盒工程,需随 UI 中文文案增量补充再重跑
- 产物体积:CJK 714KB~3.1MB(SD);Latin 11~136KB(可进 Flash)

实际运行时的 CJK bin 统一放在 `fonts/cn_font/`，模拟器镜像目录为
`simulator/fonts/cn_font/`；真机 SD 卡应复制同名六个文件到
`/fonts/cn_font/`。页面字号映射为 `F10/F14→14`、`F18→18`、
`F28→24`、`F48→48`。旧的 `fontcn12.bin`、`fontcn20.bin`、
`fontcn26.bin`、`fontcn36.bin` 不再被加载，可从 SD 卡目录移除。
本项目页面使用的精简字符集取自内置字体 `firmware/p4-hub/main/ui/fonts/ui_font_cjk_18.c`
的 `--symbols` 列表（`generate_runtime_fonts.mjs` 直接复用，无独立字符文件），避免大字号 bin
加载整套 CJK 字库造成错乱。F10/F12/F14 统一使用 `fontcn14.bin`，
因此箭头、中文标点和“待确认”等混排字符不会再走 Montserrat 方框回退。

## 2. 图标 —— 走图标字体方案(决策)

- 源:`lucide-static@1.343.0`(npmmirror 白名单版)；母版 SVG 与 `lucide.ttf` **不在仓库内**，集成产物见 `firmware/p4-hub/main/ui/images/reicon_assets.c` / `reicon_nav_assets.c`
- **用 `font/lucide.ttf` + `codepoints.json`**:21 个所需图标(lightbulb/wind/lock/
  blinds/fan/activity/wifi/droplet/sun/moon/home/layout-grid/circle-play/log-out/
  sliders-horizontal/settings/arrow-left/check/power/chevron-right/thermometer)
  码点全部命中(0xe1c2 等),集成期 `lv_font_conv --range` 抽取成图标字体
- 优点:一个字体覆盖全部图标、运行时 `lv_obj_set_style_text_color` 随状态染色
  (对应原型的 icon-chip 着色),替代逐个 SVG→C 数组

## 3. 图片(PIL 母版生成,可复现;母版不在仓库,集成产物已入固件)

| 文件 | 尺寸 | 用途 | 转 LVGL |
|---|---|---|---|
| img_bg_home.png | 1024×600 | 首页背景:深底+三团光晕+噪声 | C 数组,全屏 lv_image |
| img_sky_day/night.png | 1024×600 | 天气壁纸昼/夜天空 | C 数组,交叉淡化切换 |
| img_curtain_light.png | 400×260 | 天幕光(黄昏渐变) | 九宫格拉伸,宽度=开合度 |
| img_curtain_panel.png | 28×260 | 帘片褶皱周期纹理 | LV_IMAGE_ALIGN_TILE 平铺 |
| img_card_sheen.png | 128×128 | 玻璃卡斜向光反射(叠加层) | 叠在玻璃卡上层,低透明度 |
| img_star.png / _big.png | 12/18px | 星点(运行时透明度闪烁) | 小 C 数组 × 14 实例 |

## 4. 待机壁纸(ADR-018 / ADR-019 —— 用户自行下载素材,脚本统一加工)

- **容器格式:JWP**(32B 头 + 逐帧 baseline JPEG),不是 AVI —— 为什么换见 ADR-018。
  权威格式定义在 `firmware/p4-hub/main/standby/jwp_reader.c` 与
  `ui/assets/scripts/make_wallpaper_jwp.py`(两处必须同步修改)。
- 目标:9 场景 × 昼夜 = 18 段无缝循环,1024×600@15fps,baseline JPEG
- 估算:10s 段 ≈ 9MB(单帧 ≈60KB)→ 12~18 段 ≈ 110~160MB;SD 建议 ≥1GB FAT32 Class10
- 真机播放:P4 硬件 JPEG 解码器(`esp_driver_jpeg`,走 2D-DMA)逐帧解码 + 流式读 SD;
  待机层隐藏时暂停。软件解码 `esp_new_jpeg` 为回退选项(Kconfig 可切)

**工作流**:
1. 从免版权图库下载素材(**可商用、无需署名**):[Pexels Videos](https://www.pexels.com/videos/)、
   [Pixabay](https://pixabay.com/videos/)、[Mixkit](https://mixkit.co/)、[Coverr](https://coverr.co/)
2. 搜索词:晴="sunny sky timelapse" / 多云="clouds timelapse" / 雨="rain drops on window" /
   雪="snow falling" / 雾="fog forest" / 雷雨="lightning storm";夜晚搜 "night ..." 或直接用昼素材夜色化
3. 一条命令加工(自动裁剪 1024×600、按昼夜调色、抽帧为 baseline JPEG 并打包):

```bash
python ui/assets/scripts/make_wallpaper_jwp.py \
  --input 下载的素材.mp4 --output ui/wallpaper/rain/day.jwp \
  --width 1024 --height 600 --fps 15 --quality 4 --duration 10 --max-frame 512
# 生成后自检(逐帧尺寸 / baseline / CRC32 / 单帧上限):
python ui/assets/scripts/make_wallpaper_jwp.py --inspect ui/wallpaper/rain/day.jwp
```
   夜间版加 `--night`(仅对 `--image/--gradient` 合成路径生效;视频请用夜景素材)。
4. 无缝循环:优先选自带循环感的空镜(雨滴/雪落/雾流动);带片头的素材可用
   `--transition-frame N` 让片头只播一次(对应 JWP 头的 `transition_offset`)。
5. 部署:`/sdcard/wallpaper/<scene>/{day,night}.jwp`;仓库侧源素材在根 `ui/wallpaper/`
   （含极小的 `ui/wallpaper/test/sample.jwp`,3 帧/18KB,用于无素材时点屏验证）,
   云端源视频/ZIP 与中间产物不入库。
- ffmpeg:优先 `PATH` 上的 `ffmpeg`,也可 `--ffmpeg <路径>`;仓库内 `tools/bin/ffmpeg.exe`
  当前不存在,首次使用请自备或改用 `--frames-dir`(先用别的工具导出 JPEG 序列)。

### 4.1 调色分级 `--grade`(仅视频输入,沿用 ADR-019 定稿)

| 取值 | ffmpeg 滤镜 | 用途 |
|---|---|---|
| `day`(默认) | `eq=brightness=-0.05:saturation=0.92` | 白天素材轻度压暗,保证待机层白字可读 |
| `night` | `eq=brightness=-0.04:saturation=0.9` | 素材本身就是夜景,**不要再加深**,否则黑成一团 |
| `nightdark` | `eq=brightness=-0.14:saturation=0.75,colorbalance=rs=-0.08:bs=0.10` | 用白天素材凑夜段 |
| `none` | 无 | 原样 |

### 4.2 素材命名映射(本地源视频 ZIP 内 93 段 MP4，不在仓库内)

| ZIP 里的场景前缀 | 本工程的场景目录 | 备注 |
|---|---|---|
| `clear_*` | `sunny` | 晴 |
| `cloudy_*` | `cloudy` | 多云 |
| `overcast_*` | `overcast` | 阴 |
| `rain_*` | `rain` | 雨 |
| `thunder_*` | `storm` | 雷暴 |
| `snow_*` | `snow` | 雪 |
| `fog_*` | `fog` | 雾 |
| `haze_*` / `sand_*` | `haze` | 霾 / 扬沙(归入 haze) |
| `night_*` | `night` | 纯夜景(也可用作任意场景的 night 段) |

**当前已生成(存于 `ui/wallpaper/`，**不入库**——大素材本地保存，直接拷到 SD 即可)**——源文件取体积最小的几段:

```bash
FF=%TEMP%\ffbin\ffmpeg.exe   # 见下方"ffmpeg 从哪来"
python ui/assets/scripts/make_wallpaper_jwp.py --ffmpeg $FF \
  --input <解压出的 clear_02.mp4> --output ui/wallpaper/sunny/day.jwp \
  --width 1024 --height 600 --fps 15 --duration 8 --quality 4 --grade day
# sunny/night.jwp ← night_18.mp4  --grade night
# rain/day.jwp    ← rain_11.mp4   --grade day
# rain/night.jwp  ← rain_11.mp4   --grade nightdark
```

| 文件 | 帧数 | 体积 | 首帧亮度 | 源素材 |
|---|---|---|---|---|
| `sunny/day.jwp` | 83 | 5.5MB | 121 | `Weather/clear_02.mp4` |
| `sunny/night.jwp` | 120 | 7.9MB | 55 | `Weather/night_18.mp4` |
| `rain/day.jwp` | 120 | 2.7MB | 30 | `Weather/rain_11.mp4` |
| `rain/night.jwp` | 120 | 2.2MB | 21 | `Weather/rain_11.mp4` + `nightdark` |
| `test/sample.jwp` | 3 | 18KB | 44 | 脚本 `--gradient --night` 合成 |

> 亮度是选素材的实用判据:夜景实测 `night_12` 只有 17(近黑,已弃用),
> `night_18`=67、`night_08`=36 可读;`rain_11`(44) 比 `rain_07`(88) 更符合雨天氛围。

### 4.3 ffmpeg 从哪来(本机无二进制时的零安装做法)

本机 `E:\FFMPEG\FFmpeg-master` 是**源码树(没有编译产物)**,`PATH` 上也没有 ffmpeg。
不改 Python 环境拿到可用 ffmpeg 的办法——下载 wheel 后只取里面的静态二进制:

```bash
python -m pip download imageio-ffmpeg --no-deps -d %TEMP%\ffdl
# wheel 即 zip:imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v7.1.exe (87MB, 静态构建)
```
解出后即可 `--ffmpeg %TEMP%\ffbin\ffmpeg.exe`。若要长期使用,建议放到
`tools/bin/ffmpeg.exe`,并注意 87MB 二进制**不要入库**(自行在 `.gitignore` 增加该文件)。

## 5. 转换命令备忘(集成期)

```bash
# 字体(已验证): node tools/generate_runtime_fonts.mjs   # 生成 ui/assets/generated_cn_font/
# 图标/图片母版已不在仓库内;集成产物已固化为固件 C 资源:
#   firmware/p4-hub/main/ui/images/reicon_assets.c / reicon_nav_assets.c / img_*.c
```
