#include <windows.h>
#include <shellapi.h>
#include <stddef.h>
#include <stdint.h>

#include "proton/bridge_events.h"
#include "proton/bridge_identity.h"

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

typedef struct sl_precision_info
{
    sl_base  base;
    uint32_t conversion_formula;
    float    bias;
    float    scale;
} sl_precision_info;

typedef struct sl_viewport
{
    sl_base  base;
    uint32_t value;
} sl_viewport;

typedef struct sl_frame_token
{
    uintptr_t* vtable;
    sl_base    base;
} sl_frame_token;

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

typedef struct bridge_event
{
    uint32_t kind;
    uint64_t value;
    uint64_t generation;
} bridge_event;

typedef struct eval_thread_context
{
    int32_t(WINAPI* evaluate)(uint32_t, const void*, const sl_base* const*, uint32_t, void*);
    uint32_t feature;
    LONG     failed;
} eval_thread_context;

typedef struct injector_shared_state
{
    volatile LONG callback_count;
    volatile LONG monitoring_count;
    volatile LONG interposer_count;
    DWORD         target_pid;
    uintptr_t     identity_address;
    uint64_t      cookie;
} injector_shared_state;

typedef struct pipe_capture
{
    HANDLE handle;
    char   bytes[4096];
    DWORD  length;
    BOOL   truncated;
} pipe_capture;

typedef struct injector_outcome
{
    DWORD exit_code;
    DWORD stderr_length;
    char  stderr_bytes[4096];
    BOOL  stderr_truncated;
} injector_outcome;

typedef DWORD(WINAPI* initialize_fn)(void*);
typedef int32_t(WINAPI* init_fn)(const sl_preferences*, uint64_t);
typedef int32_t(WINAPI* shutdown_fn)(void);
typedef int32_t(WINAPI* evaluate_fn)(uint32_t, const void*, const sl_base* const*, uint32_t, void*);
typedef int32_t(WINAPI* set_tag_fn)(const sl_viewport*, const sl_resource_tag*, uint32_t, void*);
typedef int32_t(WINAPI* set_tag_for_frame_fn)(const void*, const sl_viewport*, const sl_resource_tag*, uint32_t, void*);
typedef int32_t(WINAPI* get_feature_function_fn)(uint32_t, const char*, void**);
typedef int32_t(WINAPI* set_options_fn)(const sl_viewport*, const sl_dlssg_options*);
typedef void(WINAPI* set_results_fn)(int32_t, int32_t, int32_t, int32_t, int32_t);
typedef void(WINAPI* get_snapshot_fn)(sl_snapshot*);
typedef void*(WINAPI* expected_options_fn)(void);

_Static_assert(sizeof(sl_base) == 32, "Streamline BaseStructure layout");
_Static_assert(offsetof(sl_preferences, flags) == 88, "Streamline Preferences layout");
_Static_assert(offsetof(sl_frame_token, base) == 8, "Streamline FrameToken layout");
_Static_assert(offsetof(sl_resource, native) == 40, "Streamline Resource layout");
_Static_assert(offsetof(sl_resource_tag, type) == 40, "Streamline ResourceTag layout");
_Static_assert(offsetof(sl_precision_info, conversion_formula) == 32, "Streamline PrecisionInfo layout");
_Static_assert(offsetof(sl_dlssg_options, mode) == 32, "Streamline DLSSGOptions layout");

#define EVENT_CAPACITY             4096
#define COMMAND_LINE_CAPACITY      32768
#define OBSERVATION_TIMEOUT_MS     30000ull
#define FEATURE_DLSS_G             1000u
#define RESOURCE_TAG_GUID          0x4c6a5aadu
#define RESOURCE_MASK(types)       (((uint64_t) 1u << (types)))
#define TEST_BRIDGE_TAG_SUBMISSION 10u
#define TAG_BOUNDARY_EXTENSIONS    254u

static CRITICAL_SECTION       g_event_lock;
static bridge_event           g_events[EVENT_CAPACITY];
static LONG                   g_event_count;
static BOOL                   g_event_overflow;
static injector_shared_state* g_injector_shared;

static sl_base         g_input_root;
static sl_resource     g_resources[8];
static sl_resource_tag g_tags[8];
static sl_viewport     g_viewport;
static uintptr_t       g_frame_vtable[1];
static sl_frame_token  g_frame_token;
static sl_preferences  g_preferences;
static uint64_t        g_command_buffer;

static HMODULE                 g_fixture;
static get_snapshot_fn         g_get_snapshot;
static set_results_fn          g_set_results;
static init_fn                 g_init;
static shutdown_fn             g_shutdown;
static evaluate_fn             g_evaluate;
static set_tag_fn              g_set_tag;
static set_tag_for_frame_fn    g_set_tag_for_frame;
static get_feature_function_fn g_get_feature_function;
static set_options_fn          g_set_options;
static expected_options_fn     g_expected_options;

static uint32_t WINAPI frame_index(const sl_frame_token* frame)
{
    (void) frame;
    return 7;
}

static void WINAPI observe_event(uint32_t kind, uint64_t value, uint64_t generation)
{
    if (g_injector_shared != NULL)
    {
        InterlockedIncrement(&g_injector_shared->callback_count);
        if (kind == VKSHADE_BRIDGE_MONITORING)
            InterlockedIncrement(&g_injector_shared->monitoring_count);
        if (kind == VKSHADE_BRIDGE_INTERPOSER_PRESENT)
            InterlockedIncrement(&g_injector_shared->interposer_count);
    }
    EnterCriticalSection(&g_event_lock);
    if (g_event_count < EVENT_CAPACITY)
    {
        g_events[g_event_count].kind       = kind;
        g_events[g_event_count].value      = value;
        g_events[g_event_count].generation = generation;
        ++g_event_count;
    }
    else
    {
        g_event_overflow = TRUE;
    }
    LeaveCriticalSection(&g_event_lock);
}

static void VKSHADE_BRIDGE_ABI observe_bridge_event(uint32_t kind, uint64_t value, uint64_t generation)
{
    observe_event(kind, value, generation);
}

static void write_text(const char* text)
{
    DWORD written;
    DWORD size = 0;
    while (text[size] != '\0')
        ++size;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, size, &written, NULL);
}

static int fail(const char* message)
{
    write_text("FAIL: ");
    write_text(message);
    write_text("\r\n");
    return 1;
}

#define CHECK(condition, message) \
    do \
    { \
        if (!(condition)) \
            return fail(message); \
    } while (0)

static void set_guid(sl_guid* guid, uint32_t data1, uint16_t data2, uint16_t data3)
{
    guid->data1    = data1;
    guid->data2    = data2;
    guid->data3    = data3;
    guid->data4[0] = 0x87;
    guid->data4[1] = 0xff;
    guid->data4[2] = 0x1a;
    guid->data4[3] = 0xf3;
    guid->data4[4] = 0x84;
    guid->data4[5] = 0x5b;
    guid->data4[6] = 0xe6;
    guid->data4[7] = 0x53;
}

static void set_base(sl_base* base, uint32_t data1, uint16_t data2, uint16_t data3, uint64_t version)
{
    base->next = NULL;
    set_guid(&base->type, data1, data2, data3);
    base->version = version;
}

static void set_guid_tail(sl_guid* guid, const uint8_t tail[8])
{
    uint32_t i;
    for (i = 0; i < 8; ++i)
        guid->data4[i] = tail[i];
}

static void set_runtime_objects(void)
{
    static const uint8_t viewport_tail[8]    = {0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4};
    static const uint8_t frame_tail[8]       = {0xa8, 0x04, 0x59, 0xb2, 0x06, 0x49, 0x9b, 0x18};
    static const uint8_t preferences_tail[8] = {0x8d, 0xa1, 0x67, 0x16, 0xd8, 0x79, 0xfb, 0x14};
    set_base(&g_viewport.base, 0x171b6435, 0x9b3c, 0x4fc8, 1);
    set_guid_tail(&g_viewport.base.type, viewport_tail);
    g_viewport.value = 7;
    set_base(&g_frame_token.base, 0x830a0f35, 0xdb84, 0x4171, 1);
    set_guid_tail(&g_frame_token.base.type, frame_tail);
    g_frame_vtable[0]    = (uintptr_t) &frame_index;
    g_frame_token.vtable = g_frame_vtable;
    set_base(&g_preferences.base, 0x1ca10965, 0xbf8e, 0x432b, 1);
    set_guid_tail(&g_preferences.base.type, preferences_tail);
    g_preferences.log_level = 1;
    g_preferences.flags     = (1ull << 0) | (1ull << 3) | (1ull << 6);
}

static void set_tag(sl_resource_tag* tag, sl_resource* resource, uint32_t type, uint64_t version)
{
    set_base(&tag->base, RESOURCE_TAG_GUID, 0xb445, 0x496c, version);
    tag->resource  = resource;
    tag->type      = type;
    tag->lifecycle = 1;
}

static void set_resource(sl_resource* resource, void* native)
{
    resource->base.next          = NULL;
    resource->base.type.data1    = 0x3a9d70cf;
    resource->base.type.data2    = 0x2418;
    resource->base.type.data3    = 0x4b72;
    resource->base.type.data4[0] = 0x83;
    resource->base.type.data4[1] = 0x91;
    resource->base.type.data4[2] = 0x13;
    resource->base.type.data4[3] = 0xf8;
    resource->base.type.data4[4] = 0x72;
    resource->base.type.data4[5] = 0x1c;
    resource->base.type.data4[6] = 0x72;
    resource->base.type.data4[7] = 0x61;
    resource->base.version       = 1;
    resource->native             = native;
}

static LONG event_count(void)
{
    LONG count;
    EnterCriticalSection(&g_event_lock);
    count = g_event_count;
    LeaveCriticalSection(&g_event_lock);
    return count;
}

