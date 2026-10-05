# 屏幕诊断判读 — 色块与日志 / On-screen diagnostics — color bars & log reference

> 本机没有 UART。U-Boot 阶段唯一的输出通道是 ABL 交接时留下的扫描帧缓冲
> (0xD5100000,3200×2000)。这份文档说明屏幕上每个元素的含义,
> 以及"停在某一屏"时如何定位。判读方法:**拍照**。
>
> There is no UART on this board. During the U-Boot stage the only output
> channel is the scanout framebuffer ABL hands over (0xD5100000, 3200×2000).
> This document explains every element on screen and how to localize a hang.
> Reading method: **take a photo**.

---

## 中文

### 基本机制

- **文本日志**:U-Boot 自绘大字(8×16 字体,3× 缩放),每行带**调用序号前缀**
  (即使照片只拍到半行,也能知道最后执行到哪一行),一屏约 28 行。
  `tb_logv(标签, 值)` 打印 `标签 + 8 位十六进制`;`tb_logv2` 一行两个值。
- **色条**:`tb_mark(idx)` 在自己的专属行画一条 2700×36 px 实心色条
  (行 = 1200 + idx×44,列 400..3100,避开左侧十六进制值)。
  **每条色条 = 一个底层里程碑已到达**;色条按启动顺序自上而下累积。
- 屏幕上**最后一行文本 / 最高的一条色条** = 到达的最后位置;
  该行的**下一步动作**就是挂死嫌疑点。
- 文本区在屏上部;色条区(行 1200..1989)在 memboot 开始时被 `tb_screen_clear()`
  整体擦除后重画 —— 所以最终照片里主要是 R 阶段(memboot)的色条。
- 内核接管后 U-Boot 画面被覆盖:**GO 之后的黑屏/花屏属于内核侧,不是 U-Boot**。

### 正常启动时间线(文本日志,按出现顺序)

| 行 | 含义 | 正常值 |
|---|---|---|
| `FDT PTR NOW` / `FDT PTR SAVED` | ABL 传入的 FDT 指针捕获/保存(F 阶段) | 同一指针 |
| `R ENTRIES BEFORE DM DEV` + `R SEQ OK` | R 侧 initcall 序列长度与结果 | FAIL 才是问题 |
| `DM INIT AND SCAN` → `DM INIT` / `DM SCAN FDT` / `DM SCAN OTHER` / `TIMER` / `UFS` / `SCSI` / `SCSI CNT` | 手动逐个拉起驱动(完整 dm_autoprobe 在本机挂死) | SCSI CNT ≥ 1 |
| `BCB SET` | 已向 misc 分区写 `bootonce-bootloader`(逃生门激活:挂死则下次开机 ABL 直接进 fastboot) | 1 |
| (清屏,洋红色条 mark 5) | memboot 正式开始 | — |
| `UB EL` | U-Boot 运行的异常级 | **1**(EL2 切换不可用,见 README) |
| `UB SCTLR` | SCTLR 寄存器 | `30d00988`(MMU/D-cache/I-cache 全关) |
| `PREV BA` / `PREV VOFF` / `PREV BSS` / `C1` / `C2` / `C3` / `PREV HLPS` | ABL→U-Boot 交接状态与探针区读回(诊断记录) | 无单值期望 |
| `RAM TOT` | 内存总量(从 ABL FDT 或平台 bank 汇总) | 8GiB 机器 = `0x200000000` |
| `KERNEL READ` | 从 linboot@0 读内核到 0x90000000 | **0x1000**(4096 块 = 16MiB) |
| `INITRD READ` | 从 linboot+16MiB 读 initramfs 到 0xAD000000 | **0x800**(2048 块 = 2MiB) |
| `INITRD MAGIC` | initramfs 首字 | gzip 魔数(`1f 8b`)必须在 |
| `GUNZIP` | 0x90000000 → 0xA8000000 解压 | **0**(成功)。**此步无缓存,15.4→43MB 需数秒,是正常慢,不是挂死** |
| `SRC USED` | 实际消费的压缩长度 | ≈ 内核 gz 实际大小 |
| `K8` | 解压产物首 8 字节 = ARM64 内核头(`0xfa405a4d` efi_signature_nop + 跳 primary_entry 的分支) | 头两个 u32 |
| `ORCL` | 前 1MiB FNV-1a-32 哈希(got vs 期望) | 两值相等;**不等 = UFS 读数据损坏** |
| `MEM SET` | DTB `/memory` 节点写入结果 | 0 |
| `INITRD LEN` | initramfs 长度(写入 /chosen) | 与文件一致 |
| `NO FB NODE` | 刻意**不**添加 simple-framebuffer 节点(真机会以乱码地址崩溃) | 恒出现,正常 |
| (此后) | 禁用所有 geni UART,但**豁免带 `bluetooth` 子节点的 uart14**(WCN7850 蓝牙) | — |
| `IMAGE SIZE` | 内核头自报镜像大小 | — |
| `BSS ZEROED` | 内核 BSS(镜像尾 → `_end`)清零字节数 | >0 |
| `RT1` / `RT2` / `RT3` | DRAM 探针区 64 位回读 | 读得回即健康 |
| (黄条 mark 1) | memboot 全程完成 | — |
| `JUMP` → `ENTRY` / `DTB` → `GO` | 关闭外来的 MMU 状态后 `kentry(dtb,0,0,0)` 直跳 0xA8000000 | ENTRY = `A8000000` |
| `BOOT ABORTED` + flags | **失败兜底**:上面任一步 rc≠零会走到这 | 出现即失败 |

