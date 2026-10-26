#pragma once

#include <nod.h>

#ifdef __cplusplus
extern "C" {
#endif

int melee_xbox_open_disc_stream(const char* utf8_path, NodDiscStream* stream);

#ifdef __cplusplus
}
#endif