static BOOL find_event_since(LONG start, uint32_t kind, uint64_t value, BOOL match_value, uint64_t* generation)
{
    LONG i;
    BOOL found = FALSE;
    EnterCriticalSection(&g_event_lock);
    for (i = start < 0 ? 0 : start; i < g_event_count; ++i)
    {
        if (g_events[i].kind == kind && (!match_value || g_events[i].value == value))
        {
            if (generation != NULL)
                *generation = g_events[i].generation;
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&g_event_lock);
    return found;
}

static uint64_t tag_submission_payload(int32_t result, uint32_t count)
{
    return ((uint64_t) (uint32_t) result << 32) | count;
}

static BOOL tag_submission_window_matches(LONG start, uint64_t payload, uint32_t expected_count)
{
    LONG     i;
    uint32_t count = 0;
    BOOL     valid = TRUE;
    EnterCriticalSection(&g_event_lock);
    for (i = start < 0 ? 0 : start; i < g_event_count; ++i)
    {
        if (g_events[i].kind == TEST_BRIDGE_TAG_SUBMISSION)
        {
            ++count;
            if (g_events[i].value != payload || g_events[i].generation != 0)
                valid = FALSE;
        }
    }
    LeaveCriticalSection(&g_event_lock);
    return valid && count == expected_count;
}

static BOOL wait_single_tag_submission(LONG start, int32_t result, uint32_t count)
{
    ULONGLONG deadline = GetTickCount64() + OBSERVATION_TIMEOUT_MS;
    uint64_t  payload  = tag_submission_payload(result, count);
    while (GetTickCount64() < deadline)
    {
        LONG     i;
        uint32_t found = 0;
        BOOL     valid = TRUE;
        EnterCriticalSection(&g_event_lock);
        for (i = start < 0 ? 0 : start; i < g_event_count; ++i)
        {
            if (g_events[i].kind == TEST_BRIDGE_TAG_SUBMISSION)
            {
                ++found;
                if (g_events[i].value != payload || g_events[i].generation != 0)
                    valid = FALSE;
            }
        }
        LeaveCriticalSection(&g_event_lock);
        if (!valid || found > 1)
            return FALSE;
        if (found == 1)
        {
            Sleep(150);
            return tag_submission_window_matches(start, payload, 1);
        }
        Sleep(10);
    }
    return FALSE;
}

static uint64_t tag_bits_since(LONG start)
{
    LONG     i;
    uint64_t bits = 0;
    EnterCriticalSection(&g_event_lock);
    for (i = start < 0 ? 0 : start; i < g_event_count; ++i)
    {
        if (g_events[i].kind == VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN)
            bits |= g_events[i].value;
    }
    LeaveCriticalSection(&g_event_lock);
    return bits;
}

static BOOL wait_tag_bits(LONG start, uint64_t required_bits)
{
    ULONGLONG deadline = GetTickCount64() + OBSERVATION_TIMEOUT_MS;
    while (GetTickCount64() < deadline)
    {
        if ((tag_bits_since(start) & required_bits) == required_bits)
            return TRUE;
        Sleep(10);
    }
    return FALSE;
}

static BOOL wait_event(LONG start, uint32_t kind, uint64_t value, BOOL match_value, uint64_t* generation)
{
    ULONGLONG deadline = GetTickCount64() + OBSERVATION_TIMEOUT_MS;
    while (GetTickCount64() < deadline)
    {
        if (find_event_since(start, kind, value, match_value, generation))
            return TRUE;
        Sleep(10);
    }
    return FALSE;
}

static void get_snapshot(sl_snapshot* snapshot)
{
    g_get_snapshot(snapshot);
}

typedef enum counter_kind
{
    COUNTER_EVALUATE,
    COUNTER_SET_TAG,
    COUNTER_SET_TAG_FOR_FRAME,
    COUNTER_SET_OPTIONS
} counter_kind;

static uint32_t counter_value(const sl_snapshot* snapshot, counter_kind counter)
{
    switch (counter)
    {
        case COUNTER_EVALUATE: return snapshot->evaluate_calls;
        case COUNTER_SET_TAG: return snapshot->set_tag_calls;
        case COUNTER_SET_TAG_FOR_FRAME: return snapshot->set_tag_for_frame_calls;
        case COUNTER_SET_OPTIONS: return snapshot->set_options_calls;
    }
    return 0;
}

typedef int32_t(WINAPI* attempt_fn)(void*);

static BOOL drive_until(attempt_fn   attempt,
                        void*        context,
                        counter_kind counter,
                        int32_t      expected_result,
                        LONG         start,
                        uint32_t     event_kind,
                        uint64_t     event_value,
                        BOOL         match_value,
                        LONG*        attempts)
{
    sl_snapshot before;
    sl_snapshot after;
    uint32_t    i        = 0;
    ULONGLONG   deadline = GetTickCount64() + OBSERVATION_TIMEOUT_MS;
    get_snapshot(&before);
    while (GetTickCount64() < deadline)
    {
        ++i;
        int32_t result = attempt(context);
        if (result != expected_result)
            return FALSE;
        get_snapshot(&after);
        if (counter_value(&after, counter) != counter_value(&before, counter) + i)
            return FALSE;
        if (find_event_since(start, event_kind, event_value, match_value, NULL))
        {
            *attempts = (LONG) i;
            return TRUE;
        }
        Sleep(10);
    }
    return FALSE;
}

typedef struct eval_call
{
    evaluate_fn           function;
    uint32_t              feature;
    const void*           frame;
    const sl_base* const* inputs;
    uint32_t              count;
    void*                 command_buffer;
} eval_call;

typedef struct set_tag_call
{
    set_tag_fn             function;
    const sl_viewport*     viewport;
    const sl_resource_tag* tags;
    uint32_t               count;
    void*                  command_buffer;
} set_tag_call;

typedef struct set_tag_for_frame_call
{
    set_tag_for_frame_fn   function;
    const void*            frame;
    const sl_viewport*     viewport;
    const sl_resource_tag* tags;
    uint32_t               count;
    void*                  command_buffer;
} set_tag_for_frame_call;

typedef struct options_call
{
    set_options_fn          function;
    const sl_viewport*      viewport;
    const sl_dlssg_options* options;
} options_call;

typedef struct bounded_action
{
    attempt_fn action;
    void*      context;
    int32_t    result;
} bounded_action;

static int32_t attempt_eval(void* context)
{
    eval_call* call = (eval_call*) context;
    return call->function(call->feature, call->frame, call->inputs, call->count, call->command_buffer);
}

static int32_t attempt_set_tag(void* context)
{
    set_tag_call* call = (set_tag_call*) context;
    return call->function(call->viewport, call->tags, call->count, call->command_buffer);
}

static int32_t attempt_set_tag_for_frame(void* context)
{
    set_tag_for_frame_call* call = (set_tag_for_frame_call*) context;
    return call->function(call->frame, call->viewport, call->tags, call->count, call->command_buffer);
}

static int32_t attempt_options(void* context)
{
    options_call* call = (options_call*) context;
    return call->function(call->viewport, call->options);
}

static DWORD WINAPI bounded_action_worker(void* context)
{
    bounded_action* call = (bounded_action*) context;
    call->result         = call->action(call->context);
    return 0;
}

static BOOL invoke_bounded(attempt_fn action, void* context, int32_t expected_result)
{
    bounded_action call;
    HANDLE         thread;
    DWORD          wait_result;
    call.action  = action;
    call.context = context;
    call.result  = INT32_MIN;
    thread       = CreateThread(NULL, 0, bounded_action_worker, &call, 0, NULL);
    if (thread == NULL)
        return FALSE;
    wait_result = WaitForSingleObject(thread, 10000);
    if (wait_result == WAIT_TIMEOUT)
        ExitProcess(127);
    if (wait_result != WAIT_OBJECT_0)
    {
        TerminateThread(thread, 127);
        CloseHandle(thread);
        return FALSE;
    }
    CloseHandle(thread);
    return call.result == expected_result;
}

static BOOL no_success_event_after(LONG start, uint32_t kind, uint64_t value)
{
    Sleep(150);
    return !find_event_since(start, kind, value, TRUE, NULL);
}

static BOOL no_tag_bits_outside_after(LONG start, uint64_t allowed_bits)
{
    LONG i;
    BOOL valid = TRUE;
    Sleep(150);
    EnterCriticalSection(&g_event_lock);
    for (i = start < 0 ? 0 : start; i < g_event_count; ++i)
    {
        if (g_events[i].kind == VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN && (g_events[i].value & ~allowed_bits) != 0)
        {
            valid = FALSE;
            break;
        }
    }
    LeaveCriticalSection(&g_event_lock);
    return valid;
}

static BOOL all_scoped_events_have_generation(void)
{
    LONG i;
    BOOL valid = TRUE;
    EnterCriticalSection(&g_event_lock);
    for (i = 0; i < g_event_count; ++i)
    {
        uint32_t kind = g_events[i].kind;
        if ((kind == VKSHADE_BRIDGE_FEATURE_EVALUATED || kind == VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN || kind == VKSHADE_BRIDGE_FRAMEGEN_MODE)
            && g_events[i].generation == 0)
        {
            valid = FALSE;
            break;
        }
        if ((kind == VKSHADE_BRIDGE_MONITORING || kind == VKSHADE_BRIDGE_INTERPOSER_PRESENT) && g_events[i].generation != 0)
        {
            valid = FALSE;
            break;
        }
        if (kind == TEST_BRIDGE_TAG_SUBMISSION && g_events[i].generation != 0)
        {
            valid = FALSE;
            break;
        }
    }
    LeaveCriticalSection(&g_event_lock);
    return valid;
}

static uint64_t max_scoped_generation(void)
{
    LONG     i;
    uint64_t maximum = 0;
    EnterCriticalSection(&g_event_lock);
    for (i = 0; i < g_event_count; ++i)
    {
        uint32_t kind = g_events[i].kind;
        if ((kind == VKSHADE_BRIDGE_FEATURE_EVALUATED || kind == VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN || kind == VKSHADE_BRIDGE_FRAMEGEN_MODE)
            && g_events[i].generation > maximum)
            maximum = g_events[i].generation;
    }
    LeaveCriticalSection(&g_event_lock);
    return maximum;
}

static BOOL wide_equal(LPCWSTR left, LPCWSTR right)
{
    while (*left != L'\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

static BOOL append_wchar(WCHAR* buffer, DWORD capacity, DWORD* used, WCHAR value)
{
    if (*used + 1 >= capacity)
        return FALSE;
    buffer[(*used)++] = value;
    buffer[*used]     = L'\0';
    return TRUE;
}

static BOOL append_wtext(WCHAR* buffer, DWORD capacity, DWORD* used, LPCWSTR text)
{
    while (*text != L'\0')
    {
        if (!append_wchar(buffer, capacity, used, *text++))
            return FALSE;
    }
    return TRUE;
}

static BOOL is_absolute_dos_path(LPCWSTR path)
{
    if (path[0] == L'\0' || path[1] == L'\0' || path[2] == L'\0')
        return FALSE;
    WCHAR drive = path[0];
    return ((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z')) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

static BOOL is_absolute_unix_path(LPCWSTR path)
{
    return path[0] == L'/';
}

static BOOL append_quoted_wtext(WCHAR* buffer, DWORD capacity, DWORD* used, LPCWSTR text)
{
    DWORD slashes = 0;
    if (!append_wchar(buffer, capacity, used, L'"'))
        return FALSE;
    while (*text != L'\0')
    {
        if (*text == L'\\')
        {
            ++slashes;
            ++text;
            continue;
        }
        if (*text == L'"')
        {
            DWORD n;
            for (n = 0; n < slashes * 2 + 1; ++n)
                if (!append_wchar(buffer, capacity, used, L'\\'))
                    return FALSE;
            if (!append_wchar(buffer, capacity, used, L'"'))
                return FALSE;
        }
        else
        {
            DWORD n;
            for (n = 0; n < slashes; ++n)
                if (!append_wchar(buffer, capacity, used, L'\\'))
                    return FALSE;
            if (!append_wchar(buffer, capacity, used, *text))
                return FALSE;
        }
        slashes = 0;
        ++text;
    }
    while (slashes != 0)
    {
        if (!append_wchar(buffer, capacity, used, L'\\'))
            return FALSE;
        if (!append_wchar(buffer, capacity, used, L'\\'))
            return FALSE;
        --slashes;
    }
    return append_wchar(buffer, capacity, used, L'"');
}

static BOOL append_u32_decimal(WCHAR* buffer, DWORD capacity, DWORD* used, uint32_t value)
{
    WCHAR digits[10];
    DWORD count = 0;
    do
    {
        digits[count++] = (WCHAR) (L'0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0)
    {
        if (!append_wchar(buffer, capacity, used, digits[--count]))
            return FALSE;
    }
    return TRUE;
}

static BOOL append_u64_hex(WCHAR* buffer, DWORD capacity, DWORD* used, uint64_t value)
{
    static const WCHAR digits[] = L"0123456789abcdef";
    WCHAR              reversed[16];
    DWORD              count = 0;
    if (!append_wtext(buffer, capacity, used, L"0x"))
        return FALSE;
    do
    {
        reversed[count++] = digits[value & 0xfu];
        value >>= 4;
    } while (value != 0);
    while (count != 0)
    {
        if (!append_wchar(buffer, capacity, used, reversed[--count]))
            return FALSE;
    }
    return TRUE;
}

static BOOL make_object_name(DWORD pid, LPCWSTR suffix, WCHAR* name, DWORD capacity)
{
    DWORD used = 0;
    return append_wtext(name, capacity, &used, L"Local\\vkshade-proton-streamline-test-") && append_u32_decimal(name, capacity, &used, pid)
           && append_wtext(name, capacity, &used, suffix);
}

static BOOL finish_pipe_reader(HANDLE reader)
{
    DWORD wait_result = WaitForSingleObject(reader, OBSERVATION_TIMEOUT_MS);
    if (wait_result == WAIT_OBJECT_0)
        return TRUE;
    CancelSynchronousIo(reader);
    wait_result = WaitForSingleObject(reader, 5000);
    if (wait_result == WAIT_OBJECT_0)
        return TRUE;
    if (!TerminateThread(reader, 127))
    {
        wait_result = WaitForSingleObject(reader, 5000);
        if (wait_result == WAIT_OBJECT_0)
            return FALSE;
        ExitProcess(127);
    }
    wait_result = WaitForSingleObject(reader, 5000);
    if (wait_result != WAIT_OBJECT_0)
        ExitProcess(127);
    return FALSE;
}

static DWORD WINAPI drain_pipe(LPVOID parameter)
{
    pipe_capture* capture = (pipe_capture*) parameter;
    char          buffer[512];
    DWORD         read_count;
    for (;;)
    {
        DWORD available;
        if (!ReadFile(capture->handle, buffer, sizeof(buffer), &read_count, NULL) || read_count == 0)
            break;
        available = (DWORD) sizeof(capture->bytes) - capture->length;
        if (read_count > available)
        {
            if (available != 0)
            {
                for (DWORD i = 0; i < available; ++i)
                    capture->bytes[capture->length + i] = buffer[i];
            }
            capture->length += available;
            capture->truncated = TRUE;
        }
        else
        {
            for (DWORD i = 0; i < read_count; ++i)
                capture->bytes[capture->length + i] = buffer[i];
            capture->length += read_count;
        }
    }
    return 0;
}

static void write_injector_outcome(const char* case_name, const injector_outcome* outcome, const injector_shared_state* shared)
{
    static const char hex[] = "0123456789abcdef";
    char              line[8448];
    DWORD             used = 0;
    DWORD             written;
    LONG              callback_count   = InterlockedCompareExchange((volatile LONG*) &shared->callback_count, 0, 0);
    LONG              monitoring_count = InterlockedCompareExchange((volatile LONG*) &shared->monitoring_count, 0, 0);
    LONG              interposer_count = InterlockedCompareExchange((volatile LONG*) &shared->interposer_count, 0, 0);
    const char*       prefix           = "INJECTOR case=";
    while (*prefix != '\0')
        line[used++] = *prefix++;
    while (*case_name != '\0')
        line[used++] = *case_name++;
    prefix = " exit=";
    while (*prefix != '\0')
        line[used++] = *prefix++;
    {
        DWORD value = outcome->exit_code;
        char  digits[10];
        DWORD count = 0;
        do
        {
            digits[count++] = (char) ('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count != 0)
            line[used++] = digits[--count];
    }
    prefix = " stderr_len=";
    while (*prefix != '\0')
        line[used++] = *prefix++;
    {
        DWORD value = outcome->stderr_length;
        char  digits[10];
        DWORD count = 0;
        do
        {
            digits[count++] = (char) ('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count != 0)
            line[used++] = digits[--count];
    }
    prefix = outcome->stderr_truncated ? " stderr_truncated=1 stderr_hex=" : " stderr_truncated=0 stderr_hex=";
    while (*prefix != '\0')
        line[used++] = *prefix++;
    for (DWORD i = 0; i < outcome->stderr_length; ++i)
    {
        unsigned char byte = (unsigned char) outcome->stderr_bytes[i];
        line[used++]       = hex[byte >> 4];
        line[used++]       = hex[byte & 0xfu];
    }
    prefix = " callbacks=";
    while (*prefix != '\0')
        line[used++] = *prefix++;
    {
        DWORD value = (DWORD) callback_count;
        char  digits[10];
        DWORD count = 0;
        do
        {
            digits[count++] = (char) ('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count != 0)
            line[used++] = digits[--count];
    }
    prefix = " monitoring=";
    while (*prefix != '\0')
        line[used++] = *prefix++;
    {
        DWORD value = (DWORD) monitoring_count;
        char  digits[10];
        DWORD count = 0;
        do
        {
            digits[count++] = (char) ('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count != 0)
            line[used++] = digits[--count];
    }
    prefix = " interposer=";
    while (*prefix != '\0')
        line[used++] = *prefix++;
    {
        DWORD value = (DWORD) interposer_count;
        char  digits[10];
        DWORD count = 0;
        do
        {
            digits[count++] = (char) ('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count != 0)
            line[used++] = digits[--count];
    }
    line[used++] = '\r';
    line[used++] = '\n';
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, used, &written, NULL);
}

static BOOL
run_injector(LPCWSTR application_path, LPCWSTR observer_path, uint32_t pid, uintptr_t identity_address, uint64_t cookie, injector_outcome* outcome)
{
    WCHAR               command_line[COMMAND_LINE_CAPACITY] = {0};
    DWORD               used                                = 0;
    STARTUPINFOW        startup                             = {0};
    PROCESS_INFORMATION process                             = {0};
    SECURITY_ATTRIBUTES security                            = {sizeof(security), NULL, TRUE};
    pipe_capture        capture                             = {0};
    HANDLE              pipe_write                          = NULL;
    HANDLE              null_input                          = INVALID_HANDLE_VALUE;
    HANDLE              null_output                         = INVALID_HANDLE_VALUE;
    HANDLE              reader                              = NULL;
    DWORD               wait_result;
    BOOL                success = FALSE;
    outcome->exit_code          = 0;
    outcome->stderr_length      = 0;
    outcome->stderr_truncated   = FALSE;
    if (!append_quoted_wtext(command_line, COMMAND_LINE_CAPACITY, &used, application_path)
        || !append_wchar(command_line, COMMAND_LINE_CAPACITY, &used, L' ') || !append_u32_decimal(command_line, COMMAND_LINE_CAPACITY, &used, pid)
        || !append_wchar(command_line, COMMAND_LINE_CAPACITY, &used, L' ')
        || !append_u64_hex(command_line, COMMAND_LINE_CAPACITY, &used, identity_address)
        || !append_wchar(command_line, COMMAND_LINE_CAPACITY, &used, L' ') || !append_u64_hex(command_line, COMMAND_LINE_CAPACITY, &used, cookie)
        || !append_wchar(command_line, COMMAND_LINE_CAPACITY, &used, L' ')
        || !append_quoted_wtext(command_line, COMMAND_LINE_CAPACITY, &used, observer_path))
        return FALSE;
    if (!CreatePipe(&capture.handle, &pipe_write, &security, 0) || !SetHandleInformation(capture.handle, HANDLE_FLAG_INHERIT, 0))
        goto cleanup;
    null_input  = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    null_output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (null_input == INVALID_HANDLE_VALUE || null_output == INVALID_HANDLE_VALUE)
        goto cleanup;
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = null_input;
    startup.hStdOutput = null_output;
    startup.hStdError  = pipe_write;
    if (!CreateProcessW(application_path, command_line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process))
        goto cleanup;
    CloseHandle(pipe_write);
    pipe_write = NULL;
    CloseHandle(null_input);
    null_input = INVALID_HANDLE_VALUE;
    CloseHandle(null_output);
    null_output = INVALID_HANDLE_VALUE;
    reader      = CreateThread(NULL, 0, drain_pipe, &capture, 0, NULL);
    if (reader == NULL)
    {
        TerminateProcess(process.hProcess, 125);
        if (WaitForSingleObject(process.hProcess, OBSERVATION_TIMEOUT_MS) != WAIT_OBJECT_0)
            ExitProcess(127);
        goto cleanup;
    }
    wait_result = WaitForSingleObject(process.hProcess, 30000);
    if (wait_result != WAIT_OBJECT_0)
    {
        TerminateProcess(process.hProcess, 125);
        if (WaitForSingleObject(process.hProcess, OBSERVATION_TIMEOUT_MS) != WAIT_OBJECT_0)
            ExitProcess(127);
        goto cleanup;
    }
    if (!GetExitCodeProcess(process.hProcess, &outcome->exit_code))
        goto cleanup;
    success = TRUE;

cleanup:
    if (process.hThread != NULL)
        CloseHandle(process.hThread);
    if (process.hProcess != NULL)
        CloseHandle(process.hProcess);
    if (reader != NULL)
    {
        if (!finish_pipe_reader(reader))
            success = FALSE;
        CloseHandle(reader);
    }
    if (capture.handle != NULL)
        CloseHandle(capture.handle);
    if (pipe_write != NULL)
        CloseHandle(pipe_write);
    if (null_input != INVALID_HANDLE_VALUE)
        CloseHandle(null_input);
    if (null_output != INVALID_HANDLE_VALUE)
        CloseHandle(null_output);
    if (success)
    {
        outcome->stderr_length    = capture.length;
        outcome->stderr_truncated = capture.truncated;
        for (DWORD i = 0; i < capture.length; ++i)
            outcome->stderr_bytes[i] = capture.bytes[i];
    }
    return success;
}

static BOOL load_fixture(void)
{
    g_fixture = LoadLibraryW(L"sl.interposer.dll");
    if (g_fixture == NULL)
        return FALSE;
    g_get_snapshot         = (get_snapshot_fn) GetProcAddress(g_fixture, "slTestGetSnapshot");
    g_set_results          = (set_results_fn) GetProcAddress(g_fixture, "slTestSetResults");
    g_init                 = (init_fn) GetProcAddress(g_fixture, "slInit");
    g_shutdown             = (shutdown_fn) GetProcAddress(g_fixture, "slShutdown");
    g_evaluate             = (evaluate_fn) GetProcAddress(g_fixture, "slEvaluateFeature");
    g_set_tag              = (set_tag_fn) GetProcAddress(g_fixture, "slSetTag");
    g_set_tag_for_frame    = (set_tag_for_frame_fn) GetProcAddress(g_fixture, "slSetTagForFrame");
    g_get_feature_function = (get_feature_function_fn) GetProcAddress(g_fixture, "slGetFeatureFunction");
    g_expected_options     = (expected_options_fn) GetProcAddress(g_fixture, "slTestExpectedSetOptions");
    return g_get_snapshot != NULL && g_set_results != NULL && g_init != NULL && g_shutdown != NULL && g_evaluate != NULL && g_set_tag != NULL
           && g_set_tag_for_frame != NULL && g_get_feature_function != NULL && g_expected_options != NULL;
}

static DWORD WINAPI evaluate_worker(void* argument)
{
    eval_thread_context* context = (eval_thread_context*) argument;
    uint32_t             i;
    for (i = 0; i < 8; ++i)
    {
        if (context->evaluate(context->feature, &g_frame_token, NULL, 0, &g_command_buffer) != 0)
            InterlockedExchange(&context->failed, 1);
    }
    return 0;
}

static int run_inject_target(LPCWSTR mapping_name, LPCWSTR ready_name, LPCWSTR stop_name)
{
    HANDLE                         mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name);
    HANDLE                         ready;
    HANDLE                         stop;
    DWORD                          wait_result;
    int                            exit_code = 1;
    static vkshade_proton_identity identity;
    if (mapping == NULL)
        return 1;
    g_injector_shared = (injector_shared_state*) MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(injector_shared_state));
    if (g_injector_shared == NULL)
    {
        CloseHandle(mapping);
        return 1;
    }
    ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name);
    stop  = OpenEventW(SYNCHRONIZE, FALSE, stop_name);
    if (ready == NULL || stop == NULL || !load_fixture())
        goto cleanup;
    InitializeCriticalSection(&g_event_lock);
    identity.cookie                     = 0x564b534841444501ull;
    identity.callback_address           = (uint64_t) (uintptr_t) &observe_bridge_event;
    g_injector_shared->target_pid       = GetCurrentProcessId();
    g_injector_shared->identity_address = (uintptr_t) &identity;
    g_injector_shared->cookie           = identity.cookie;
    if (!SetEvent(ready))
    {
        DeleteCriticalSection(&g_event_lock);
        goto cleanup;
    }
    wait_result = WaitForSingleObject(stop, 120000);
    if (wait_result == WAIT_OBJECT_0)
        return 0;
    return exit_code;

cleanup:
    if (ready != NULL)
        CloseHandle(ready);
    if (stop != NULL)
        CloseHandle(stop);
    UnmapViewOfFile(g_injector_shared);
    g_injector_shared = NULL;
    CloseHandle(mapping);
    return exit_code;
}

static int run_injector_case(LPCWSTR injector_path, LPCWSTR observer_path)
{
    HANDLE                 mapping = NULL;
    HANDLE                 ready   = NULL;
    HANDLE                 stop    = NULL;
    PROCESS_INFORMATION    target  = {0};
    STARTUPINFOW           startup = {0};
    injector_shared_state* shared  = NULL;
    WCHAR                  self_path[COMMAND_LINE_CAPACITY];
    WCHAR                  target_command[COMMAND_LINE_CAPACITY] = {0};
    WCHAR                  mapping_name[128]                     = {0};
    WCHAR                  ready_name[128]                       = {0};
    WCHAR                  stop_name[128]                        = {0};
    DWORD                  self_length;
    DWORD                  used = 0;
    injector_outcome       injector_result;
    DWORD                  wait_result;
    DWORD                  target_exit_code = 0;
    uint32_t               i;
    const char*            failure        = NULL;
    BOOL                   target_started = FALSE;
    int                    result         = 1;

    if (!is_absolute_dos_path(injector_path) || !is_absolute_unix_path(observer_path))
    {
        failure = "injector path must be absolute DOS; observer path must be absolute Unix";
        goto cleanup;
    }
    if (!make_object_name(GetCurrentProcessId(), L"-map", mapping_name, 128) || !make_object_name(GetCurrentProcessId(), L"-ready", ready_name, 128)
        || !make_object_name(GetCurrentProcessId(), L"-stop", stop_name, 128))
    {
        failure = "make unique injector synchronization names";
        goto cleanup;
    }
    SetLastError(ERROR_SUCCESS);
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD) sizeof(injector_shared_state), mapping_name);
    if (mapping == NULL || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        failure = "create fresh injector shared mapping";
        goto cleanup;
    }
    shared = (injector_shared_state*) MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(injector_shared_state));
    if (shared == NULL)
    {
        failure = "map injector shared state";
        goto cleanup;
    }
    SetLastError(ERROR_SUCCESS);
    ready = CreateEventW(NULL, TRUE, FALSE, ready_name);
    if (ready == NULL || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        failure = "create fresh injector readiness event";
        goto cleanup;
    }
    SetLastError(ERROR_SUCCESS);
    stop = CreateEventW(NULL, TRUE, FALSE, stop_name);
    if (stop == NULL || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        failure = "create fresh injector synchronization events";
        goto cleanup;
    }
    self_length = GetModuleFileNameW(NULL, self_path, COMMAND_LINE_CAPACITY);
    if (self_length == 0 || self_length >= COMMAND_LINE_CAPACITY || !append_quoted_wtext(target_command, COMMAND_LINE_CAPACITY, &used, self_path)
        || !append_wtext(target_command, COMMAND_LINE_CAPACITY, &used, L" --inject-target ")
        || !append_quoted_wtext(target_command, COMMAND_LINE_CAPACITY, &used, mapping_name)
        || !append_wchar(target_command, COMMAND_LINE_CAPACITY, &used, L' ')
        || !append_quoted_wtext(target_command, COMMAND_LINE_CAPACITY, &used, ready_name)
        || !append_wchar(target_command, COMMAND_LINE_CAPACITY, &used, L' ')
        || !append_quoted_wtext(target_command, COMMAND_LINE_CAPACITY, &used, stop_name))
    {
        failure = "prepare target process command line";
        goto cleanup;
    }
    startup.cb = sizeof(startup);
    if (!CreateProcessW(self_path, target_command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &target))
    {
        failure = "start isolated target process";
        goto cleanup;
    }
    target_started = TRUE;
    if (WaitForSingleObject(ready, 30000) != WAIT_OBJECT_0)
    {
        failure = "target did not publish live identity";
        goto cleanup;
    }
    if (shared->target_pid != target.dwProcessId || shared->identity_address == 0 || shared->cookie == 0)
    {
        failure = "target identity publication does not match live process";
        goto cleanup;
    }

    if (!run_injector(injector_path, observer_path, shared->target_pid, shared->identity_address, shared->cookie ^ 1u, &injector_result))
    {
        failure = "wrong-cookie injector invocation did not finish";
        goto cleanup;
    }
    for (i = 0; i < 100; ++i)
        Sleep(10);
    write_injector_outcome("wrong-cookie", &injector_result, shared);
    if (injector_result.exit_code == 0)
    {
        failure = "wrong-cookie injector request unexpectedly succeeded";
        goto cleanup;
    }
    if (InterlockedCompareExchange(&shared->callback_count, 0, 0) != 0)
    {
        failure = "wrong-cookie request caused target observer callback";
        goto cleanup;
    }

    if (!run_injector(injector_path, observer_path, 0xffffffffu, shared->identity_address, shared->cookie, &injector_result))
    {
        failure = "invalid-target injector invocation did not finish";
        goto cleanup;
    }
    for (i = 0; i < 100; ++i)
        Sleep(10);
    write_injector_outcome("invalid-target", &injector_result, shared);
    if (injector_result.exit_code == 0)
    {
        failure = "invalid-target injector request unexpectedly succeeded";
        goto cleanup;
    }
    if (InterlockedCompareExchange(&shared->callback_count, 0, 0) != 0)
    {
        failure = "invalid-target request caused target observer callback";
        goto cleanup;
    }

    if (!run_injector(injector_path, observer_path, shared->target_pid, shared->identity_address, shared->cookie, &injector_result))
    {
        failure = "valid-target injector invocation did not finish";
        goto cleanup;
    }
    write_injector_outcome("valid-target", &injector_result, shared);
    if (injector_result.exit_code != 0)
    {
        failure = "valid-target injector request returned failure";
        goto cleanup;
    }
    {
        ULONGLONG deadline = GetTickCount64() + OBSERVATION_TIMEOUT_MS;
        while (GetTickCount64() < deadline)
        {
            if (InterlockedCompareExchange(&shared->monitoring_count, 0, 0) != 0 && InterlockedCompareExchange(&shared->interposer_count, 0, 0) != 0)
                break;
            Sleep(10);
        }
    }
    if (InterlockedCompareExchange(&shared->monitoring_count, 0, 0) == 0 || InterlockedCompareExchange(&shared->interposer_count, 0, 0) == 0)
    {
        write_injector_outcome("valid-target-final", &injector_result, shared);
        failure = "valid injector did not initialize target observer callbacks";
        goto cleanup;
    }
    write_injector_outcome("valid-target-final", &injector_result, shared);
    result = 0;

cleanup:
    if (stop != NULL)
        SetEvent(stop);
    if (target_started)
    {
        wait_result = WaitForSingleObject(target.hProcess, 5000);
        if (wait_result != WAIT_OBJECT_0)
        {
            TerminateProcess(target.hProcess, 126);
            if (WaitForSingleObject(target.hProcess, OBSERVATION_TIMEOUT_MS) != WAIT_OBJECT_0)
                ExitProcess(127);
            if (failure == NULL)
                failure = "stop isolated target process";
            result = 1;
        }
        else if (!GetExitCodeProcess(target.hProcess, &target_exit_code) || target_exit_code != 0)
        {
            if (failure == NULL)
                failure = "isolated target did not exit successfully";
            result = 1;
        }
        CloseHandle(target.hThread);
        CloseHandle(target.hProcess);
    }
    if (shared != NULL)
        UnmapViewOfFile(shared);
    if (stop != NULL)
        CloseHandle(stop);
    if (ready != NULL)
        CloseHandle(ready);
    if (mapping != NULL)
        CloseHandle(mapping);
    if (failure != NULL)
        return fail(failure);
    write_text("PASS: isolated PE injector contract\r\n");
    return result;
}

static int run_case(BOOL preload, BOOL stress_metadata)
{
    HMODULE                 observer;
    initialize_fn           initialize;
    vkshade_proton_identity identity;
    sl_snapshot             snapshot;
    sl_base*                inputs[1];
    eval_call               eval;
    set_tag_call            tag_call;
    set_tag_for_frame_call  frame_tag_call;
    options_call            option_call;
    sl_dlssg_options        options;
    sl_resource_tag         removal_tag;
    sl_resource_tag         error_tag;
    sl_resource_tag         chain_heads[2];
    sl_resource_tag         chain_nodes[5];
    sl_precision_info       precision_info;
    sl_base                 chain_extension;
    sl_resource_tag         boundary_heads[2];
    sl_base                 boundary_extensions[2][TAG_BOUNDARY_EXTENSIONS];
    sl_resource_tag         boundary_tails[2];
    sl_resource_tag         cycle_head;
    sl_resource_tag         cycle_linked;
    sl_base                 cycle_extension;
    sl_base                 invalid_root;
    sl_base                 cyclic_root;
    sl_resource_tag         cyclic_tag;
    sl_snapshot             before;
    sl_snapshot             after;
    uint64_t                generation          = 0;
    uint64_t                shutdown_generation = 0;
    uint64_t                pre_shutdown_generation;
    uint64_t                pre_failed_shutdown_generation;
    uint64_t                post_failed_shutdown_generation = 0;
    uint64_t                chain_resource_bits             = RESOURCE_MASK(0) | RESOURCE_MASK(1) | RESOURCE_MASK(5);
    uint64_t                cycle_resource_bits             = RESOURCE_MASK(0) | RESOURCE_MASK(1);
    uint64_t                boundary_head_bits              = RESOURCE_MASK(0) | RESOURCE_MASK(1);
    uint64_t                boundary_tail_bits              = RESOURCE_MASK(5) | RESOURCE_MASK(6);
    LONG                    attempts;
    LONG                    event_start;
    BOOL                    failed_shutdown_notified;
    int32_t                 result;
    void*                   resolved = NULL;
    uint32_t                mode;
    LONG                    expected_eval_calls      = 0;
    LONG                    expected_set_tag_calls   = 0;
    LONG                    expected_frame_tag_calls = 0;
    LONG                    expected_options_calls   = 0;

    InitializeCriticalSection(&g_event_lock);
    if (preload && !load_fixture())
        return fail("preload sl.interposer.dll");

    observer = LoadLibraryW(L"vkshade_streamline_observer.dll");
    CHECK(observer != NULL, "load observer PE");
    initialize = (initialize_fn) GetProcAddress(observer, "vkshade_companion_initialize_v1");
    CHECK(initialize != NULL, "resolve companion initializer");

    identity.cookie           = 0x564b534841444501ull;
    identity.callback_address = (uint64_t) (uintptr_t) &observe_bridge_event;
    result                    = (int32_t) initialize(&identity);
    CHECK(result == 0, "initialize valid identity returns zero");
    CHECK(wait_event(0, VKSHADE_BRIDGE_MONITORING, 0, FALSE, NULL), "MONITORING arrives asynchronously");

    if (!preload)
        CHECK(load_fixture(), "load sl.interposer.dll after monitoring");
    CHECK(wait_event(0, VKSHADE_BRIDGE_INTERPOSER_PRESENT, 0, FALSE, NULL), "detect already-loaded or later-loaded interposer");

    if (g_fixture == NULL)
        CHECK(load_fixture(), "load fixture once");
    get_snapshot(&snapshot);
    CHECK(snapshot.init_calls == 0 && snapshot.shutdown_calls == 0 && snapshot.evaluate_calls == 0,
          "observer setup does not call game Streamline functions");
    CHECK(snapshot.get_feature_function_calls == 0 && snapshot.get_state_calls == 0, "observer setup does not query game feature functions or state");

    g_set_results(0, 0, 0, 0, 0);
    set_runtime_objects();
    result = g_init(&g_preferences, 0x1122334455667788ull);
    CHECK(result == 0, "slInit returns original success result");
    get_snapshot(&snapshot);
    CHECK(snapshot.init_calls == 1 && snapshot.last_sdk_version == 0x1122334455667788ull && snapshot.last_preferences == (uintptr_t) &g_preferences,
          "slInit preserves preferences pointer and full 64-bit SDK version");

    set_resource(&g_resources[0], (void*) (uintptr_t) 0x11110000u);
    set_resource(&g_resources[1], (void*) (uintptr_t) 0x22220000u);
    set_resource(&g_resources[2], (void*) (uintptr_t) 0x33330000u);
    set_resource(&g_resources[3], (void*) (uintptr_t) 0x44440000u);
    set_resource(&g_resources[4], (void*) (uintptr_t) 0x55550000u);
    set_resource(&g_resources[5], (void*) (uintptr_t) 0x66660000u);
    set_resource(&g_resources[6], (void*) (uintptr_t) 0x77770000u);
    set_resource(&g_resources[7], (void*) (uintptr_t) 0x88880000u);

    set_tag(&chain_heads[0], &g_resources[0], 0, 1);
    set_tag(&chain_heads[1], NULL, 6, 1);
    set_tag(&chain_nodes[0], &g_resources[1], 1, 1);
    set_tag(&chain_nodes[1], &g_resources[2], 5, 1);
    set_tag(&chain_nodes[2], NULL, 2, 1);
    set_tag(&chain_nodes[3], &g_resources[3], 20, 1);
    chain_nodes[3].base.type.data1 = 0xdeadbeef;
    set_tag(&chain_nodes[4], &g_resources[4], 21, 2);
    set_base(&precision_info.base, 0x98f6e9ba, 0x8d16, 0x4831, 1);
    {
        static const uint8_t precision_tail[8] = {0xa8, 0x02, 0x4d, 0x3b, 0x52, 0xff, 0x26, 0xbf};
        set_guid_tail(&precision_info.base.type, precision_tail);
    }
    precision_info.conversion_formula = 1;
    precision_info.bias               = 0.0f;
    precision_info.scale              = 1.0f;
    set_base(&chain_extension, 0xe17e0001, 0x1212, 0x3434, 1);
    chain_heads[0].base.next = &precision_info.base;
    precision_info.base.next = &chain_nodes[0].base;
    chain_nodes[0].base.next = &chain_extension;
    chain_extension.next     = &chain_nodes[2].base;
    chain_nodes[2].base.next = &chain_nodes[3].base;
    chain_nodes[3].base.next = &chain_nodes[4].base;
    chain_nodes[4].base.next = &chain_nodes[1].base;

    set_base(&g_input_root, 0x12345678, 0x1234, 0x5678, 1);
    set_tag(&g_tags[0], &g_resources[0], 0, 1);
    set_tag(&g_tags[1], &g_resources[1], 1, 1);
    set_tag(&g_tags[2], NULL, 2, 1);
    set_tag(&g_tags[3], &g_resources[2], 20, 1);
    g_tags[3].base.type.data1 = 0xdeadbeef;
    set_tag(&g_tags[4], &g_resources[3], 21, 2);
    g_input_root.next   = &g_tags[0].base;
    g_tags[0].base.next = &g_tags[1].base;
    g_tags[1].base.next = &g_tags[2].base;
    g_tags[2].base.next = &g_tags[3].base;
    g_tags[3].base.next = &g_tags[4].base;
    inputs[0]           = &g_input_root;
    eval.function       = g_evaluate;
    eval.feature        = FEATURE_DLSS_G;
    eval.frame          = &g_frame_token;
    eval.inputs         = (const sl_base* const*) inputs;
    eval.count          = 1;
    eval.command_buffer = &g_command_buffer;
    event_start         = event_count();
    CHECK(drive_until(attempt_eval, &eval, COUNTER_EVALUATE, 0, event_start, VKSHADE_BRIDGE_FEATURE_EVALUATED, FEATURE_DLSS_G, TRUE, &attempts),
          "successful slEvaluateFeature is observed");
    expected_eval_calls += attempts;
    CHECK(wait_event(event_start, VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, 3, TRUE, &generation),
          "ResourceTag chain reports only compatible nonnull types 0 and 1");
    CHECK(no_tag_bits_outside_after(event_start, RESOURCE_MASK(0) | RESOURCE_MASK(1)),
          "null and unsupported ResourceTags contribute no resource type bits");
    CHECK(find_event_since(event_start, VKSHADE_BRIDGE_FEATURE_EVALUATED, FEATURE_DLSS_G, TRUE, &generation) && generation != 0,
          "feature event carries exact ID and nonzero source generation");
    get_snapshot(&snapshot);
    CHECK(snapshot.last_feature == FEATURE_DLSS_G && snapshot.last_frame == (uintptr_t) &g_frame_token && snapshot.last_inputs == (uintptr_t) inputs
              && snapshot.last_count == 1 && snapshot.last_command_buffer == (uintptr_t) &g_command_buffer,
          "slEvaluateFeature arguments reach original unchanged");

    if (stress_metadata)
    {
        inputs[0]    = &g_input_root;
        eval.feature = 4001;
        eval.inputs  = (const sl_base* const*) inputs;
        eval.count   = UINT32_MAX;
        Sleep(150);
        event_start = event_count();
        write_text("CHECK: excessive nonnull input start\r\n");
        get_snapshot(&before);
        CHECK(invoke_bounded(attempt_eval, &eval, 0), "excessive nonnull input count inspection stays bounded");
        write_text("CHECK: excessive nonnull input done\r\n");
        get_snapshot(&after);
        ++expected_eval_calls;
        CHECK(after.evaluate_calls == before.evaluate_calls + 1 && after.last_inputs == (uintptr_t) inputs && after.last_count == UINT32_MAX
                  && after.last_feature == 4001,
              "excessive nonnull input count reaches original unchanged");
        CHECK(wait_event(event_start, VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, FALSE, NULL),
              "successful excessive nonnull evaluation reports limited coverage");
        CHECK(no_tag_bits_outside_after(event_start, 0), "excessive evaluation does not report inspected input tags");

        set_base(&cyclic_root, 0x12345678, 0x1234, 0x5678, 1);
        set_tag(&cyclic_tag, &g_resources[0], 14, 1);
        cyclic_root.next     = &cyclic_tag.base;
        cyclic_tag.base.next = &cyclic_root;
        inputs[0]            = &cyclic_root;
        eval.feature         = 4000;
        eval.count           = 1;
        write_text("CHECK: cyclic metadata start\r\n");
        get_snapshot(&before);
        CHECK(invoke_bounded(attempt_eval, &eval, 0), "cyclic metadata inspection stays bounded");
        write_text("CHECK: cyclic metadata done\r\n");
        get_snapshot(&after);
        ++expected_eval_calls;
        CHECK(after.evaluate_calls == before.evaluate_calls + 1 && after.last_inputs == (uintptr_t) inputs && after.last_count == 1
                  && after.last_feature == 4000,
              "cyclic metadata call preserves original arguments and invocation");

        eval.feature = 4002;
        eval.inputs  = NULL;
        eval.count   = UINT32_MAX;
        write_text("CHECK: excessive input start\r\n");
        get_snapshot(&before);
        CHECK(invoke_bounded(attempt_eval, &eval, 0), "excessive input count inspection stays bounded");
        write_text("CHECK: excessive input done\r\n");
        get_snapshot(&after);
        ++expected_eval_calls;
        CHECK(after.evaluate_calls == before.evaluate_calls + 1 && after.last_inputs == 0 && after.last_count == UINT32_MAX
                  && after.last_feature == 4002,
              "excessive input count reaches original unchanged");
        eval.feature = FEATURE_DLSS_G;
        eval.inputs  = (const sl_base* const*) inputs;
        eval.count   = 1;
    }

    set_tag(&g_tags[5], &g_resources[4], 5, 1);
    set_tag(&g_tags[6], &g_resources[5], 6, 1);
    tag_call.function       = g_set_tag;
    tag_call.viewport       = &g_viewport;
    tag_call.tags           = &g_tags[5];
    tag_call.count          = 2;
    tag_call.command_buffer = &g_command_buffer;
    event_start             = event_count();
    CHECK(drive_until(attempt_set_tag,
                      &tag_call,
                      COUNTER_SET_TAG,
                      0,
                      event_start,
                      VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN,
                      RESOURCE_MASK(5) | RESOURCE_MASK(6),
                      TRUE,
                      &attempts),
          "successful slSetTag reports native resource types");
    expected_set_tag_calls += attempts;
    CHECK(wait_event(event_start, TEST_BRIDGE_TAG_SUBMISSION, tag_submission_payload(0, 2), TRUE, &generation) && generation == 0,
          "ordinary nonnull slSetTag reports exact successful result and array count");
    get_snapshot(&snapshot);
    CHECK(snapshot.last_viewport == (uintptr_t) &g_viewport && snapshot.last_tags == (uintptr_t) &g_tags[5] && snapshot.last_count == 2
              && snapshot.last_command_buffer == (uintptr_t) &g_command_buffer,
          "slSetTag arguments reach original unchanged");

    if (stress_metadata)
    {
        tag_call.count = UINT32_MAX;
        Sleep(150);
        event_start = event_count();
        write_text("CHECK: excessive slSetTag count start\r\n");
        get_snapshot(&before);
        CHECK(invoke_bounded(attempt_set_tag, &tag_call, 0), "excessive slSetTag count inspection stays bounded");
        write_text("CHECK: excessive slSetTag count done\r\n");
        get_snapshot(&after);
        ++expected_set_tag_calls;
        CHECK(after.set_tag_calls == before.set_tag_calls + 1 && after.last_tags == (uintptr_t) &g_tags[5] && after.last_count == UINT32_MAX,
              "excessive slSetTag count reaches original unchanged");
        CHECK(wait_event(event_start, VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, FALSE, NULL), "excessive slSetTag count reports limited coverage");
        CHECK(wait_single_tag_submission(event_start, 0, UINT32_MAX), "slSetTag submission reports exact successful result and count");
        tag_call.count = 2;
    }

    set_tag(&g_tags[7], &g_resources[6], 7, 1);
    frame_tag_call.function       = g_set_tag_for_frame;
    frame_tag_call.frame          = &g_frame_token;
    frame_tag_call.viewport       = &g_viewport;
    frame_tag_call.tags           = &g_tags[7];
    frame_tag_call.count          = 1;
    frame_tag_call.command_buffer = &g_command_buffer;
    event_start                   = event_count();
    CHECK(drive_until(attempt_set_tag_for_frame,
                      &frame_tag_call,
                      COUNTER_SET_TAG_FOR_FRAME,
                      0,
                      event_start,
                      VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN,
                      RESOURCE_MASK(7),
                      TRUE,
                      &attempts),
          "successful slSetTagForFrame reports native resource type");
    expected_frame_tag_calls += attempts;
    CHECK(wait_event(event_start, TEST_BRIDGE_TAG_SUBMISSION, tag_submission_payload(0, 1), TRUE, &generation) && generation == 0,
          "ordinary nonnull slSetTagForFrame reports exact successful result and array count");
    get_snapshot(&snapshot);
    CHECK(snapshot.last_frame == (uintptr_t) &g_frame_token && snapshot.last_viewport == (uintptr_t) &g_viewport
              && snapshot.last_tags == (uintptr_t) &g_tags[7] && snapshot.last_count == 1
              && snapshot.last_command_buffer == (uintptr_t) &g_command_buffer,
          "slSetTagForFrame arguments reach original unchanged");

    if (stress_metadata)
    {
        frame_tag_call.count = UINT32_MAX;
        Sleep(150);
        event_start = event_count();
        write_text("CHECK: excessive tag start\r\n");
        get_snapshot(&before);
        CHECK(invoke_bounded(attempt_set_tag_for_frame, &frame_tag_call, 0), "excessive tag count inspection stays bounded");
        write_text("CHECK: excessive tag done\r\n");
        get_snapshot(&after);
        ++expected_frame_tag_calls;
        CHECK(after.set_tag_for_frame_calls == before.set_tag_for_frame_calls + 1 && after.last_tags == (uintptr_t) &g_tags[7]
                  && after.last_count == UINT32_MAX,
              "excessive tag count reaches original unchanged");
        CHECK(wait_event(event_start, VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, FALSE, NULL), "excessive slSetTagForFrame count reports limited coverage");
        CHECK(wait_single_tag_submission(event_start, 0, UINT32_MAX), "slSetTagForFrame submission reports exact successful result and count");
        frame_tag_call.count = 1;
    }

    tag_call.tags  = chain_heads;
    tag_call.count = 2;
    Sleep(150);
    event_start = event_count();
    get_snapshot(&before);
    CHECK(invoke_bounded(attempt_set_tag, &tag_call, 0), "chained slSetTag completes successfully");
    get_snapshot(&after);
    ++expected_set_tag_calls;
    CHECK(after.set_tag_calls == before.set_tag_calls + 1 && after.last_viewport == (uintptr_t) &g_viewport
              && after.last_tags == (uintptr_t) chain_heads && after.last_count == 2 && after.last_command_buffer == (uintptr_t) &g_command_buffer,
          "chained slSetTag preserves original arguments and invocation");
    CHECK(wait_tag_bits(event_start, chain_resource_bits), "slSetTag observes depth, motion, and normals through extension chains");
    CHECK(no_tag_bits_outside_after(event_start, chain_resource_bits), "slSetTag null and unsupported chain nodes contribute no resource bits");
    CHECK(wait_single_tag_submission(event_start, 0, 2), "slSetTag submission reports exact successful result and array count");

    frame_tag_call.tags  = chain_heads;
    frame_tag_call.count = 2;
    Sleep(150);
    event_start = event_count();
    get_snapshot(&before);
    CHECK(invoke_bounded(attempt_set_tag_for_frame, &frame_tag_call, 0), "chained slSetTagForFrame completes successfully");
    get_snapshot(&after);
    ++expected_frame_tag_calls;
    CHECK(after.set_tag_for_frame_calls == before.set_tag_for_frame_calls + 1 && after.last_frame == (uintptr_t) &g_frame_token
              && after.last_viewport == (uintptr_t) &g_viewport && after.last_tags == (uintptr_t) chain_heads && after.last_count == 2
              && after.last_command_buffer == (uintptr_t) &g_command_buffer,
          "chained slSetTagForFrame preserves original arguments and invocation");
    CHECK(wait_tag_bits(event_start, chain_resource_bits), "slSetTagForFrame observes depth, motion, and normals through extension chains");
    CHECK(no_tag_bits_outside_after(event_start, chain_resource_bits),
          "slSetTagForFrame null and unsupported chain nodes contribute no resource bits");
    CHECK(wait_single_tag_submission(event_start, 0, 2), "slSetTagForFrame submission reports exact successful result and array count");

    if (stress_metadata)
    {
        set_tag(&cycle_head, &g_resources[0], 0, 1);
        set_tag(&cycle_linked, &g_resources[1], 1, 1);
        set_base(&cycle_extension, 0xe17e0002, 0x5656, 0x7878, 1);
        cycle_head.base.next   = &cycle_extension;
        cycle_extension.next   = &cycle_linked.base;
        cycle_linked.base.next = &cycle_head.base;

        tag_call.tags  = &cycle_head;
        tag_call.count = 1;
        Sleep(150);
        event_start = event_count();
        get_snapshot(&before);
        CHECK(invoke_bounded(attempt_set_tag, &tag_call, 0), "cyclic slSetTag chain completes successfully");
        get_snapshot(&after);
        ++expected_set_tag_calls;
        CHECK(after.set_tag_calls == before.set_tag_calls + 1 && after.last_tags == (uintptr_t) &cycle_head && after.last_count == 1
                  && after.last_viewport == (uintptr_t) &g_viewport && after.last_command_buffer == (uintptr_t) &g_command_buffer,
              "cyclic slSetTag chain preserves original arguments and invocation");
        CHECK(wait_event(event_start, VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, FALSE, NULL), "cyclic slSetTag chain reports limited coverage");
        CHECK(no_tag_bits_outside_after(event_start, cycle_resource_bits), "cyclic slSetTag chain reports no unsupported resource bits");
        CHECK(wait_single_tag_submission(event_start, 0, 1), "cyclic slSetTag submission reports exact successful result and array count");

        frame_tag_call.tags  = &cycle_head;
        frame_tag_call.count = 1;
        Sleep(150);
        event_start = event_count();
        get_snapshot(&before);
        CHECK(invoke_bounded(attempt_set_tag_for_frame, &frame_tag_call, 0), "cyclic slSetTagForFrame chain completes successfully");
        get_snapshot(&after);
        ++expected_frame_tag_calls;
        CHECK(after.set_tag_for_frame_calls == before.set_tag_for_frame_calls + 1 && after.last_frame == (uintptr_t) &g_frame_token
                  && after.last_viewport == (uintptr_t) &g_viewport && after.last_tags == (uintptr_t) &cycle_head && after.last_count == 1
                  && after.last_command_buffer == (uintptr_t) &g_command_buffer,
              "cyclic slSetTagForFrame chain preserves original arguments and invocation");
        CHECK(wait_event(event_start, VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, FALSE, NULL), "cyclic slSetTagForFrame chain reports limited coverage");
        CHECK(no_tag_bits_outside_after(event_start, cycle_resource_bits), "cyclic slSetTagForFrame chain reports no unsupported resource bits");
        CHECK(wait_single_tag_submission(event_start, 0, 1), "cyclic slSetTagForFrame submission reports exact successful result and array count");

        set_tag(&boundary_heads[0], &g_resources[0], 0, 1);
        set_tag(&boundary_heads[1], &g_resources[1], 1, 1);
        set_tag(&boundary_tails[0], &g_resources[2], 5, 1);
        set_tag(&boundary_tails[1], &g_resources[3], 6, 1);
        for (uint32_t chain_index = 0; chain_index < 2; ++chain_index)
        {
            for (uint32_t node_index = 0; node_index < TAG_BOUNDARY_EXTENSIONS; ++node_index)
            {
                set_base(&boundary_extensions[chain_index][node_index], 0xe17e0100u + chain_index, (uint16_t) (0x1000u + chain_index), 0x3434, 1);
                if (node_index + 1 < TAG_BOUNDARY_EXTENSIONS)
                    boundary_extensions[chain_index][node_index].next = &boundary_extensions[chain_index][node_index + 1];
                else
                    boundary_extensions[chain_index][node_index].next = &boundary_tails[chain_index].base;
            }
            boundary_heads[chain_index].base.next = &boundary_extensions[chain_index][0];
        }

        tag_call.tags  = &boundary_heads[0];
        tag_call.count = 1;
        Sleep(150);
        event_start = event_count();
        get_snapshot(&before);
        CHECK(invoke_bounded(attempt_set_tag, &tag_call, 0), "256-node slSetTag chain completes successfully");
        get_snapshot(&after);
        ++expected_set_tag_calls;
        CHECK(after.set_tag_calls == before.set_tag_calls + 1 && after.last_tags == (uintptr_t) &boundary_heads[0] && after.last_count == 1
                  && after.last_viewport == (uintptr_t) &g_viewport && after.last_command_buffer == (uintptr_t) &g_command_buffer,
              "256-node slSetTag chain preserves original arguments and invocation");
        CHECK(wait_tag_bits(event_start, RESOURCE_MASK(5)), "slSetTag observes the final ResourceTag at the 256-node boundary");
        CHECK(no_tag_bits_outside_after(event_start, RESOURCE_MASK(0) | RESOURCE_MASK(5)),
              "256-node slSetTag chain reports only its supported head and tail types");
        CHECK(wait_single_tag_submission(event_start, 0, 1), "256-node slSetTag submission reports exact successful result and count");

        frame_tag_call.tags  = &boundary_heads[0];
        frame_tag_call.count = 1;
        Sleep(150);
        event_start = event_count();
        get_snapshot(&before);
        CHECK(invoke_bounded(attempt_set_tag_for_frame, &frame_tag_call, 0), "256-node slSetTagForFrame chain completes successfully");
        get_snapshot(&after);
        ++expected_frame_tag_calls;
        CHECK(after.set_tag_for_frame_calls == before.set_tag_for_frame_calls + 1 && after.last_frame == (uintptr_t) &g_frame_token
                  && after.last_viewport == (uintptr_t) &g_viewport && after.last_tags == (uintptr_t) &boundary_heads[0] && after.last_count == 1
                  && after.last_command_buffer == (uintptr_t) &g_command_buffer,
              "256-node slSetTagForFrame chain preserves original arguments and invocation");
        CHECK(wait_tag_bits(event_start, RESOURCE_MASK(5)), "slSetTagForFrame observes the final ResourceTag at the 256-node boundary");
        CHECK(no_tag_bits_outside_after(event_start, RESOURCE_MASK(0) | RESOURCE_MASK(5)),
              "256-node slSetTagForFrame chain reports only its supported head and tail types");
        CHECK(wait_single_tag_submission(event_start, 0, 1), "256-node slSetTagForFrame submission reports exact successful result and count");

        tag_call.tags  = boundary_heads;
        tag_call.count = 2;
        Sleep(150);
        event_start = event_count();
        get_snapshot(&before);
        CHECK(invoke_bounded(attempt_set_tag, &tag_call, 0), "shared-budget slSetTag chains complete successfully");
        get_snapshot(&after);
        ++expected_set_tag_calls;
        CHECK(after.set_tag_calls == before.set_tag_calls + 1 && after.last_tags == (uintptr_t) boundary_heads && after.last_count == 2
                  && after.last_viewport == (uintptr_t) &g_viewport && after.last_command_buffer == (uintptr_t) &g_command_buffer,
              "shared-budget slSetTag preserves original arguments and invocation");
        CHECK(wait_event(event_start, VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, FALSE, NULL), "slSetTag chains share the 256-node coverage budget");
        CHECK(wait_event(event_start, VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, 0, FALSE, NULL), "slSetTag observes resources before the shared boundary");
        CHECK((tag_bits_since(event_start) & boundary_head_bits) != 0, "slSetTag observes an array-head resource before the shared boundary");
        CHECK(no_tag_bits_outside_after(event_start, boundary_head_bits | boundary_tail_bits),
              "slSetTag shared-budget chains report only supported resource bits");
        CHECK((tag_bits_since(event_start) & boundary_tail_bits) != boundary_tail_bits,
              "slSetTag shares one 256-node budget across two 256-node chains");
        CHECK(wait_single_tag_submission(event_start, 0, 2), "shared-budget slSetTag submission reports exact successful result and array count");

        frame_tag_call.tags  = boundary_heads;
        frame_tag_call.count = 2;
        Sleep(150);
        event_start = event_count();
        get_snapshot(&before);
        CHECK(invoke_bounded(attempt_set_tag_for_frame, &frame_tag_call, 0), "shared-budget slSetTagForFrame chains complete successfully");
        get_snapshot(&after);
        ++expected_frame_tag_calls;
        CHECK(after.set_tag_for_frame_calls == before.set_tag_for_frame_calls + 1 && after.last_frame == (uintptr_t) &g_frame_token
                  && after.last_viewport == (uintptr_t) &g_viewport && after.last_tags == (uintptr_t) boundary_heads && after.last_count == 2
                  && after.last_command_buffer == (uintptr_t) &g_command_buffer,
              "shared-budget slSetTagForFrame preserves original arguments and invocation");
        CHECK(wait_event(event_start, VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, FALSE, NULL), "slSetTagForFrame chains share the 256-node coverage budget");
        CHECK(wait_event(event_start, VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, 0, FALSE, NULL),
              "slSetTagForFrame observes resources before the shared boundary");
        CHECK((tag_bits_since(event_start) & boundary_head_bits) != 0, "slSetTagForFrame observes an array-head resource before the shared boundary");
        CHECK(no_tag_bits_outside_after(event_start, boundary_head_bits | boundary_tail_bits),
              "slSetTagForFrame shared-budget chains report only supported resource bits");
        CHECK((tag_bits_since(event_start) & boundary_tail_bits) != boundary_tail_bits,
              "slSetTagForFrame shares one 256-node budget across two 256-node chains");
        CHECK(wait_single_tag_submission(event_start, 0, 2),
              "shared-budget slSetTagForFrame submission reports exact successful result and array count");
    }

    result = g_get_feature_function(FEATURE_DLSS_G, "slDLSSGSetOptions", &resolved);
    CHECK(result == 0 && resolved != NULL, "game resolves slDLSSGSetOptions successfully");
    CHECK(resolved == g_expected_options(), "game receives original resolved function pointer");
    g_set_options = (set_options_fn) resolved;
    get_snapshot(&snapshot);
    CHECK(snapshot.get_feature_function_calls == 1 && snapshot.get_state_calls == 0, "observer does not call game feature getters or DLSSG state");

    for (mode = 0; mode <= 3; ++mode)
    {
        options.base.next = NULL;
        set_guid(&options.base.type, 0xfac5f1cb, 0x2dfd, 0x4f36);
        options.base.type.data4[0] = 0xa1;
        options.base.type.data4[1] = 0xe6;
        options.base.type.data4[2] = 0x3a;
        options.base.type.data4[3] = 0x9e;
        options.base.type.data4[4] = 0x86;
        options.base.type.data4[5] = 0x52;
        options.base.type.data4[6] = 0x56;
        options.base.type.data4[7] = 0xc5;
        options.base.version       = 5;
        options.mode               = mode;
        option_call.function       = g_set_options;
        option_call.viewport       = &g_viewport;
        option_call.options        = &options;
        event_start                = event_count();
        CHECK(drive_until(attempt_options, &option_call, COUNTER_SET_OPTIONS, 0, event_start, VKSHADE_BRIDGE_FRAMEGEN_MODE, mode, TRUE, &attempts),
              "successful game-resolved DLSSG mode call is observed");
        expected_options_calls += attempts;
        get_snapshot(&snapshot);
        CHECK(snapshot.last_viewport == (uintptr_t) &g_viewport && snapshot.last_options == (uintptr_t) &options && snapshot.last_mode == mode,
              "slDLSSGSetOptions arguments reach original unchanged");
        CHECK(find_event_since(event_start, VKSHADE_BRIDGE_FRAMEGEN_MODE, mode, TRUE, &generation) && generation != 0,
              "frame generation mode event has nonzero generation");
    }

    set_tag(&removal_tag, NULL, 13, 1);
    frame_tag_call.tags = &removal_tag;
    Sleep(150);
    event_start = event_count();
    result      = g_set_tag_for_frame(frame_tag_call.frame, frame_tag_call.viewport, frame_tag_call.tags, 1, frame_tag_call.command_buffer);
    ++expected_frame_tag_calls;
    CHECK(result == 0 && no_tag_bits_outside_after(event_start, 0), "null resource removal contributes no resource type bits");
    CHECK(wait_single_tag_submission(event_start, 0, 1), "null slSetTagForFrame submission reports exact successful result and count");

    set_tag(&error_tag, &g_resources[7], 10, 1);
    set_base(&invalid_root, 0x87654321, 0x8765, 0x4321, 1);
    invalid_root.next = &error_tag.base;
    inputs[0]         = &invalid_root;
    eval.feature      = 424242u;
    event_start       = event_count();
    g_set_results(0, 0, 1, 0, 0);
    get_snapshot(&before);
    result = attempt_eval(&eval);
    get_snapshot(&after);
    ++expected_eval_calls;
    CHECK(result == 1 && after.evaluate_calls == before.evaluate_calls + 1, "failed slEvaluateFeature preserves error result and one invocation");
    CHECK(no_success_event_after(event_start, VKSHADE_BRIDGE_FEATURE_EVALUATED, 424242u) && no_tag_bits_outside_after(event_start, 0),
          "failed evaluation emits no success observations");

    tag_call.tags  = &g_tags[5];
    tag_call.count = 1;
    event_start    = event_count();
    g_set_results(0, 0, 0, 1, 0);
    get_snapshot(&before);
    result = attempt_set_tag(&tag_call);
    get_snapshot(&after);
    ++expected_set_tag_calls;
    CHECK(result == 1 && after.set_tag_calls == before.set_tag_calls + 1, "failed slSetTag preserves error result and one invocation");
    CHECK(no_tag_bits_outside_after(event_start, 0), "failed slSetTag emits no successful tag observation");
    CHECK(wait_single_tag_submission(event_start, 1, 1), "failed slSetTag submission reports exact error result and count");

    frame_tag_call.tags  = &g_tags[6];
    frame_tag_call.count = 1;
    event_start          = event_count();
    get_snapshot(&before);
    result = attempt_set_tag_for_frame(&frame_tag_call);
    get_snapshot(&after);
    ++expected_frame_tag_calls;
    CHECK(result == 1 && after.set_tag_for_frame_calls == before.set_tag_for_frame_calls + 1,
          "failed slSetTagForFrame preserves error result and one invocation");
    CHECK(no_tag_bits_outside_after(event_start, 0), "failed slSetTagForFrame emits no successful tag observation");
    CHECK(wait_single_tag_submission(event_start, 1, 1), "failed slSetTagForFrame submission reports exact error result and count");

    options.mode        = 2;
    option_call.options = &options;
    event_start         = event_count();
    g_set_results(0, 0, 0, 0, 1);
    get_snapshot(&before);
    result = attempt_options(&option_call);
    get_snapshot(&after);
    ++expected_options_calls;
    CHECK(result == 1, "failed slDLSSGSetOptions preserves exact error result");
    CHECK(after.set_options_calls == before.set_options_calls + 1, "failed slDLSSGSetOptions invokes original exactly once");
    CHECK(no_success_event_after(event_start, VKSHADE_BRIDGE_FRAMEGEN_MODE, 2), "failed frame generation mode call emits no mode observation");
    g_set_results(0, 0, 0, 0, 0);

    {
        eval_thread_context contexts[4];
        HANDLE              threads[4];
        uint32_t            n;
        event_start = event_count();
        get_snapshot(&before);
        for (n = 0; n < 4; ++n)
        {
            contexts[n].evaluate = g_evaluate;
            contexts[n].feature  = 3000u + n;
            contexts[n].failed   = 0;
            threads[n]           = CreateThread(NULL, 0, evaluate_worker, &contexts[n], 0, NULL);
            CHECK(threads[n] != NULL, "create concurrent caller");
        }
        for (n = 0; n < 4; ++n)
        {
            CHECK(WaitForSingleObject(threads[n], 30000) == WAIT_OBJECT_0, "concurrent Streamline calls complete");
            CloseHandle(threads[n]);
            CHECK(contexts[n].failed == 0, "concurrent original calls retain success results");
        }
        get_snapshot(&after);
        expected_eval_calls += 32;
        CHECK(after.evaluate_calls == before.evaluate_calls + 32, "concurrent evaluation calls invoke original exactly once each");
        for (n = 0; n < 4; ++n)
        {
            CHECK(wait_event(event_start, VKSHADE_BRIDGE_FEATURE_EVALUATED, 3000u + n, TRUE, NULL),
                  "concurrent successful feature observation arrives");
        }
    }

    pre_shutdown_generation = max_scoped_generation();
    event_start             = event_count();
    result                  = g_shutdown();
    CHECK(result == 0, "successful slShutdown returns original result");
    CHECK(wait_event(event_start, VKSHADE_BRIDGE_SHUTDOWN, 0, FALSE, &shutdown_generation), "successful shutdown closes observation epoch");
    CHECK(shutdown_generation != 0 && shutdown_generation > pre_shutdown_generation, "shutdown event advances nonzero observation generation");

    result = g_init(&g_preferences, 0x8877665544332211ull);
    CHECK(result == 0, "reinitialization returns original result");
    eval.feature = FEATURE_DLSS_G;
    eval.inputs  = NULL;
    eval.count   = 0;
    event_start  = event_count();
    CHECK(drive_until(attempt_eval, &eval, COUNTER_EVALUATE, 0, event_start, VKSHADE_BRIDGE_FEATURE_EVALUATED, FEATURE_DLSS_G, TRUE, &attempts),
          "post-shutdown successful evaluation is observed");
    expected_eval_calls += attempts;
    CHECK(find_event_since(event_start, VKSHADE_BRIDGE_FEATURE_EVALUATED, FEATURE_DLSS_G, TRUE, &generation) && generation > pre_shutdown_generation
              && generation >= shutdown_generation,
          "post-shutdown observation uses the new generation, including reset generation");

    g_set_results(0, 1, 0, 0, 0);
    Sleep(150);
    pre_failed_shutdown_generation = max_scoped_generation();
    event_start                    = event_count();
    result                         = g_shutdown();
    CHECK(result == 1, "failed slShutdown preserves exact game error result");
    eval.feature = FEATURE_DLSS_G;
    eval.inputs  = NULL;
    eval.count   = 0;
    CHECK(drive_until(attempt_eval, &eval, COUNTER_EVALUATE, 0, event_start, VKSHADE_BRIDGE_FEATURE_EVALUATED, FEATURE_DLSS_G, TRUE, &attempts),
          "successful evaluation after failed shutdown is observed");
    expected_eval_calls += attempts;
    CHECK(find_event_since(event_start, VKSHADE_BRIDGE_FEATURE_EVALUATED, FEATURE_DLSS_G, TRUE, &post_failed_shutdown_generation),
          "post-failure evaluation event is present in isolated window");
    Sleep(150);
    failed_shutdown_notified = find_event_since(event_start, VKSHADE_BRIDGE_SHUTDOWN, 0, FALSE, NULL);
    if (failed_shutdown_notified || post_failed_shutdown_generation > pre_failed_shutdown_generation)
    {
        CHECK(wait_event(event_start, VKSHADE_BRIDGE_COVERAGE_LIMITED, 0, FALSE, NULL), "failed shutdown reset reports limited coverage");
    }

    get_snapshot(&snapshot);
    CHECK(snapshot.init_calls == 2 && snapshot.shutdown_calls == 2, "init and shutdown originals each run once per game request");
    CHECK(snapshot.evaluate_calls == (uint32_t) expected_eval_calls, "evaluation original invocation count matches game calls");
    CHECK(snapshot.set_tag_calls == (uint32_t) expected_set_tag_calls && snapshot.set_tag_for_frame_calls == (uint32_t) expected_frame_tag_calls,
          "tag original invocation counts match game calls");
    CHECK(snapshot.set_options_calls == (uint32_t) expected_options_calls, "DLSSG setter original invocation count matches game calls");
    CHECK(snapshot.get_feature_function_calls == 1 && snapshot.get_state_calls == 0, "observer never calls game feature lookup or DLSSG state");
    CHECK(all_scoped_events_have_generation(), "scoped events use nonzero generations; process events use zero");
    EnterCriticalSection(&g_event_lock);
    {
        BOOL overflow = g_event_overflow;
        LeaveCriticalSection(&g_event_lock);
        CHECK(!overflow, "callback event buffer remains within test capacity");
    }
    DeleteCriticalSection(&g_event_lock);
    write_text(stress_metadata ? "PASS: Streamline metadata bounds\r\n"
                               : (preload ? "PASS: preloaded Streamline companion\r\n" : "PASS: late-loaded Streamline companion\r\n"));
    return 0;
}

void WINAPI mainCRTStartup(void)
{
    int     argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int     result;
    if (argv == NULL)
        ExitProcess(1);
    if (argc == 5 && wide_equal(argv[1], L"--inject-target"))
    {
        result = run_inject_target(argv[2], argv[3], argv[4]);
    }
    else if (argc == 4 && wide_equal(argv[1], L"--injector-contract"))
    {
        result = run_injector_case(argv[2], argv[3]);
    }
    else if (argc == 2 && wide_equal(argv[1], L"--metadata-stress"))
    {
        result = run_case(TRUE, TRUE);
    }
    else if (argc == 2 && wide_equal(argv[1], L"--preloaded"))
    {
        result = run_case(TRUE, FALSE);
    }
    else if (argc == 2 && wide_equal(argv[1], L"--late-load"))
    {
        result = run_case(FALSE, FALSE);
    }
    else
    {
        result = fail("expected one mode; --injector-contract requires injector and observer paths");
    }
    LocalFree(argv);
    ExitProcess((UINT) result);
}
