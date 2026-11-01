#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "bridge_events.h"
#include "bridge_identity.h"
#include "streamline_abi.h"
#include "vendor/minhook/include/MinHook.h"

typedef int32_t sl_result;
typedef sl_result (*evaluate_fn)(uint32_t, const void*, const sl_base**, uint32_t, void*);
typedef sl_result (*tag_fn)(const void*, const sl_resource_tag*, uint32_t, void*);
typedef sl_result (*tag_frame_fn)(const void*, const void*, const sl_resource_tag*, uint32_t, void*);
typedef sl_result (*get_function_fn)(uint32_t, const char*, void**);
typedef sl_result (*device_fn)(const void*);
typedef sl_result (*init_fn)(const void*, uint64_t);
typedef sl_result (*shutdown_fn)(void);
typedef sl_result (*options_fn)(const void*, const sl_dlssg_options*);

static void(VKSHADE_BRIDGE_ABI* callback)(uint32_t, uint64_t, uint64_t);
static HANDLE          wake_event;
static LONG            initialized;
static LONG64          source_generation = 1;
static LONG            generation_exhausted;
static LONG            shutdowns_in_flight;
static evaluate_fn     original_evaluate;
static tag_fn          original_tag;
static tag_frame_fn    original_tag_frame;
static get_function_fn original_get_function;
static device_fn       original_d3d, original_vulkan;
static init_fn         original_init;
static shutdown_fn     original_shutdown;
static HMODULE         core_owners[8];
static unsigned        core_owner_count;

static const sl_guid tag_guid      = {0x4c6a5aad, 0xb445, 0x496c, {0x87, 0xff, 0x1a, 0xf3, 0x84, 0x5b, 0xe6, 0x53}};
static const sl_guid resource_guid = {0x3a9d70cf, 0x2418, 0x4b72, {0x83, 0x91, 0x13, 0xf8, 0x72, 0x1c, 0x72, 0x61}};
static const sl_guid options_guid  = {0xfac5f1cb, 0x2dfd, 0x4f36, {0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5}};

static void emit(uint32_t kind, uint64_t value, uint64_t generation)
{
    callback(kind, value, generation);
}
static uint64_t generation_at_entry(void)
{
    uint64_t generation;
    if (InterlockedCompareExchange(&shutdowns_in_flight, 0, 0) || InterlockedCompareExchange(&generation_exhausted, 0, 0))
        return 0;
    generation = (uint64_t) InterlockedCompareExchange64(&source_generation, 0, 0);
    if (InterlockedCompareExchange(&shutdowns_in_flight, 0, 0) || InterlockedCompareExchange(&generation_exhausted, 0, 0))
        return 0;
    return generation;
}
static void emit_scoped(uint32_t kind, uint64_t value, uint64_t generation)
{
    if (generation && generation == generation_at_entry())
        emit(kind, value, generation);
}
static void shutdown_event(void)
{
    LONG64 old, next;
    for (;;)
    {
        old = InterlockedCompareExchange64(&source_generation, 0, 0);
        if (old == INT64_MAX)
        {
            InterlockedExchange(&generation_exhausted, 1);
            emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
            return;
        }
        next = old + 1;
        if (InterlockedCompareExchange64(&source_generation, next, old) == old)
            break;
    }
    emit(VKSHADE_BRIDGE_SHUTDOWN, 0, (uint64_t) next);
}
static int matches(const sl_base* base, const sl_guid* guid, size_t version)
{
    return base && base->version == version && memcmp(&base->type, guid, sizeof(*guid)) == 0;
}
static uint64_t tag_mask(const sl_resource_tag* tag)
{
    if (!matches(&tag->base, &tag_guid, 1) || !tag->resource || !matches(&tag->resource->base, &resource_guid, 1) || !tag->resource->native
        || tag->type >= 64)
        return 0;
    return UINT64_C(1) << tag->type;
}
static uint64_t chain_mask(const sl_base* node, uint32_t* inspected, int* limited)
{
    const sl_base* anchor = node;
    uint32_t       power = 1, period = 0;
    uint64_t       mask = 0;
    if (*inspected == 256)
    {
        *limited = 1;
        return 0;
    }
    ++*inspected;
    while (node)
    {
        if (matches(node, &tag_guid, 1))
            mask |= tag_mask((const sl_resource_tag*) node);
        node = node->next;
        if (node)
        {
            ++period;
            if (node == anchor)
            {
                *limited = 1;
                break;
            }
            if (period == power)
            {
                anchor = node;
                power *= 2;
                period = 0;
            }
            if (*inspected == 256)
            {
                *limited = 1;
                break;
            }
            ++*inspected;
        }
    }
    return mask;
}
static uint64_t tags_mask(const sl_resource_tag* tags, uint32_t count, int* limited)
{
    uint64_t mask = 0;
    uint32_t i, inspected = 0;
    if (count > 256)
    {
        *limited = 1;
        return 0;
    }
    if (tags)
        for (i = 0; i < count; ++i)
        {
            if (inspected == 256)
            {
                *limited = 1;
                break;
            }
            mask |= chain_mask(&tags[i].base, &inspected, limited);
            if (*limited)
                break;
        }
    return mask;
}
struct option_slot
{
    void* volatile target;
    HMODULE    owner;
    LONG       published;
    options_fn original;
};
static struct option_slot slots[16];