### 色条系统(tb_mark)

颜色表(ARGB)与调用点。**同一序号固定画在同一行**,照片里不会混淆:

| idx | 颜色 | 调用点 | 含义 |
|---|---|---|---|
| 0 | 绿 | `crt0_64.S`;`board_r.c` | board_init_f 返回 crt0;GHWDT SMC 查询**成功**(hypervisor watchdog 服务存在) |
| 1 | 黄 | `board_r.c` | memboot 全程完成,即将跳内核 |
| 2 | 蓝 | `relocate_64.S` | relocate_code 进入,栈帧存活 |
| 3 | 白 | `relocate_64.S` | 镜像已拷贝到重定位地址 |
| 4 | 青 | `relocate_64.S` | 重定位 fixup 完成 |
| 5 | 洋红 | `crt0_64.S`;`board_r.c` | 重定位完成,已运行重定位后代码;memboot 开始(清屏后重画) |
| 6 | 橙 | `relocate_64.S` | EL 切换完成,SCTLR 已读 |
| 7 | 红 | `board_r.c` | GHWDT SMC 查询**失败**(SMCCC 接口不存在/拒绝) |
| 8 | 鸭青 | `crt0_64.S` | c_runtime_cpu_setup 返回 |
| 11 | 浅蓝 | `relocate_64.S` | 即将对副本清刷 D-cache |
| 12 | 灰 | `relocate_64.S` | D-cache 清刷返回 |
| 13 | 橄榄 | `relocate_64.S` | L3 清刷返回,即将 ret |

**红色进度条(行 1150)**:relocate fixup 循环每 16 次迭代画 1 像素 ——
条**停止增长** = 挂死点就在 fixup 循环里,且能看到大约停在第几个 16 次迭代。

### 失败屏判读

| 屏上出现 | 含义 |
|---|---|
| `ABORT PC` / `ABORT FAR` / `ABORT ESR` / `ABORT X2 TABLE` / `ABORT X0 ENTRY` / `ABORT X1 ENTRY` | 异常处理器强制显示(静音设置也压不住)。PC 与 `u-boot-tb710fu-System.map` 对照:记录地址 − 链接地址 = 重定位偏移,再回到该偏移查真实函数 |
| `BOOT ABORTED` + `gd->flags` 十六进制 | memboot 流程 rc 非零走到兜底 —— 往上找最后一个非零行 |
| `R SEQ FAIL` + 值 | R 侧 initcall 序列返回错误 |
| `ORCL` 两值不等 | gunzip 后数据哈希对不上离线值 ⇒ **UFS 读出坏数据**(重刷 linboot 镜像、查分区) |
| `RAMOOPS DIAG` + 大段文本 + `PET WDT` | **这是上一次崩溃的 printk 环**(热复位不清 DRAM 留下的残迹),显示后永久喂狗停在原地 —— 此路径**只拍照不引导**。看到它 = 上次内核/U-Boot 崩了,而不是这次 U-Boot 卡住 |

### 定位速查(卡住的行 → 下一步动作 → 嫌疑)

