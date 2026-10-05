# tb710fu-tools / 辅助工具

Helpers used with this U-Boot fork. See the main README for the full workflow.

- `pack_uboot_image.py <template-boot.img> <u-boot.bin> <out.img>` — wrap u-boot.bin
  (gzip + 4 KiB ANDROID! header with kernel_size = payload length) into an
  ABL-flashable boot image.
- `patch-uboot-linboot.py` / `patch_uboot_uart14.py` — idempotent source patchers
  that produced the changes already applied in this tree (linboot read source;
  bluetooth/uart14 exemption). Kept for provenance/replay on a fresh upstream tree.