static void publish_target(void* target)
{
    unsigned i;
    HMODULE  owner;
    if (!target)
        return;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR) target, &owner))
    {
        emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
        return;
    }
    for (i = 0; i < 16; ++i)
    {
        void* previous = InterlockedCompareExchangePointer(&slots[i].target, target, NULL);
        if (!previous)
        {
            slots[i].owner = owner;
            InterlockedExchange(&slots[i].published, 1);
            SetEvent(wake_event);
            return;
        }
        if (previous == target)
        {
            FreeLibrary(owner);
            return;
        }
    }
    FreeLibrary(owner);
    emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
}
static sl_result observe_options(unsigned slot, const void* viewport, const sl_dlssg_options* options)
{
    uint64_t  generation = generation_at_entry();
    int       valid      = options && matches(&options->base, &options_guid, 5) && options->mode <= 3;
    uint32_t  mode       = valid ? options->mode : 0;
    sl_result result     = slots[slot].original(viewport, options);
    if (!result && valid)
        emit_scoped(VKSHADE_BRIDGE_FRAMEGEN_MODE, mode, generation);
    return result;
}
#define OPTION_HOOK(n) \
    static sl_result option_hook_##n(const void* v, const sl_dlssg_options* o) \
    { \
        return observe_options(n, v, o); \
    }
#define VKSHADE_ALL_OPTION_HOOKS \
    OPTION_HOOK(0) \
    OPTION_HOOK(1) \
    OPTION_HOOK(2) \
    OPTION_HOOK(3) \
    OPTION_HOOK(4) \
    OPTION_HOOK(5) \
    OPTION_HOOK(6) \
    OPTION_HOOK(7) \
    OPTION_HOOK(8) \
    OPTION_HOOK(9) \
    OPTION_HOOK(10) \
    OPTION_HOOK(11) \
    OPTION_HOOK(12) \
    OPTION_HOOK(13) \
    OPTION_HOOK(14) \
    OPTION_HOOK(15)
VKSHADE_ALL_OPTION_HOOKS
#undef VKSHADE_ALL_OPTION_HOOKS
#undef OPTION_HOOK
static options_fn option_hooks[16] = {option_hook_0,
                                      option_hook_1,
                                      option_hook_2,
                                      option_hook_3,
                                      option_hook_4,
                                      option_hook_5,
                                      option_hook_6,
                                      option_hook_7,
                                      option_hook_8,
                                      option_hook_9,
                                      option_hook_10,
                                      option_hook_11,
                                      option_hook_12,
                                      option_hook_13,
                                      option_hook_14,
                                      option_hook_15};

