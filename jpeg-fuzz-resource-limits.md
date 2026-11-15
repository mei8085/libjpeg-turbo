# libjpeg-turbo 模糊测试资源消耗排查报告：渐进扫描次数、霍夫曼表与虚拟数组

日期：2026-11-15
代码版本：分支 `task/75`（HEAD `3ef3367`）
构建：`cmake -DWITH_SIMD=0 -DENABLE_SHARED=0 -DCMAKE_BUILD_TYPE=Release`（静态 `cjpeg-static` / `djpeg-static` / `tjbench-static`）

## 1. 结论摘要

对模糊生成的 JPEG 码流，解码路径上三类资源的管控现状不对称：

| 资源 | 库核心（libjpeg API）是否有上限 | 应用层（djpeg / TurboJPEG）是否有上限 | 放大效应 |
|---|---|---|---|
| 渐进扫描次数（SOS 数量） | **无** | 有（默认关闭，需显式开启） | CPU 时间随扫描数线性放大，输入字节极少 |
| 霍夫曼表（DHT 数量/重定义） | 内存天然有界（4+4 表槽），解析耗时与输入字节线性 | 无需额外限制 | 无放大（~2 MB DHT 仅耗时 <10 ms） |
| 虚拟数组（全图系数缓冲） | **默认无**（`max_memory_to_use = 0` 表示无限） | 有（默认 0 = 无限，需显式设置） | 内存随 SOF 声明尺寸放大，1.7 KB 输入可分配 8.4 GB |

**最需要补上限的解析阶段**是：
1. `get_sos()` / `consume_markers()`（`src/jdmarker.c:309`、`src/jdmarker.c:330`）——扫描计数 `input_scan_number++`（`src/jdmarker.c:382`）无任何上界；
2. `realize_virt_arrays()` 之前的内存预算来源 `jpeg_mem_init()`（`src/jmemnobs.c:101`）——默认返回 0（无限），且 `jmemnobs.c` 无后备存储（backing store），超限前会把全部系数数组真实分配进 RAM。

霍夫曼表不需要新的内存上限（见 §3.2），其消耗与输入规模成正比，不构成放大攻击面。

## 2. 排查范围与方法

- 静态阅读标记解析（`jdmarker.c`）、输入控制（`jdinput.c`）、熵解码（`jdhuff.c`/`jdphuff.c`）、系数控制（`jdcoefct.c`）、内存管理（`jmemmgr.c`/`jmemnobs.c`）、TurboJPEG 封装（`turbojpeg.c`）与两个命令行前端（`djpeg.c`/`cdjpeg.c`、`tjbench.c`）。
- 用脚本生成小型触发样本（生成器：`analysis-scripts/gen_samples.py`、`analysis-scripts/gen_manyscans.py`；样本：`analysis-samples/`），分别用 libjpeg API 入口（`djpeg-static`）和 TurboJPEG 入口（`tjbench-static`）对比解码耗时与峰值 RSS（`/usr/bin/time -v`）。
- 对照上游模糊目标（`fuzz/decompress.cc`、`fuzz/decompress_libjpeg.cc`）确认官方对三类资源的设防位置。

## 3. 代码路径分析

### 3.1 渐进扫描次数：核心无上限，非法 progression 仅告警

