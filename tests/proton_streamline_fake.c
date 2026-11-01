#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <intrin.h>

typedef struct sl_guid
{
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} sl_guid;

typedef struct sl_base
{
    struct sl_base* next;
    sl_guid         type;
    uint64_t        version;
} sl_base;

typedef struct sl_preferences
{
    sl_base         base;
    uint8_t         show_console;
    uint8_t         reserved0[3];
    uint32_t        log_level;
    const wchar_t** paths_to_plugins;
    uint32_t        num_paths_to_plugins;
    const wchar_t*  path_to_logs_and_data;
    void*           allocate_callback;
    void*           release_callback;
    void*           log_message_callback;
    uint64_t        flags;
    const uint32_t* features_to_load;
    uint32_t        num_features_to_load;
    uint32_t        application_id;
    uint32_t        engine;
    const char*     engine_version;
    const char*     project_id;
    uint32_t        render_api;
} sl_preferences;

typedef struct sl_resource
{
    sl_base base;
    uint8_t resource_type;
    uint8_t reserved[7];
    void*   native;
} sl_resource;

typedef struct sl_resource_tag
{
    sl_base      base;
    sl_resource* resource;
    uint32_t     type;
    int32_t      lifecycle;
    uint32_t     extent[4];
} sl_resource_tag;

typedef struct sl_viewport
{
    sl_base  base;
    uint32_t value;
} sl_viewport;

typedef struct sl_dlssg_options
{
    sl_base  base;
    uint32_t mode;
    uint32_t reserved[17];
} sl_dlssg_options;

typedef struct sl_snapshot
{
    uint32_t  init_calls;
    uint32_t  shutdown_calls;
    uint32_t  evaluate_calls;
    uint32_t  set_tag_calls;
    uint32_t  set_tag_for_frame_calls;
    uint32_t  get_feature_function_calls;
    uint32_t  set_options_calls;
    uint32_t  get_state_calls;
    uint32_t  last_feature;
    uint32_t  last_count;
    uint32_t  last_mode;
    uint64_t  last_sdk_version;
    uintptr_t last_preferences;
    uintptr_t last_frame;
    uintptr_t last_inputs;
    uintptr_t last_command_buffer;
    uintptr_t last_viewport;
    uintptr_t last_tags;
    uintptr_t last_options;
} sl_snapshot;

typedef int32_t(WINAPI* sl_dlssg_set_options_fn)(const sl_viewport*, const sl_dlssg_options*);
typedef int32_t(WINAPI* sl_dlssg_get_state_fn)(const sl_viewport*, void*, const sl_dlssg_options*);

_Static_assert(sizeof(sl_base) == 32, "Streamline BaseStructure layout");
_Static_assert(offsetof(sl_preferences, flags) == 88, "Streamline Preferences layout");
_Static_assert(offsetof(sl_resource, native) == 40, "Streamline Resource layout");
_Static_assert(offsetof(sl_resource_tag, type) == 40, "Streamline ResourceTag layout");
_Static_assert(offsetof(sl_dlssg_options, mode) == 32, "Streamline DLSSGOptions layout");

static volatile LONG   g_init_calls;
static volatile LONG   g_shutdown_calls;
static volatile LONG   g_evaluate_calls;
static volatile LONG   g_set_tag_calls;
static volatile LONG   g_set_tag_for_frame_calls;
static volatile LONG   g_get_feature_function_calls;
static volatile LONG   g_set_options_calls;
static volatile LONG   g_get_state_calls;
static volatile LONG   g_init_result;
static volatile LONG   g_shutdown_result;
static volatile LONG   g_evaluate_result;
static volatile LONG   g_tags_result;
static volatile LONG   g_options_result;
static volatile LONG   g_last_feature;
static volatile LONG   g_last_count;
static volatile LONG   g_last_mode;
static volatile LONG64 g_last_sdk_version;
static void* volatile g_last_preferences;
static void* volatile g_last_frame;
static void* volatile g_last_inputs;
static void* volatile g_last_command_buffer;
static void* volatile g_last_viewport;
static void* volatile g_last_tags;
static void* volatile g_last_options;

static int32_t WINAPI slDLSSGSetOptions(const sl_viewport* viewport, const sl_dlssg_options* options)
{
    InterlockedExchangePointer(&g_last_viewport, (void*) viewport);
    InterlockedExchangePointer(&g_last_options, (void*) options);
    InterlockedExchange(&g_last_mode, (LONG) options->mode);
    InterlockedIncrement(&g_set_options_calls);
    return InterlockedCompareExchange(&g_options_result, 0, 0);
}

static int32_t WINAPI slDLSSGGetState(const sl_viewport* viewport, void* state, const sl_dlssg_options* options)
{
    (void) viewport;
    (void) state;
    (void) options;
    InterlockedIncrement(&g_get_state_calls);
    return 0;
}

__declspec(dllexport) int32_t WINAPI slInit(const sl_preferences* preferences, uint64_t sdk_version)
{
    InterlockedExchangePointer(&g_last_preferences, (void*) preferences);
    InterlockedExchange64(&g_last_sdk_version, (LONG64) sdk_version);
    InterlockedIncrement(&g_init_calls);
    return InterlockedCompareExchange(&g_init_result, 0, 0);
}