| 最后可见 | 下一步动作 | 嫌疑 |
|---|---|---|
| `DM INIT`/`UFS`/`SCSI` 行 | UFS/SCSI 控制器拉起 | UFS 初始化、链路 |
| `KERNEL READ` 后无 `INITRD READ` | 读 initramfs | linboot 分区名/布局错、分区表异常 |
| `INITRD MAGIC` 值怪 | 校验 gzip 魔数 | initramfs 偏移错或数据坏 |
| 停在 `GUNZIP` 之前几秒 | 无缓存解压 15.4→43MB | **正常耗时**(数秒),等 |
| `GUNZIP` 长时间不出 | 解压挂死 | 解压目标区/内存映射 |
| `ORCL` 不等 | 哈希校验 | UFS 数据坏 |
| `MEM SET` 非 0 | DTB 扩容/写节点 | 树过大(fdt_open_into 1MiB 上限) |
| 黄条后有 `JUMP` 无 `GO` | 收尾与跳转 | 跳转路径 |
| `GO` 之后黑屏/异常 | 内核已接管 | **内核侧**,换内核/DTB 调 |

---

## English

### How it works

- **Text log**: self-drawn large text (8×16 font at 3× scale), every line
  prefixed with a **call number** (a half-legible photo still shows which line
  ran last), ~28 lines per screen. `tb_logv(label, v)` prints
  `label + 8 hex digits`; `tb_logv2` prints two values per line.
- **Color bars**: `tb_mark(idx)` paints one solid 2700×36 px bar on its own
  dedicated row (row = 1200 + idx×44, columns 400..3100, clear of the hex
  values at the left edge). **One bar = one low-level milestone reached**;
  bars accumulate top-to-bottom in boot order.
- The **last text line / highest bar** on screen = the last position reached;
  the **next action after that line** is the suspect.
- The text zone sits in the upper part of the screen; the bar zone
  (rows 1200..1989) is wiped and repainted when memboot starts, so a final
  photo mostly shows the R-phase (memboot) bars.
- Once the kernel takes over it overwrites the U-Boot screen: **a black or
  corrupted display after GO is kernel-side, not U-Boot**.

### Normal boot timeline (text log, in order)

| Line | Meaning | Expected |
|---|---|---|
| `FDT PTR NOW` / `FDT PTR SAVED` | ABL-passed FDT pointer captured/saved (F phase) | same pointer |
| `R ENTRIES BEFORE DM DEV` + `R SEQ OK` | R-side initcall sequence count and result | FAIL is the problem |
| `DM INIT AND SCAN` → `DM INIT` / `DM SCAN FDT` / `DM SCAN OTHER` / `TIMER` / `UFS` / `SCSI` / `SCSI CNT` | drivers brought up manually (full dm_autoprobe hangs this board) | SCSI CNT ≥ 1 |
| `BCB SET` | `bootonce-bootloader` written to misc (escape hatch: on a hang the next ABL boot goes straight to fastboot) | 1 |
| (screen clear, magenta mark 5) | memboot begins | — |
| `UB EL` | exception level U-Boot runs at | **1** (EL2 switch unusable, see README) |
| `UB SCTLR` | SCTLR | `30d00988` (MMU / D-cache / I-cache all off) |
| `PREV BA` / `PREV VOFF` / `PREV BSS` / `C1` / `C2` / `C3` / `PREV HLPS` | ABL→U-Boot handoff state and probe-area readbacks (diagnostic records) | no single expected value |
| `RAM TOT` | total RAM (from ABL FDT or platform banks) | 8 GiB machine = `0x200000000` |
| `KERNEL READ` | kernel read from linboot@0 to 0x90000000 | **0x1000** (4096 blocks = 16 MiB) |
| `INITRD READ` | initramfs read from linboot+16 MiB to 0xAD000000 | **0x800** (2048 blocks = 2 MiB) |
| `INITRD MAGIC` | first word of the initramfs | gzip magic (`1f 8b`) must be there |
| `GUNZIP` | 0x90000000 → 0xA8000000 decompression | **0** (ok). **Uncached, 15.4→43 MB takes seconds — normal slowness, not a hang** |
| `SRC USED` | compressed length consumed | ≈ the kernel gz size |
| `K8` | first 8 bytes of the decompressed image = ARM64 kernel header (`0xfa405a4d` efi_signature_nop + branch to primary_entry) | first two u32s |
| `ORCL` | FNV-1a-32 over the first 1 MiB (got vs expected) | values equal; **a mismatch = corrupted UFS read** |
| `MEM SET` | result of writing the DTB `/memory` node | 0 |
| `INITRD LEN` | initramfs length (written to /chosen) | matches the file |
| `NO FB NODE` | a simple-framebuffer node is deliberately **not** added (crashes on real hardware) | always present, normal |
| (after this) | all geni UARTs disabled **except** nodes carrying a `bluetooth` child (uart14 = WCN7850 BT) | — |
| `IMAGE SIZE` | image size reported by the kernel header | — |
| `BSS ZEROED` | kernel BSS bytes zeroed (image end → `_end`) | >0 |
| `RT1` / `RT2` / `RT3` | 64-bit readbacks of the DRAM probe area | reads returning = healthy |
| (yellow mark 1) | memboot ran to completion | — |
| `JUMP` → `ENTRY` / `DTB` → `GO` | foreign MMU state dropped, `kentry(dtb,0,0,0)` jumps to 0xA8000000 | ENTRY = `A8000000` |
| `BOOT ABORTED` + flags | **failure fallthrough**: any nonzero rc above lands here | appearing at all = failure |

