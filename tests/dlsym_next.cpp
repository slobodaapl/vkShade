#include <dlfcn.h>
#include <iostream>

int main()
{
    using Probe   = int (*)();
    using Missing = bool (*)();
    auto probe    = reinterpret_cast<Probe>(dlsym(RTLD_DEFAULT, "vkshade_test_next_symbol"));
    auto missing  = reinterpret_cast<Missing>(dlsym(RTLD_DEFAULT, "vkshade_test_next_missing"));
    auto marker   = reinterpret_cast<Probe>(dlsym(RTLD_DEFAULT, "vkshade_test_hook_marker"));
    if (!probe || probe() != 42 || !missing || !missing() || !marker || marker() != 73)
    {
        std::cerr << "RTLD_NEXT must resolve after the caller and retain downstream dlsym hooks\n";
        return 1;
    }
    return 0;
}
