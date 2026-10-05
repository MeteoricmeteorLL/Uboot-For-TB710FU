# SPDX-License-Identifier: GPL-2.0+
#
# (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>

obj-y += board.o gunyah.o dram.o
obj-$(CONFIG_EFI_HAVE_CAPSULE_SUPPORT) += capsule_update.o
# TB710FU: of_fixup.o defines ft_board_setup(), which the linker needs
# whenever CONFIG_OF_BOARD_SETUP is on, so it must not depend on OF_LIVE.
obj-y += of_fixup.o