- SOS 标记由 `get_sos()` 解析（`src/jdmarker.c:309`）。它只校验本扫描的分量数（`n > MAX_COMPS_IN_SCAN`，`src/jdmarker.c:326`），随后无条件执行 `cinfo->input_scan_number++`（`src/jdmarker.c:382`）。**全库没有任何对扫描总数的检查。**
- 多扫描文件由 `consume_markers()` 循环接受后续 SOS（`src/jdmarker.c:350-354`），只要 `has_multiple_scans` 为真（渐进或多分量非交织文件必然为真，`src/jdinput.c:152-156`）就无限接受。
- 渐进参数合法性在 `start_pass_phuff_decoder()`（`src/jdphuff.c:81`）校验：Ss/Se/Ah/Al 越界会 `ERREXIT`（`src/jdphuff.c:119-121`），但**重复扫描同一谱带/同一逼近位只产生告警** `JWRN_BOGUS_PROGRESSION`（`src/jdphuff.c:130-143`），解码照常进行。因此把同一个扫描复制任意多次都"合法通过"。
- 每个扫描的代价：一次完整的熵解码输入遍（`jdcoefct.c` 的 `consume_data` 按 MCU 循环调用 `decode_mcu`）+ 每扫描重建派生霍夫曼表（`src/jdphuff.c:159-176`，注释明确写道 "We may build same derived table more than once"）。即使扫描数据用 EOBRUN 压缩到几个字节，`decode_mcu` 仍按 MCU 逐个调用，CPU 开销 ≈ 扫描数 × 图像 MCU 数。
- 输出侧 `jpeg_start_decompress()`/`jpeg_start_output()` 以 `input_scan_number` 为循环上界（`src/jdapistd.c:810`、`src/jdcoefct.c:317`），扫描数直接决定总遍数。

**已有限制仅在库外**：
- djpeg：`-maxscans N`，在进度回调里检查（`src/cdjpeg.c:34-40`），默认 0 = 不限（`src/djpeg.c:211`）。
- TurboJPEG：`TJPARAM_SCANLIMIT` / `TJFLAG_LIMITSCANS`（置 500），在 `my_progress_monitor()` 里 longjmp 中止（`src/turbojpeg.c:169-189`、`src/turbojpeg.c:600`）。实例结构体被 `memset` 清零（`src/turbojpeg.c:630`），**默认 `scanLimit = 0` = 不限**。
- 上游 fuzz 目标全部显式设限：`fuzz/decompress.cc:95-97`（非无损 100、无损 50）、`fuzz/decompress_libjpeg.cc:222`（`input_scan_number > 100` 即 bail）——这本身就说明核心库缺这个上限。

### 3.2 霍夫曼表：内存天然有界，重复定义只花线性 CPU

- `get_dht()`（`src/jdmarker.c:438`）在单个标记段内循环解析表定义，段长被 16 位长度字段限制（≤ 65533 字节）；表索引被 `NUM_HUFF_TBLS`（= 4，`src/jpeglib.h:69`）限制（`src/jdmarker.c:485-494`）。
- 重复定义同一槽位时 `jpeg_alloc_huff_table` 只在首次分配（`src/jdmarker.c:496-497`），之后是 `memcpy` 覆盖（`src/jdmarker.c:499-500`）。**DHT 标记出现多少次，霍夫曼表存储都恒为 4 DC + 4 AC 个 `JHUFF_TBL`，内存不增长。**
- 派生表（解码用加速结构）同样按槽位缓存、每扫描重建（`src/jdhuff.c:143-170`），数量有界。
- 实测（§4.2）确认：2 万个 DHT 段（2.1 MB）解析耗时 <10 ms、RSS 无变化。DHT 重定义是 T.81 允许的合法行为（编码器可在扫描间换表），也不应加以限制；其 CPU 消耗与输入字节数成正比，无放大效应。

### 3.3 虚拟数组：默认内存预算为"无限"，无后备存储兜底

