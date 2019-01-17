/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once
#include <tilck/common/basic_defs.h>

void init_tty(void);
void tty_setup_for_panic(void);
int tty_keypress_handler_int(u32 key, u8, bool check_mods);
