#include <dlfcn.h>
#include <cstring>

static int hookMarker()
{
    return 73;
}

extern "C" void* dlsym(void* handle, const char* symbol)
{
    if (std::strcmp(symbol, "vkshade_test_hook_marker") == 0)
        return reinterpret_cast<void*>(&hookMarker);

    using Lookup = void* (*)(void*, const char*);
    auto next    = reinterpret_cast<Lookup>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
    if (!next)
        next = reinterpret_cast<Lookup>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34"));
    return next ? next(handle, symbol) : nullptr;
}

extern "C" int vkshade_test_next_symbol()
{
    static bool resolving = false;
    if (resolving)
        return -1;
    resolving        = true;
    using Next       = int (*)();
    auto      next   = reinterpret_cast<Next>(dlsym(RTLD_NEXT, "vkshade_test_next_symbol"));
    const int result = next ? next() + 1 : -2;
    resolving        = false;
    return result;
}

extern "C" bool vkshade_test_next_missing()
{
    dlerror();
    return dlsym(RTLD_NEXT, "vkshade_test_missing_symbol") == nullptr && dlerror() != nullptr;
}
