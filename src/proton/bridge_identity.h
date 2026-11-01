#ifndef VKSHADE_PROTON_BRIDGE_IDENTITY_H
#define VKSHADE_PROTON_BRIDGE_IDENTITY_H

#include <stdint.h>

typedef struct vkshade_proton_identity
{
    uint64_t cookie;
    uint64_t callback_address;
} vkshade_proton_identity;

typedef char vkshade_proton_identity_size_must_be_16[sizeof(vkshade_proton_identity) == 16 ? 1 : -1];

#endif
