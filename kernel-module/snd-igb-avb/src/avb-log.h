/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de> */
#ifndef _AVB_LOG_H_
#define _AVB_LOG_H_

#include "igb.h"

#define AVB_DRV_NAME "snd_igb_avb"

static inline void __avb_printk(const struct igb_adapter *adapter,
                                const char *level, const char *fmt, ...)
{
    va_list args;
    char buf[256];
    int len;

    va_start(args, fmt);
    if (adapter && adapter->pdev) {
        if (adapter->netdev) {
            len = snprintf(buf, sizeof(buf), "%s" AVB_DRV_NAME " %s %s: ",
                          level, pci_name(adapter->pdev), adapter->netdev->name);
            vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
        } else {
            len = snprintf(buf, sizeof(buf), "%s" AVB_DRV_NAME " %s: ",
                          level, pci_name(adapter->pdev));
            vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
        }
    } else {
        len = snprintf(buf, sizeof(buf), "%s" AVB_DRV_NAME ": ", level);
        vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
    }
    va_end(args);
    printk("%s", buf);
}

#define avb_info(adapter, fmt, ...) \
    __avb_printk(adapter, KERN_INFO, fmt, ##__VA_ARGS__)

#define avb_warn(adapter, fmt, ...) \
    __avb_printk(adapter, KERN_WARNING, fmt, ##__VA_ARGS__)

#define avb_err(adapter, fmt, ...) \
    __avb_printk(adapter, KERN_ERR, fmt, ##__VA_ARGS__)

#define avb_debug(adapter, fmt, ...) \
    __avb_printk(adapter, KERN_DEBUG, fmt, ##__VA_ARGS__)

#endif