- 渐进/多扫描解码在 `_jinit_d_coef_controller()` 为每个分量申请全图虚拟块数组（`src/jdcoefct.c:903-909`），尺寸 = `width_in_blocks × height_in_blocks × sizeof(JBLOCK)(128 字节)`，由 SOF 声明的宽高直接决定，与文件中实际熵数据量无关。
- 虚拟数组在 `realize_virt_arrays()`（`src/jmemmgr.c:696`）才真正分配，可用内存由 `jpeg_mem_available()` 报告（`src/jmemnobs.c:65-78`）：当 `max_memory_to_use == 0` 时直接返回 `max_bytes_needed`——"we got all you want bud!"。
- 默认值链条：`jpeg_mem_init()` 返回 0（`src/jmemnobs.c:101-104`）→ `max_memory_to_use = 0`（`src/jmemmgr.c:1250`）→ 无限。唯一的库内覆盖途径是 `JPEGMEM` 环境变量（`src/jmemmgr.c:1270-1287`）。
- `jmemnobs.c` 不提供后备存储：超限时 `jpeg_open_backing_store()` 直接 `ERREXIT(JERR_NO_BACKING_STORE)`（`src/jmemnobs.c:87-92`，消息文本即 "Memory limit exceeded"，`src/jerror.h:114`）。也就是说**设限前全量进 RAM，设限后整体报错**，没有中间态。
- 应用层限制：djpeg `-maxmemory N`（`src/djpeg.c:362-373`）、TurboJPEG `TJPARAM_MAXMEMORY`（`src/turbojpeg.c:2344` 等四处赋值），**两者默认均为 0 = 无限**。TurboJPEG 另有 `TJPARAM_MAXPIXELS`（默认 0 = 无限，`src/turbojpeg.c:2353-2356`）可在读头后按像素数拦截。
- 注意：`max_memory_to_use` 只管 libjpeg 内存管理器内部的分配；应用自己的输出缓冲（如 tjbench 为 65500×65500 灰度图分配的 4.3 GB 目的缓冲）不在其内。

## 4. 实测

### 4.1 触发样本

| 样本 | 构造 | 大小 |
|---|---|---|
| `prog_base.jpg` | `cjpeg -grayscale -progressive`，64×64，6 个扫描 | 1.7 KB |
| `scans_106/1006/5006.jpg` | 复制 `prog_base.jpg` 最后一个扫描段 N 次 | 50 KB–2.4 MB |
| `ms_10/100/1000/10000.jpg` | 手工构造 1024×1024 渐进流：1 个 DC 扫描 + N 个 AC 扫描；每个 AC 扫描用 EOBRUN 覆盖全部 16384 个 MCU，**每个扫描仅 ~13 字节** | 2.3–132 KB |
| `ms_big.jpg` | 同上，4096×4096（262144 MCU/扫描）× 20000 扫描 | 715 KB |
| `dht_200/2000/20000.jpg` | 基线文件中 DHT 段重复 N 次 | 23 KB–2.1 MB |
| `bigdim_30k.jpg` / `bigdim_65k.jpg` | 把 `prog_base.jpg` 的 SOF 改为 30000×30000 / 65500×65500，熵数据不变 | **均 1739 字节** |

### 4.2 扫描次数 → CPU 时间（djpeg，libjpeg API 入口）

| 样本 | 扫描数 | 文件大小 | 耗时 | 峰值 RSS |
|---|---|---|---|---|
| `ms_10` | 11 | 2.3 KB | <0.01 s | 3.8 MB |
| `ms_100` | 101 | 3.5 KB | 0.01 s | 4.0 MB |
| `ms_1000` | 1001 | 15 KB | 0.07 s | 3.8 MB |
| `ms_10000` | 10001 | 132 KB | **0.79 s** | 4.0 MB |
| `ms_big` | 20001 | 715 KB | **23.3 s** | 34 MB |

耗时与 扫描数 × MCU 数 严格线性（`ms_big` 每扫描 MCU 数是 `ms_10000` 的 16 倍、扫描数 2 倍，耗时约 30 倍）。**~13 字节的输入即可买一个全图解码遍**；外推：10 MB 的构造文件（约 75 万个扫描）在 4096×4096 尺寸下需约 15 分钟单核 CPU。TurboJPEG 入口同样无默认限制：`tjbench ms_10000.jpg` 默认参数耗时 6.7 s（含多轮基准迭代），加 `-limitscans` 后 0.04 s 内在第 501 个扫描处中止。

### 4.3 DHT 重复 → 仅线性 CPU（djpeg）

| 样本 | DHT 段数 | 文件大小 | 耗时 | 峰值 RSS |
|---|---|---|---|---|
| `base` | 2 | 2.1 KB | <0.01 s | 1.9 MB |
| `dht_200` | 200 | 23 KB | <0.01 s | 1.9 MB |
| `dht_2000` | 2000 | 218 KB | <0.01 s | 1.8 MB |
| `dht_20000` | 20000 | 2.1 MB | <0.01 s | 1.9 MB |

内存恒定（表槽复用），CPU 与字节数成正比且常数极小。**此维度无需新增上限。**

