# U-Boot for Lenovo TB710FU (小新 Pad Pro GT) — SM8650Q

**Custom U-Boot that boots mainline Linux on the Lenovo Xiaoxin Pad Pro GT (TB710FU).**
**在联想小新 Pad Pro GT(TB710FU)上引导主线 Linux 的定制 U-Boot。**

- 中文说明:[#中文说明](#中文说明)
- English: [#english](#english)

---

<!-- 中文部分 -->

## 中文说明

### 这是什么

这是针对 **联想小新 Pad Pro GT(TB710FU,国行 TB710FU_PRC)** 的定制 U-Boot,基于
[DanDrewCJ/u-boot-oneplus-13r](https://github.com/DanDrewCJ/u-boot-oneplus-13r)(一加 13R / SM8650 的 U-Boot 移植)深度改造。

硬件背景:高通 SM8650Q(骁龙 8 Gen 3,pineapple),11" 2000×3200 NT36532 双 DSI 面板,
8GB LPDDR5X,256GB UFS。整机**没有引出 UART**,所以本 U-Boot 把全部启动日志用大字号
直接画在面板 framebuffer(0xD5100000,3200×2000)上。

它**不是一个带交互 shell 的通用引导器**,而是一个单一用途的自动引导器(memboot):
刷在 Android A/B 槽的 boot 分区里,由原厂 ABL 当作"内核"加载运行;随后从 GPT 分区
`linboot` 读取主线内核 + initramfs + 设备树,修补后直接跳转进主线 Linux(当前 7.2.x)。

### 启动链

```
XBL(原厂) → ABL(原厂,加载 boot 槽镜像)
    → 本 U-Boot(约 1.4MB,gzip 后 ~600KB)
        → 从 linboot 分区读内核/initramfs/DTB → 修补 DTB → 跳转
    → 主线内核 + initramfs → 真 ext4 根文件系统(linsys 分区)
```

### memboot 从 `linboot` 分区读取的原始布局

`linboot` 分区按**裸块**读取(不识别文件系统),布局如下(4KB 块):

| 偏移 | 内容 | 读窗口 |
|---|---|---|
| `0` | 内核 `Image.gz`(gzip 压缩的主线内核) | 0x1000 块 = 16 MiB → 载入 0x90000000 |
| `16 MiB` | initramfs(`cpio.gz`) | 0x800 块 = 2 MiB → 载入 0xAD000000 |
| `24 MiB` | 设备树 DTB(补零至 0x28000) | 0x28 块 = 160 KiB |

- 内核在 0x90000000 处 gunzip 解压到 **0xA8000000**(最大 64 MiB),解压后对前 1 MiB 做
  FNV-1a-32 "oracle" 校验(防 UFS 读坏数据导致玄学崩溃)。
- 内核入口地址 0xA8000000,**直接以 EL1 跳转** `kentry(dtb, 0, 0, 0)` ——
  本机 `armv8_switch_to_el2()` 不可用(U-Boot 自身运行在 EL1,该 helper 检测到非 EL2
  会直接 return 不跳转)。
- U-Boot 会修补 DTB:写 `/memory` 内存银行、`/chosen`(initrd 起止 + bootargs
  `console=tty0 ... console=tbfb`)、删除 `stdout-path`、**禁用所有 geni UART 但豁免带
  `bluetooth` 子节点的 uart14**(WCN7850 蓝牙走这路,一刀切禁用会弄死蓝牙)、加入
  reserved-memory `framebuffer@d5100000`。刻意**不加** `simple-framebuffer` 节点
  (它在真机上会以乱码地址崩溃)。
- board_r 只跑 `init_sequence_r[0..14]`,其余驱动手动逐个拉起 —— 完整的 dm_autoprobe
  在这块板子上会挂死。

## ⚠️ 已知问题:启动时间很长

**现象**:冷启动后 U-Boot 阶段屏幕会停留大段大字日志(依次 DM INIT → KERNEL READ →
INITRD MAGIC → GUNZIP → DTB → GO/JUMP),整体从上电到桌面(Plasma)约 **2–2.5 分钟**。

**已定位的真实耗时点**(按贡献排序,均有实测/拍照证据):

1. **gunzip 全程无 MMU、无缓存** —— 这是大头。内核 15.4MB → 43MB 的解压 + 27MB BSS
   清零全部在关 MMU/D-cache 的状态下逐字节强序访问内存完成(实拍 SCTLR =
   `0x30d00988`:M=0、C=0、I=0)。U-Boot 自带 `dcache_enable()` 在本机不可用:它的
   `mmu_setup()` 按 DT 内存银行建页表,会挂死(见下文缓存窗口实验)。
2. **为"拍照可读"设计的屏显日志**:每次开机约 50 行日志,每行 = 清 1600×56px 区域 +
   3 倍字号绘制(源码注释自证:*"drawn at 3x so a phone camera can read it"*),
   数十 MB 的强序 framebuffer 写入。
3. **1 MiB FNV oracle 哈希**:gunzip 后对内核逐字节哈希校验。
4. ~~DTB 读取冗余~~(旧血统曾从分区读 16 MiB 只为拿 136KB 的 DTB,120 倍冗余 I/O;
   **当前源码已修复**:只读 160 KiB 窗口)。

**尝试过但失败的加速方案**(已全部回滚,**不在本源码树中**,仅作记录避免重复踩坑):

- **quiet boot**(`tb_quiet_ub=1`,静音全部屏显日志,失败遥测仍强制显示):
  补丁本身逻辑没问题,但一版含此补丁的构建被误刷上板 → "连字都没有"的黑屏死机
  (该层代码从未被单独证明可启动),整体回滚。
- **fastgunzip / 手工缓存窗口**(`tb_cache_window()`:两张 4KB 页表,GB2
  0x80000000-0xBFFFFFFF 映射 Normal WB,其余 Device):**v1 直接冻死** —— 根因是
  `qcom_parse_memory()` 把 8GB 多 bank 内存算成一个跨 4GB 的 ram_size,U-Boot 被重定位
  到 4GB 以上,而 v1 映射只覆盖 GB2 → 开 MMU 后正在执行的代码与栈落在 Device 映射上,
  取指即冻结(连 ABORT 向量表都在 Device 区,救不回来)。**v2**(运行时按
  `gd->relocaddr`/`start_addr_sp` 判定归入 Normal)上板仍失败,原因未完全查清。
  **核心教训:在这台机器上做任何 MMU 映射,必须覆盖重定位后代码与栈所在的 GB。**
- 更稳妥的候选方案(未实施):把**未压缩 Image**放进 linboot,完全跳过 gunzip
  (需要重排分区布局);或按 SMEM 精确内存 bank 建 2MB 粒度映射。

### 构建

主机依赖同上游 U-Boot(bison、flex、gcc、python3、dtc 由源码树自带等),交叉编译器
`aarch64-linux-gnu-gcc`(实测 15.2.0)。

```sh
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- sm8650-lenovo-tb710fu_defconfig
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
```

- **注意是 `ARCH=arm` 而不是 arm64** —— U-Boot 把 arm64 放在 arch/arm 下;
  裸 `make` 会用 x86 gcc 直接编废。
- 产物 `u-boot.bin` 约 **1,451,336 字节**(与板上验证版本大小一致,可作健全性检查)。
- `sm8650-lenovo-tb710fu_defconfig` 已校准为可**逐字节复现**板上验证过的 `.config`
  (`CONFIG_OF_LIVE` 关闭、`CONFIG_OFNODE_MULTI_TREE=y` —— 这两个若与验证配置不符,
  二进制会大 16KB 且 DM 行为不同)。

### 打包与刷写

U-Boot 需要包成 ABL 能接受的 Android boot 镜像:4 KiB `ANDROID!` 头(从任意一份
已知良好的 boot 镜像取前 4 KiB 作模板,`kernel_size` 字段填 gzip 载荷长度)+
`u-boot.bin` 的 gzip。`tb710fu-tools/pack_uboot_image.py` 即做此事:

```sh
# 脚本内部完成 gzip(mtime=0)并写入 ANDROID! 头;模板可以是整份已知良好 boot 镜像,脚本只取前 4 KiB
python3 tb710fu-tools/pack_uboot_image.py <模板boot镜像> u-boot.bin boot_b-out.img
fastboot flash boot_b boot_b-out.img
fastboot set_active a && fastboot set_active b && fastboot erase misc
```

**必须冷启动验证**:长按电源约 15 秒彻底断电 → 松手后再开机。
`fastboot reboot` 热重启在本机会失败(UFS/USB 控制器状态残留)。

逃生门:memboot 一开始就会往 misc 写 `bootonce-bootloader` BCB → 若它自己挂了,
下次开机 ABL 直接进 fastboot;也可长按电源+音量减手动进。刷 boot_b 属于改引导链,
**请务必保留一份能回滚的已知良好 boot 镜像**。

### 免责声明

刷写引导链有变砖风险,由此造成的一切后果自负。本项目与联想、高通无任何关联。
仅供个人研究学习。

### 致谢与许可

- [DanDrewCJ/u-boot-oneplus-13r](https://github.com/DanDrewCJ/u-boot-oneplus-13r) —— SM8650 U-Boot 底子
- 上游 U-Boot / denx.de、Qualcomm 主线支持的所有贡献者
- [GUF296](https://github.com/GUF296) —— 同平台 Y700(TB321FU)主线移植全链路的先行者
- ACLaniakea、lolipuru —— TB710FU 家族内核/设备树资料

许可证:**GPL-2.0-only**(继承上游 U-Boot,详见 `Licenses/`)。

---

<!-- English section -->

## English

### What is this

A customized U-Boot for the **Lenovo Xiaoxin Pad Pro GT (TB710FU, PRC variant
TB710FU_PRC)**, heavily modified from
[DanDrewCJ/u-boot-oneplus-13r](https://github.com/DanDrewCJ/u-boot-oneplus-13r)
(U-Boot for the OnePlus 13R / SM8650).

Hardware: Qualcomm SM8650Q (Snapdragon 8 Gen 3, "pineapple"), 11" 2000×3200
NT36532 dual-DSI panel, 8 GB LPDDR5X, 256 GB UFS. The tablet has **no UART
exposed**, so this U-Boot draws all boot logs as large text directly on the
panel framebuffer (0xD5100000, 3200×2000).

This is **not an interactive bootloader** — it is a single-purpose auto-booter
("memboot"): flashed into an Android A/B-slot boot partition, loaded and run by
the stock ABL as if it were a kernel. It then reads the mainline kernel,
initramfs and device tree from a GPT partition named `linboot`, patches the
tree, and jumps into mainline Linux (currently 7.2.x).

### Boot chain

```
XBL (stock) → ABL (stock, loads the boot-slot image)
    → this U-Boot (~1.4 MB, ~600 KB gzipped)
        → read kernel/initramfs/DTB from the linboot partition → patch DTB → jump
    → mainline kernel + initramfs → real ext4 rootfs (linsys partition)
```

### Raw `linboot` partition layout

`linboot` is read as **raw blocks** (no filesystem), using 4 KB blocks:

| Offset | Content | Read window |
|---|---|---|
| `0` | Kernel `Image.gz` (gzipped mainline kernel) | 0x1000 blocks = 16 MiB → loaded at 0x90000000 |
| `16 MiB` | initramfs (`cpio.gz`) | 0x800 blocks = 2 MiB → loaded at 0xAD000000 |
| `24 MiB` | Device tree DTB (zero-padded to 0x28000) | 0x28 blocks = 160 KiB |

- The kernel is gunzipped from 0x90000000 into **0xA8000000** (64 MiB max),
  then a FNV-1a-32 "oracle" hash over its first 1 MiB is verified (guards
  against silent UFS data corruption).
- Kernel entry is 0xA8000000, entered **directly at EL1** via
  `kentry(dtb, 0, 0, 0)` — `armv8_switch_to_el2()` is unusable here (U-Boot
  itself runs at EL1; the helper detects non-EL2 and just returns).
- DTB patches applied by U-Boot: `/memory` RAM banks, `/chosen` (initrd ranges
  and bootargs `console=tty0 ... console=tbfb`), `stdout-path` removed, **all
  geni UARTs disabled except nodes carrying a `bluetooth` child** (uart14 is
  the WCN7850 Bluetooth UART — blanket-disabling kills BT), and a
  reserved-memory `framebuffer@d5100000` node. A `simple-framebuffer` node is
  deliberately **not** added (it crashes on real hardware with a garbage
  address).
- board_r only runs `init_sequence_r[0..14]`; remaining drivers are brought up
  manually — a full `dm_autoprobe` hangs this board.

## ⚠️ Known issue: boot takes a long time

**Symptom**: after a cold boot, the U-Boot stage keeps printing large on-screen
log lines (DM INIT → KERNEL READ → INITRD MAGIC → GUNZIP → DTB → GO/JUMP …);
power-on to Plasma desktop totals roughly **2–2.5 minutes**.

**Measured contributors** (ordered by impact, all evidence-backed):

1. **gunzip runs with the MMU and caches completely off** — the dominant cost.
   Decompressing 15.4 MB → 43 MB plus clearing 27 MB of BSS happens with
   MMU/D-cache disabled (photo of SCTLR = `0x30d00988`: M=0, C=0, I=0), i.e.
   every access is an uncached, strongly-ordered DRAM transaction. U-Boot's own
   `dcache_enable()` cannot be used on this board: its `mmu_setup()` builds
   page tables from DT memory banks and hangs (see the cache-window experiment
   below).
2. **Photo-readable on-screen logging**: ~50 log lines per boot, each one
   clearing a 1600×56 px region and drawing text at 3× scale (the source
   comments admit: *"drawn at 3x so a phone camera can read it"*) — tens of MB
   of strongly-ordered framebuffer writes.
3. **1 MiB FNV oracle hash** over the gunzipped kernel, byte by byte.
4. ~~Redundant DTB read~~ — older builds read 16 MiB from flash to obtain a
   136 KB DTB (120× redundant I/O). **Fixed in the current tree**: only the
   160 KiB window is read.

**Acceleration attempts that failed** (all reverted, **not in this tree**,
documented so nobody re-steps on them):

- **quiet boot** (`tb_quiet_ub=1`, silences all screen logging while keeping
  failure telemetry forced-visible): the patch itself was sound, but a build
  containing it was flashed by accident and produced a black screen with *no
  text at all* — that code path had never been independently proven to boot.
  Fully reverted.
- **fastgunzip / manual cache window** (`tb_cache_window()`: two 4 KB page
  tables, GB2 0x80000000-0xBFFFFFFF mapped Normal WB, everything else Device):
  **v1 froze instantly** — `qcom_parse_memory()` turns the 8 GB multi-bank
  memory into one ram_size spanning past 4 GB, so U-Boot relocates itself above
  4 GB while the v1 mapping only covered GB2; enabling the MMU put the running
  code and stack on Device mappings and instruction fetch froze (even the ABORT
  vector table lived in a Device region). **v2** (runtime GB detection from
  `gd->relocaddr` / `start_addr_sp`) still failed on hardware for reasons not
  fully root-caused. **Key lesson: any MMU mapping on this machine must cover
  the GB containing the relocated code and stack.**
- Safer candidates (not implemented): store an **uncompressed Image** in
  linboot to skip gunzip entirely (requires repartitioning), or build 2 MB
  block mappings from the exact SMEM memory banks.

### Building

Host dependencies are the usual upstream U-Boot ones (bison, flex, gcc,
python3, …); cross compiler `aarch64-linux-gnu-gcc` (tested with 15.2.0).

```sh
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- sm8650-lenovo-tb710fu_defconfig
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
```

- **Use `ARCH=arm`, not arm64** — U-Boot keeps arm64 under arch/arm; a bare
  `make` will happily build garbage with the x86 host gcc.
- The result is `u-boot.bin`, ~**1,451,336 bytes** (same size as the
  board-verified build — handy sanity check).
- `sm8650-lenovo-tb710fu_defconfig` is calibrated to reproduce the
  board-verified `.config` byte-for-byte (`CONFIG_OF_LIVE` off,
  `CONFIG_OFNODE_MULTI_TREE=y` — with any other combination the binary grows by
  16 KB and the DM behaves differently from the tested build).

### Packing and flashing

U-Boot must be wrapped in an Android boot image ABL accepts: a 4 KiB
`ANDROID!` header (take the first 4 KiB of any known-good boot image as the
template; `kernel_size` = gzip payload length) followed by gzipped
`u-boot.bin`. `tb710fu-tools/pack_uboot_image.py` does exactly this:

```sh
# The script gzips u-boot.bin itself (mtime=0) and writes the ANDROID! header;
# the template can be a full known-good boot image (only its first 4 KiB are used).
python3 tb710fu-tools/pack_uboot_image.py <template-boot-image> u-boot.bin boot_b-out.img
fastboot flash boot_b boot_b-out.img
fastboot set_active a && fastboot set_active b && fastboot erase misc
```

**Cold-boot verification is mandatory**: hold the power button ~15 s for a full
power-off, release, then power on. A hot `fastboot reboot` fails on this device
(residual UFS/USB controller state).

Escape hatch: memboot writes the `bootonce-bootloader` BCB into `misc` before
doing anything risky — if it hangs, the next boot drops straight into fastboot
(power + volume-down also works). Flashing boot_b changes the boot chain, so
**always keep a known-good boot image for rollback**.

### Disclaimer

Flashing bootloader payloads can brick your device. Everything here is provided
as-is, use at your own risk. Not affiliated with Lenovo or Qualcomm. For
research and personal use.

### Credits & license

- [DanDrewCJ/u-boot-oneplus-13r](https://github.com/DanDrewCJ/u-boot-oneplus-13r) — the SM8650 U-Boot base
- Upstream U-Boot (denx.de) and every Qualcomm mainline contributor
- [GUF296](https://github.com/GUF296) — full mainline port of the same-SoC Y700 (TB321FU)
- ACLaniakea, lolipuru — TB710FU-family kernel/DT resources

License: **GPL-2.0-only** (inherited from upstream U-Boot, see `Licenses/`).
