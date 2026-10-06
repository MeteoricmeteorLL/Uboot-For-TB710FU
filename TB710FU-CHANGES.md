# TB710FU 变更说明（2026-10-06）

## 这次改了什么

`common/board_r.c` 里那处 **`framebuffer@d5100000` 保留节点的写错字节序**，以及它
**多余的 `no-map`**；另外补上黑匣子两个日志环的保留。同步了随附镜像所用的
`configs/sm8650-lenovo-tb710fu_defconfig`。

```c
/* 改前 —— 小端主机内存被 fdt_setprop() 原样拷进大端 DTB：每个 cell 字节反序 */
u32 reg[4] = { 0, 0xd5100000, 0, 0x1900000 };
fdt_setprop(fdt, fn, "reg", reg, sizeof(reg));   /* 0xd5100000 -> 0x000010d5 */
fdt_setprop_empty(fdt, fn, "no-map");

/* 改后 —— 先把值反转，再让它原样拷贝；并且不要 no-map */
u32 reg[4] = { 0, cpu_to_fdt32(0xd5100000), 0, cpu_to_fdt32(0x1900000) };
fdt_setprop(fdt, fn, "reg", reg, sizeof(reg));
```

## 为什么这是必须修的

内核里有一套自绘的启动调试设施（引导色块 `tb_mark`、tbfb 面板控制台、每 10 秒一个
绿方块的心跳仪 `tb_gauge_draw`、initcall 条），全部通过 `__va(0xd5100000)` 往
bootloader 的开机动画 framebuffer（25 MiB）上画；黑匣子的日志环则写在
`0xB0000000` / `0xD6A00000` 各 1 MiB。

由于上面的字节序 bug，内核实际只保留了 `0x10d5` 处 36,865 字节（根本不是 RAM），
**真正那 25 MiB + 2 MiB 一个字节都没保留**。内核日志里那句

```
framebuffer 1045.framebuffer: framebuffer at 0x1045, 0x9001 bytes
```

就是它。后果是 `/proc/iomem` 里这段显示为普通 `System RAM`，页分配器照常发放，而绘制
代码随即改写落在里面的页 —— 表现为**大文件写入静默损坏**（写 4 GiB 的文件，立刻从页
缓存读回的 md5 与写入源不符，而 drop 缓存后从磁盘读回是对的），以及运行中偶发崩溃。
之所以和文件大小相关：分配器优先用高物理地址段，只在内存压力下才回落到这片低地址。

另外，`no-map` 是第二颗雷：只把地址修对而不去掉它，该区会被剔出内核线性映射，
`__va(0xd5100000)` 立刻翻译失败 —— 正是历史上那个 3.019s 崩溃的形状。

## 实测验证（同一台机器）

| 检查 | 修复前 | 修复后 |
|---|---|---|
| 内存探针（6 轮） | 3 个页被改写为像素值 `00ff00ff`/`000000ff` | **0 坏块** |
| 4 GiB 写：源 / 页缓存 / 磁盘 md5 | 页缓存读不符 | 三方全部一致 |
| 内核日志 | — | `reserved mem: 0xd5100000..0xd69fffff (25600 KiB) map non-reusable` |
| `/proc/iomem` | `d5100000-d7bfffff : System RAM` | 三处 `reserved` 子区间 |

## 构建与刷写

```sh
# 注意：U-Boot 没有 arch/arm64 目录，64 位用 ARCH=arm + CONFIG_ARM64=y
make ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
# 本仓库不含硬编码物理地址之外的改动；产物 u-boot.bin 应为 1 451 384 字节（本次修复版）
```

配套的可刷写镜像在 **Releases** 里（`boot_b-linboot-v3-fbreg.img`，
614 974 字节，sha256 `515e24c567ef937ce8c543189e008191869ddcb24f5079bd3c2b083493ddd642`）。
刷入后**必须冷启动**（长按电源约 15 秒断电再开机）：

```sh
dd if=boot_b-linboot-v3-fbreg.img of=/dev/block/by-name/boot_b bs=4096 conv=fsync
sync
```

> 提示：如果 DTB 里已经声明了这三个 `reserved-memory` 节点（本项目的 Linux 侧已如此），
> U-Boot 会因为 `fdt_add_subnode` 失败而跳过这段，bug 已被规避 —— 刷这个镜像属于把修复
> 落到引导程序本身，换用任何 DTB 都安全。

## 相关

* 完整根因、证据链与修复过程：Linux 侧仓库 `docs/KNOWN-ISSUES.md` 第 1 节。
* 补丁脚本（幂等，可对旧树重放）：`tools/patch-uboot-fbreg.py`。
