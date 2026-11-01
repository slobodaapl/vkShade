#ifndef VKSHADE_STREAMLINE_ABI_H
#define VKSHADE_STREAMLINE_ABI_H

/* Minimal declarations derived from NVIDIA Streamline SDK; see streamline_LICENSE.txt. */
#include <stddef.h>
#include <stdint.h>

typedef struct sl_guid
{
    uint32_t data1;
    uint16_t data2, data3;
    uint8_t  data4[8];
} sl_guid;

typedef struct sl_base
{
    const struct sl_base* next;
    sl_guid               type;
    size_t                version;
} sl_base;

typedef struct sl_resource
{
    sl_base  base;
    char     type;
    void *   native, *memory, *view;
    uint32_t state, width, height, native_format, mip_levels, array_layers;
    uint64_t gpu_virtual_address;
    uint32_t flags, usage, reserved;
} sl_resource;

typedef struct sl_resource_tag
{
    sl_base            base;
    const sl_resource* resource;
    uint32_t           type;
    int32_t            lifecycle;
    uint32_t           top, left, width, height;
} sl_resource_tag;

typedef struct sl_dlssg_options
{
    sl_base  base;
    uint32_t mode, num_frames, flags;
    uint32_t dynamic_width, dynamic_height, num_back_buffers;
    uint32_t mvec_depth_width, mvec_depth_height, color_width, color_height;
    uint32_t color_format, mvec_format, depth_format, hudless_format, ui_format;
    void*    on_api_error;
    char     reserved15;
    uint32_t queue_parallelism;
    char     ui_recomposition;
    float    dynamic_target_frame_rate;
} sl_dlssg_options;

_Static_assert(sizeof(sl_guid) == 16, "Streamline GUID ABI");
_Static_assert(sizeof(sl_base) == 32 && offsetof(sl_base, type) == 8 && offsetof(sl_base, version) == 24, "Streamline base ABI");
_Static_assert(sizeof(sl_resource) == 112 && offsetof(sl_resource, native) == 40 && offsetof(sl_resource, gpu_virtual_address) == 88,
               "Streamline resource ABI");
_Static_assert(sizeof(sl_resource_tag) == 64 && offsetof(sl_resource_tag, resource) == 32 && offsetof(sl_resource_tag, type) == 40,
               "Streamline tag ABI");
_Static_assert(sizeof(sl_dlssg_options) == 120 && offsetof(sl_dlssg_options, mode) == 32 && offsetof(sl_dlssg_options, on_api_error) == 96
                   && offsetof(sl_dlssg_options, dynamic_target_frame_rate) == 116,
               "Streamline DLSSG ABI");

#endif