static sl_result hook_evaluate(uint32_t feature, const void* frame, const sl_base** inputs, uint32_t count, void* cmd)
{
    uint64_t  generation = generation_at_entry();
    uint64_t  mask       = 0;
    uint32_t  i, inspected = 0;
    int       limited = 0;
    sl_result result;
    if (count > 256)
        limited = 1;
    else if (inputs)
        for (i = 0; i < count; ++i)
        {
            if (inspected == 256)
            {
                limited = 1;
                break;
            }
            mask |= chain_mask(inputs[i], &inspected, &limited);
            if (limited)
                break;
        }
    result = original_evaluate(feature, frame, inputs, count, cmd);
    if (!result)
    {
        if (limited)
            emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
        if (mask)
            emit_scoped(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, mask, generation);
        emit_scoped(VKSHADE_BRIDGE_FEATURE_EVALUATED, feature, generation);
    }
    return result;
}
static sl_result hook_tag(const void* viewport, const sl_resource_tag* tags, uint32_t count, void* cmd)
{
    uint64_t  generation = generation_at_entry();
    int       limited    = 0;
    uint64_t  mask       = tags_mask(tags, count, &limited);
    sl_result result     = original_tag(viewport, tags, count, cmd);
    emit(VKSHADE_BRIDGE_TAG_SUBMISSION, ((uint64_t) (uint32_t) result << 32) | count, 0);
    if (!result)
    {
        if (limited)
            emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
        if (mask)
            emit_scoped(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, mask, generation);
    }
    return result;
}
static sl_result hook_tag_frame(const void* frame, const void* viewport, const sl_resource_tag* tags, uint32_t count, void* cmd)
{
    uint64_t  generation = generation_at_entry();
    int       limited    = 0;
    uint64_t  mask       = tags_mask(tags, count, &limited);
    sl_result result     = original_tag_frame(frame, viewport, tags, count, cmd);
    emit(VKSHADE_BRIDGE_TAG_SUBMISSION, ((uint64_t) (uint32_t) result << 32) | count, 0);
    if (!result)
    {
        if (limited)
            emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
        if (mask)
            emit_scoped(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, mask, generation);
    }
    return result;
}
static sl_result hook_get_function(uint32_t feature, const char* name, void** out)
{
    sl_result result = original_get_function(feature, name, out);
    if (!result && feature == 1000 && name && !strcmp(name, "slDLSSGSetOptions") && out)
        publish_target(*out);
    return result;
}
static sl_result hook_d3d(const void* device)
{
    return original_d3d(device);
}
static sl_result hook_vulkan(const void* info)
{
    return original_vulkan(info);
}
static sl_result hook_init(const void* preferences, uint64_t version)
{
    sl_result r = original_init(preferences, version);
    if (!r)
        emit(VKSHADE_BRIDGE_INTERPOSER_PRESENT, 0, 0);
    return r;
}
static sl_result hook_shutdown(void)
{
    sl_result r;
    InterlockedIncrement(&shutdowns_in_flight);
    /* Both resets invalidate observations even when shutdown fails. */
    shutdown_event();
    r = original_shutdown();
    shutdown_event();
    InterlockedDecrement(&shutdowns_in_flight);
    if (r)
        emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
    return r;
}

