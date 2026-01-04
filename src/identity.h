#pragma once
#ifndef IDENTITY_H
#define IDENTITY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

int identity_load(account* out, int require_priv);

#ifdef __cplusplus
}
#endif

#endif /* IDENTITY_H */