### 4.4 虚拟数组 → 内存（djpeg / tjbench）

| 样本 | 文件大小 | 入口与参数 | 峰值 RSS | 结果 |
|---|---|---|---|---|
| `bigdim_30k` | 1739 B | djpeg 默认 | **1.76 GB** | 正常解码（熵数据不足处以灰填充） |
| `bigdim_65k` | 1739 B | djpeg 默认 | **8.38 GB** | 正常解码 |
| `bigdim_65k` | 1739 B | djpeg `-maxmemory 100m` | 1.9 MB | 立即报错 `Memory limit exceeded`，退出码 1 |
| `bigdim_65k` | 1739 B | tjbench 默认 | **20.9 GB** | 跑完全部基准迭代，耗时 3 分 41 秒 |
| `bigdim_65k` | 1739 B | tjbench `-maxmemory 100` | （tjbench 自身缓冲另计） | 报 `Memory limit exceeded` 中止 |

1739 字节输入 → 8.4 GB 常驻内存，放大约 **480 万倍**；65500×65500 灰度图的系数数组理论值为 8188×8188×128 ≈ 8.6 GB，与实测吻合。彩色 4:4:4 同尺寸将达 ~25.7 GB。

### 4.5 现有限制的生效位置（对照验证）

| 限制 | 位置 | 默认 | 实测效果 |
|---|---|---|---|
| djpeg `-maxscans N` | `src/cdjpeg.c:34-40`（进度回调） | 0 = 不限 | `-maxscans 500` 解码 `ms_10000`：0.04 s 打印 `Scan number 501 exceeds maximum scans (500)` 退出 |
| TurboJPEG `TJPARAM_SCANLIMIT` / `TJFLAG_LIMITSCANS` | `src/turbojpeg.c:169-189`（进度回调 longjmp） | 0 = 不限 | `tjbench -limitscans`：0.04 s 报 `Progressive JPEG image has more than 500 scans` |
| djpeg `-maxmemory N` | `src/djpeg.c:362-373` → `max_memory_to_use` | 0 = 不限 | 见 §4.4，立即报错 |
| TurboJPEG `TJPARAM_MAXMEMORY` | `src/turbojpeg.c:2344/2421/2677/3105` | 0 = 不限 | 见 §4.4 |
| TurboJPEG `TJPARAM_MAXPIXELS` | `src/turbojpeg.c:2353-2356` | 0 = 不限 | 读头后按像素数拦截 |
| `JPEGMEM` 环境变量 | `src/jmemmgr.c:1270-1287` | 未设置 | 库内唯一默认覆盖通道 |

**共同问题：所有限制都是"opt-in"，且都在库核心之外（进度回调或参数下传）。直接使用 libjpeg API 的第三方程序（不装进度回调、不设 `max_memory_to_use`）得不到任何保护。** 上游 fuzz 目标不得不自行设限（`fuzz/decompress.cc:95`、`fuzz/decompress_libjpeg.cc:222`）即是证据。

## 5. 缺少上限的解析阶段定位

1. **扫描计数（首要）**：`src/jdmarker.c:382` 的 `input_scan_number++` 与 `src/jdmarker.c:350-354` 的多 SOS 接受循环；非法重复 progression 在 `src/jdphuff.c:130-143` 只告警。建议在 `get_sos()` 或 `start_input_pass()`（`src/jdinput.c:296`）处对扫描总数做硬检查。
2. **虚拟数组内存预算（首要）**：`src/jmemnobs.c:101` 默认返回 0（无限预算）；`src/jmemmgr.c:754-763` 在预算充足时把数组按全高分配。建议给 `max_memory_to_use` 一个非零默认值。
3. **霍夫曼表**：解析阶段（`get_dht()`）已有长度与索引双重约束，内存有界、CPU 线性，**不缺上限**；唯一可选项是对单码流中 DHT 标记总数/总字节数设宽松上限以裁剪无效解析，但收益很小。

## 6. 增加上限后的兼容性分析

### 6.1 扫描次数上限

