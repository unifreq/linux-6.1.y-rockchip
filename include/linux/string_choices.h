/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_STRING_CHOICES_H_
#define _LINUX_STRING_CHOICES_H_

/*
 * Compatibility shim.
 *
 * On this kernel the str_$TRUE_$FALSE helpers (str_yes_no(),
 * str_enable_disable(), ...) still live in <linux/string_helpers.h>.
 * Realtek DSA drivers include <linux/string_choices.h>; forward to the
 * header that actually provides the helpers.
 */
#include <linux/string_helpers.h>

#endif /* _LINUX_STRING_CHOICES_H_ */
