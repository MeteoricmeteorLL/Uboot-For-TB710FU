// SPDX-License-Identifier: GPL-2.0

#ifndef __QCOM_PRIV_H__
#define __QCOM_PRIV_H__

#include <stdbool.h>

/**
 * enum qcom_boot_source - Track where we got loaded from.
 * Used for capsule update logic.
 *
 * @QCOM_BOOT_SOURCE_ANDROID: chainloaded (typically from ABL)
 * @QCOM_BOOT_SOURCE_XBL: flashed to the XBL or UEFI partition
 */
enum qcom_boot_source {
	QCOM_BOOT_SOURCE_ANDROID = 1,
	QCOM_BOOT_SOURCE_XBL,
};

extern enum qcom_boot_source qcom_boot_source;

#if IS_ENABLED(CONFIG_EFI_HAVE_CAPSULE_SUPPORT)
void qcom_configure_capsule_updates(void);
#else
void qcom_configure_capsule_updates(void) {}
#endif /* EFI_HAVE_CAPSULE_SUPPORT */

#if CONFIG_IS_ENABLED(OF_LIVE)
/**
 * qcom_of_fixup_nodes() - Fixup Qualcomm DT nodes
 *
 * Adjusts nodes in the live tree to improve compatibility with U-Boot.
 */
void qcom_of_fixup_nodes(void);
#else
static inline void qcom_of_fixup_nodes(void)
{
	log_debug("Unable to dynamically fixup USB nodes, please enable CONFIG_OF_LIVE\n");
}
#endif /* OF_LIVE */

void gunyah_init(void);

int qcom_parse_memory(const void *fdt);

#endif /* __QCOM_PRIV_H__ */