### Color bar system (tb_mark)

Color table (ARGB) and call sites. **An index always paints the same row**,
so bars never get confused in a photo:

| idx | Color | Call site | Meaning |
|---|---|---|---|
| 0 | green | `crt0_64.S`; `board_r.c` | board_init_f returned to crt0; GHWDT SMCCC query **succeeded** (hypervisor watchdog service exists) |
| 1 | yellow | `board_r.c` | memboot ran to completion, about to jump into the kernel |
| 2 | blue | `relocate_64.S` | relocate_code entered, stack frame live |
| 3 | white | `relocate_64.S` | image copied to the relocation address |
| 4 | cyan | `relocate_64.S` | relocation fixups done |
| 5 | magenta | `crt0_64.S`; `board_r.c` | relocation finished, running relocated; memboot start (repainted after the screen clear) |
| 6 | orange | `relocate_64.S` | EL switch done, SCTLR read |
| 7 | red | `board_r.c` | GHWDT SMCCC query **failed** (SMCCC interface absent/refused) |
| 8 | teal | `crt0_64.S` | c_runtime_cpu_setup returned |
| 11 | light blue | `relocate_64.S` | about to flush D-cache over the copy |
| 12 | gray | `relocate_64.S` | D-cache flush returned |
| 13 | olive | `relocate_64.S` | L3 flush returned, about to return |

**Red progress bar (row 1150)**: the relocate fixup loop paints one pixel per
16 iterations — a bar that **stops growing** marks the stall point inside the
fixup loop, and roughly which 16-iteration group it stopped in.

### Failure screens

| On screen | Meaning |
|---|---|
| `ABORT PC` / `ABORT FAR` / `ABORT ESR` / `ABORT X2 TABLE` / `ABORT X0 ENTRY` / `ABORT X1 ENTRY` | exception handler output, forced visible (silencing cannot suppress it). Match PC against `u-boot-tb710fu-System.map`: recorded address − link address = relocation offset |
| `BOOT ABORTED` + `gd->flags` hex | memboot fell through with nonzero rc — look up for the last nonzero line |
| `R SEQ FAIL` + value | R-side initcall sequence returned an error |
| `ORCL` values differ | post-gunzip hash mismatches the offline value ⇒ **corrupted UFS read** (reflash the linboot image, check the partition) |
| `RAMOOPS DIAG` + block of text + `PET WDT` | **this is the previous crash's printk ring** (a warm reset leaves DRAM intact), displayed then parked forever petting the watchdog — this path **exists to be photographed, not to boot**. Seeing it means the previous kernel/U-Boot crashed, not that this boot is stuck |

### Quick triage (last visible line → next action → suspect)

| Last visible | Next action | Suspect |
|---|---|---|
| `DM INIT`/`UFS`/`SCSI` line | UFS/SCSI controller bring-up | UFS init, link |
| nothing after `KERNEL READ` | reading the initramfs | linboot partition name/layout wrong, GPT odd |
| weird `INITRD MAGIC` | gzip magic check | initramfs offset wrong or data bad |
| seconds of nothing before `GUNZIP` | uncached 15.4→43 MB decompression | **normal**, wait |
| `GUNZIP` stuck for a long time | decompression hung | decompression target / memory mapping |
| `ORCL` mismatch | hash check | bad UFS data |
| `MEM SET` nonzero | DTB resize/node write | tree too large (fdt_open_into 1 MiB cap) |
| `JUMP` without `GO` | teardown and jump | jump path |
| black/garbled after `GO` | kernel has taken over | **kernel side** — debug kernel/DTB |