static int create_export(HMODULE module, const char* name, void* detour, void** original)
{
    HMODULE owner;
    void*   target = (void*) GetProcAddress(module, name);
    if (!target)
        return 1;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR) target, &owner))
        return 0;
    core_owners[core_owner_count++] = owner;
    return MH_CreateHook(target, detour, original) == MH_OK && MH_QueueEnableHook(target) == MH_OK;
}
static int install_core(HMODULE module)
{
    if (MH_Initialize() != MH_OK)
        return 0;
#define HOOK(name, hook, original) \
    if (!create_export(module, name, (void*) hook, (void**) &original)) \
    goto fail
    HOOK("slEvaluateFeature", hook_evaluate, original_evaluate);
    HOOK("slSetTag", hook_tag, original_tag);
    HOOK("slSetTagForFrame", hook_tag_frame, original_tag_frame);
    HOOK("slGetFeatureFunction", hook_get_function, original_get_function);
    HOOK("slSetD3DDevice", hook_d3d, original_d3d);
    HOOK("slSetVulkanInfo", hook_vulkan, original_vulkan);
    HOOK("slInit", hook_init, original_init);
    HOOK("slShutdown", hook_shutdown, original_shutdown);
#undef HOOK
    if (MH_ApplyQueued() != MH_OK)
        return -1;
    return 1;
fail:
    MH_Uninitialize();
    while (core_owner_count)
        FreeLibrary(core_owners[--core_owner_count]);
    return 0;
}
static void install_options(void)
{
    unsigned             i;
    static unsigned char handled[16];
    for (i = 0; i < 16; ++i)
    {
        MEMORY_BASIC_INFORMATION info;
        DWORD                    protection;
        void*                    target;
        HMODULE                  owner;
        if (!InterlockedCompareExchange(&slots[i].published, 0, 0) || handled[i])
            continue;
        target     = slots[i].target;
        owner      = slots[i].owner;
        handled[i] = 1;
        if (!VirtualQuery(target, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        {
            FreeLibrary(owner);
            emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
            continue;
        }
        protection = info.Protect & 0xff;
        if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE
            && protection != PAGE_EXECUTE_WRITECOPY)
        {
            FreeLibrary(owner);
            emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
            continue;
        }
        if (MH_CreateHook(target, (void*) option_hooks[i], (void**) &slots[i].original) != MH_OK)
        {
            FreeLibrary(owner);
            emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
            continue;
        }
        if (MH_EnableHook(target) != MH_OK)
        {
            emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
        }
        /* An enable attempt can leave a thread in the trampoline even on failure. */
    }
}
static DWORD WINAPI worker(void* unused)
{
    HMODULE module = NULL;
    int     installed;
    (void) unused;
    emit(VKSHADE_BRIDGE_MONITORING, 0, 0);
    while (!GetModuleHandleExW(0, L"sl.interposer.dll", &module))
        WaitForSingleObject(wake_event, 1000);
    emit(VKSHADE_BRIDGE_INTERPOSER_PRESENT, 0, 0);
    installed = install_core(module);
    if (installed <= 0)
    {
        if (!installed)
            FreeLibrary(module);
        emit(VKSHADE_BRIDGE_UNAVAILABLE, 0, 0);
        return 0;
    }
    emit(VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, 0);
    publish_target((void*) GetProcAddress(module, "slDLSSGSetOptions"));
    for (;;)
    {
        install_options();
        WaitForSingleObject(wake_event, INFINITE);
    }
}

__declspec(dllexport) DWORD WINAPI vkshade_companion_initialize_v1(void* identity_address)
{
    vkshade_proton_identity identity;
    HANDLE                  thread;
    if (!identity_address || InterlockedCompareExchange(&initialized, 1, 0) != 0)
        return 1;
    memcpy(&identity, identity_address, sizeof(identity));
    if (!identity.callback_address)
    {
        InterlockedExchange(&initialized, 0);
        return 1;
    }
    callback   = (void(VKSHADE_BRIDGE_ABI*)(uint32_t, uint64_t, uint64_t))(uintptr_t) identity.callback_address;
    wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!wake_event)
    {
        InterlockedExchange(&initialized, 0);
        return 1;
    }
    thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!thread)
    {
        CloseHandle(wake_event);
        InterlockedExchange(&initialized, 0);
        return 1;
    }
    CloseHandle(thread);
    return 0;
}