- **合法图像的扫描数上界**：按 T.81 的 progression 规则，每个 (分量, 系数位置) 最多 1 个首扫描 + 13 个精化扫描（Ah 1..13），理论最大值 ≈ 分量数 × 64 × 14（4 分量时约 3584）。实际编码器远低于此：libjpeg 默认渐进脚本约 10 个扫描，常见相机/软件文件 ≤ 数十个。
- **建议**：库核心内置默认上限取 **500**（与 `TJFLAG_LIMITSCANS` 既有取值一致），可覆盖全部真实图像；同时保留 `TJPARAM_SCANLIMIT = 0` / API 字段置 0 表示"不限"的逃生门，使极少数 >500 扫描的合法文件（如自定义 `-scans` 脚本生成的极限渐进文件）仍可通过显式配置解码。这样默认安全、兼容可恢复。
- 实施点选择 `get_sos()`（解析期即拦截）优于进度回调（解码期才拦截），可在第一个超限扫描的熵数据被解码前就报错，且对不装进度回调的 libjpeg API 用户同样生效。ABI 上可在 `jpeg_decompress_struct` 的扩展区或借助现有 `master` 指针域存放上限值，避免破坏公共结构布局。

### 6.2 虚拟数组内存上限

- **合法图像的内存需求**：系数数组 ≈ 像素数 / 64 × 128 字节 × (1 + 色度抽样比)。一张 5000 万像素 4:2:0 照片约需 1.5×(50e6/64)×128 ≈ 150 MB；1 GB 的默认预算可覆盖约 3.3 亿像素的 4:2:0 图像，远超常见照片。
- **建议**：将 `jpeg_mem_init()` 的默认预算从 0 改为有限值（如 1–2 GB），或让 TurboJPEG 的 `maxMemory`/`maxPixels` 默认非零。超限时行为已是确定性的 `JERR_NO_BACKING_STORE`（"Memory limit exceeded"）错误返回，不会截断输出或产生半解码图像。
- **兼容性**：受影响的是"系数数组超过预算"的图像——这类图像在当前默认下本来就会真实占用等量内存，在内存不足的机器上即触发 OOM；设限只是把不可控的 OOM 变成可控的解码错误。需要解码超大图的应用可继续用 `TJPARAM_MAXMEMORY`、`max_memory_to_use` 或 `JPEGMEM` 显式提高预算，解码结果与今天完全一致。
- 注意上限只约束 libjpeg 内部分配；应用侧输出缓冲（宽×高×像素字节数）仍需应用自行按 `TJPARAM_MAXPIXELS` 之类机制把关。

### 6.3 霍夫曼表

- 不建议新增硬限制。DHT 重定义是合法且被真实编码器使用的行为；现有 4+4 槽位与 64 KB 段长已把内存与单次解析开销封死。若仍想裁剪模糊场景下的无效解析，可对"每码流 DHT 标记总数"设非常宽松的上限（如 10000），对合法文件零影响（正常文件 DHT 段数为个位数）。

## 7. 复现说明

```sh
# 构建（本报告环境）
cmake -G"Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DWITH_SIMD=0 -DENABLE_SHARED=0 ..
make -j12 cjpeg-static djpeg-static tjbench-static

# 生成样本（脚本与样本分目录）
python3 -I analysis-scripts/gen_samples.py analysis-samples build-analysis
python3 -I analysis-scripts/gen_manyscans.py analysis-samples/ms_10000.jpg 1024 1024 10000
python3 -I analysis-scripts/gen_manyscans.py analysis-samples/ms_big.jpg 4096 4096 20000

# 观测
/usr/bin/time -v ./djpeg-static -pnm -outfile /dev/null analysis-samples/ms_big.jpg
/usr/bin/time -v ./djpeg-static -pnm -outfile /dev/null analysis-samples/bigdim_65k.jpg
./djpeg-static -maxscans 500 -pnm -outfile /dev/null analysis-samples/ms_10000.jpg
./djpeg-static -maxmemory 100m -pnm -outfile /dev/null analysis-samples/bigdim_65k.jpg
./tjbench-static analysis-samples/ms_10000.jpg -limitscans
```
