/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Force-included into every game translation unit (see CMakeLists.txt). */
#ifndef PC_COMPAT_H
#define PC_COMPAT_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#if defined(MELEE_XBOX_UWP)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
#include <math.h>

#include "pc/disc.h"
#include "pc/libm/pc_trig.h"

#endif