static BOOL same_text(const char* left, const char* right)
{
    while (*left != '\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

__declspec(dllexport) int32_t WINAPI slShutdown(void)
{
    InterlockedIncrement(&g_shutdown_calls);
    return InterlockedCompareExchange(&g_shutdown_result, 0, 0);
}

__declspec(dllexport) int32_t WINAPI
slEvaluateFeature(uint32_t feature, const void* frame, const sl_base* const* inputs, uint32_t count, void* command_buffer)
{
    InterlockedExchange(&g_last_feature, (LONG) feature);
    InterlockedExchange(&g_last_count, (LONG) count);
    InterlockedExchangePointer(&g_last_frame, (void*) frame);
    InterlockedExchangePointer(&g_last_inputs, (void*) inputs);
    InterlockedExchangePointer(&g_last_command_buffer, command_buffer);
    InterlockedIncrement(&g_evaluate_calls);
    return InterlockedCompareExchange(&g_evaluate_result, 0, 0);
}

__declspec(dllexport) int32_t WINAPI slSetTag(const sl_viewport* viewport, const sl_resource_tag* tags, uint32_t count, void* command_buffer)
{
    InterlockedExchange(&g_last_count, (LONG) count);
    InterlockedExchangePointer(&g_last_viewport, (void*) viewport);
    InterlockedExchangePointer(&g_last_tags, (void*) tags);
    InterlockedExchangePointer(&g_last_command_buffer, command_buffer);
    InterlockedIncrement(&g_set_tag_calls);
    return InterlockedCompareExchange(&g_tags_result, 0, 0);
}

__declspec(dllexport) int32_t WINAPI
slSetTagForFrame(const void* frame, const sl_viewport* viewport, const sl_resource_tag* tags, uint32_t count, void* command_buffer)
{
    InterlockedExchange(&g_last_count, (LONG) count);
    InterlockedExchangePointer(&g_last_frame, (void*) frame);
    InterlockedExchangePointer(&g_last_viewport, (void*) viewport);
    InterlockedExchangePointer(&g_last_tags, (void*) tags);
    InterlockedExchangePointer(&g_last_command_buffer, command_buffer);
    InterlockedIncrement(&g_set_tag_for_frame_calls);
    return InterlockedCompareExchange(&g_tags_result, 0, 0);
}

__declspec(dllexport) int32_t WINAPI slGetFeatureFunction(uint32_t feature, const char* name, void** function)
{
    InterlockedExchange(&g_last_feature, (LONG) feature);
    InterlockedIncrement(&g_get_feature_function_calls);
    if (feature == 1000 && same_text(name, "slDLSSGSetOptions"))
    {
        *function = (void*) slDLSSGSetOptions;
        return 0;
    }
    if (feature == 1000 && same_text(name, "slDLSSGGetState"))
    {
        *function = (void*) slDLSSGGetState;
        return 0;
    }
    return 1;
}

__declspec(dllexport) void* WINAPI slTestExpectedSetOptions(void)
{
    return (void*) slDLSSGSetOptions;
}

__declspec(dllexport) void WINAPI
slTestSetResults(int32_t init_result, int32_t shutdown_result, int32_t evaluate_result, int32_t tags_result, int32_t options_result)
{
    InterlockedExchange(&g_init_result, init_result);
    InterlockedExchange(&g_shutdown_result, shutdown_result);
    InterlockedExchange(&g_evaluate_result, evaluate_result);
    InterlockedExchange(&g_tags_result, tags_result);
    InterlockedExchange(&g_options_result, options_result);
}

__declspec(dllexport) void WINAPI slTestGetSnapshot(sl_snapshot* out)
{
    out->init_calls                 = (uint32_t) InterlockedCompareExchange(&g_init_calls, 0, 0);
    out->shutdown_calls             = (uint32_t) InterlockedCompareExchange(&g_shutdown_calls, 0, 0);
    out->evaluate_calls             = (uint32_t) InterlockedCompareExchange(&g_evaluate_calls, 0, 0);
    out->set_tag_calls              = (uint32_t) InterlockedCompareExchange(&g_set_tag_calls, 0, 0);
    out->set_tag_for_frame_calls    = (uint32_t) InterlockedCompareExchange(&g_set_tag_for_frame_calls, 0, 0);
    out->get_feature_function_calls = (uint32_t) InterlockedCompareExchange(&g_get_feature_function_calls, 0, 0);
    out->set_options_calls          = (uint32_t) InterlockedCompareExchange(&g_set_options_calls, 0, 0);
    out->get_state_calls            = (uint32_t) InterlockedCompareExchange(&g_get_state_calls, 0, 0);
    out->last_feature               = (uint32_t) InterlockedCompareExchange(&g_last_feature, 0, 0);
    out->last_count                 = (uint32_t) InterlockedCompareExchange(&g_last_count, 0, 0);
    out->last_mode                  = (uint32_t) InterlockedCompareExchange(&g_last_mode, 0, 0);
    out->last_sdk_version           = (uint64_t) InterlockedCompareExchange64(&g_last_sdk_version, 0, 0);
    out->last_preferences           = (uintptr_t) InterlockedCompareExchangePointer(&g_last_preferences, NULL, NULL);
    out->last_frame                 = (uintptr_t) InterlockedCompareExchangePointer(&g_last_frame, NULL, NULL);
    out->last_inputs                = (uintptr_t) InterlockedCompareExchangePointer(&g_last_inputs, NULL, NULL);
    out->last_command_buffer        = (uintptr_t) InterlockedCompareExchangePointer(&g_last_command_buffer, NULL, NULL);
    out->last_viewport              = (uintptr_t) InterlockedCompareExchangePointer(&g_last_viewport, NULL, NULL);
    out->last_tags                  = (uintptr_t) InterlockedCompareExchangePointer(&g_last_tags, NULL, NULL);
    out->last_options               = (uintptr_t) InterlockedCompareExchangePointer(&g_last_options, NULL, NULL);
}
