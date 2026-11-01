#include "host_bridge.hpp"

#if defined(__linux__) && defined(__x86_64__)
#include "bridge_events.h"
#include "bridge_identity.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <asm/prctl.h>
#include <dlfcn.h>
#include <link.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

namespace
{
    std::atomic_flag  starting = ATOMIC_FLAG_INIT;
    std::atomic<bool> attempted{false};

    struct Guard
    {
        ~Guard()
        {
            starting.clear(std::memory_order_release);
        }
    };

    void unavailable() noexcept
    {
        vkshade_bridge_observe_event(VKSHADE_BRIDGE_HOST_UNAVAILABLE, 0, 0);
    }

    bool canonical_file(const std::string& path, int mode, std::string& result)
    {
        char* resolved = realpath(path.c_str(), nullptr);
        if (!resolved)
            return false;
        std::string canonical(resolved);
        free(resolved);
        struct stat st;
        if (canonical[0] != '/' || stat(canonical.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || access(canonical.c_str(), mode) != 0)
            return false;
        result = std::move(canonical);
        return true;
    }

    bool wine_mapping(const std::string& mapping)
    {
        static const std::string suffix = "/wine/x86_64-unix/ntdll.so";
        return mapping.size() > suffix.size() && mapping[0] == '/' && mapping.compare(mapping.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool wine_runner(const std::string& mapping, std::string& runner)
    {
        static const std::string suffix = "/lib/wine/x86_64-unix/ntdll.so";
        if (mapping.size() <= suffix.size() || mapping.compare(mapping.size() - suffix.size(), suffix.size(), suffix) != 0)
            return false;
        return canonical_file(mapping.substr(0, mapping.size() - suffix.size()) + "/bin/wine", X_OK, runner);
    }

    struct WineSearch
    {
        std::string runner;
        bool        found  = false;
        bool        failed = false;
    };

    int find_wine(struct dl_phdr_info* info, size_t, void* data) noexcept
    {
        auto& search = *static_cast<WineSearch*>(data);
        try
        {
            if (info->dlpi_name && wine_mapping(info->dlpi_name))
            {
                search.found = true;
                wine_runner(info->dlpi_name, search.runner);
                return 1;
            }
        }
        catch (...)
        {
            search.failed = true;
            return 1;
        }
        return 0;
    }

    bool find_runner(std::string& runner, bool& found)
    {
        WineSearch search;
        dl_iterate_phdr(find_wine, &search);
        found = search.found;
        if (search.failed || found)
        {
            runner = std::move(search.runner);
            return !runner.empty();
        }
        std::ifstream maps("/proc/self/maps");
        std::string   line;
        while (std::getline(maps, line))
        {
            const auto slash = line.find('/');
            if (slash != std::string::npos && wine_mapping(line.substr(slash)))
            {
                found = true;
                return wine_runner(line.substr(slash), runner);
            }
        }
        return false;
    }

    bool read_self(uintptr_t address, void* output, size_t size)
    {
        iovec local{output, size};
        iovec remote{reinterpret_cast<void*>(address), size};
        return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
    }

    bool windows_pid(uint32_t& pid)
    {
        unsigned long gs = 0;
        if (syscall(SYS_arch_prctl, ARCH_GET_GS, &gs) != 0 || !gs || gs > std::numeric_limits<uintptr_t>::max() - 0x40)
            return false;
        uintptr_t self = 0, process = 0;
        if (!read_self(gs + 0x30, &self, sizeof(self)) || self != gs || !read_self(gs + 0x40, &process, sizeof(process)) || !process
            || process > UINT32_MAX)
            return false;
        pid = static_cast<uint32_t>(process);
        return true;
    }

    struct Job
    {
        std::string              wine, helper, companion, pid, address, cookie;
        std::vector<std::string> environment;
    };

    using PidfdSpawn  = int (*)(int*, const char*, const posix_spawn_file_actions_t*, const posix_spawnattr_t*, char* const[], char* const[]);
    using PidfdSignal = int (*)(int, int, siginfo_t*, unsigned int);

    // Returns one for a reaped child, zero on timeout, and minus one on error.
    int wait_pidfd(int fd, int seconds, siginfo_t& info)
    {
        timespec deadline{};
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
            return -1;
        deadline.tv_sec += seconds;
        for (;;)
        {
            info = {};
            if (waitid(static_cast<idtype_t>(3), static_cast<id_t>(fd), &info, WEXITED | WNOHANG) != 0)
            {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (info.si_pid)
                return 1;
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
                return -1;
            long long remaining = (deadline.tv_sec - now.tv_sec) * 1000000000LL + deadline.tv_nsec - now.tv_nsec;
            if (remaining <= 0)
                return 0;
            pollfd descriptor{fd, POLLIN, 0};
            int    result = poll(&descriptor, 1, static_cast<int>((remaining + 999999) / 1000000));
            if (result < 0 && errno != EINTR)
                return -1;
            if (result > 0 && (descriptor.revents & (POLLERR | POLLNVAL)))
                return -1;
        }
    }

    void supervise_pidfd(int fd, PidfdSignal signal_child)
    {
        siginfo_t info{};
        int       result = wait_pidfd(fd, 30, info);
        if (result <= 0 && !(result < 0 && errno == ECHILD))
        {
            unavailable();
            signal_child(fd, SIGTERM, nullptr, 0);
            timespec grace{};
            bool     have_grace = clock_gettime(CLOCK_MONOTONIC, &grace) == 0;
            grace.tv_sec += 2;
            result = wait_pidfd(fd, 2, info);
            if (result < 0 && errno != ECHILD)
            {
                if (have_grace)
                {
                    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &grace, nullptr) == EINTR)
                    {
                    }
                }
                else
                {
                    timespec delay{2, 0};
                    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
                    {
                    }
                }
                result = 0;
            }
            if (result == 0)
            {
                signal_child(fd, SIGKILL, nullptr, 0);
                for (;;)
                {
                    info   = {};
                    result = waitid(static_cast<idtype_t>(3), static_cast<id_t>(fd), &info, WEXITED);
                    if (result == 0)
                        break;
                    const int error = errno;
                    if (error == ECHILD)
                        break;
                    if (error == EINTR)
                        continue;
                    timespec delay{1, 0};
                    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
                    {
                    }
                }
                result = result == 0 ? 1 : -1;
            }
        }
        if (result < 0 || (result == 1 && (info.si_code != CLD_EXITED || info.si_status != 0)))
            unavailable();
        close(fd);
    }

    void* run_helper(void* argument) noexcept
    {
        std::unique_ptr<Job> job(static_cast<Job*>(argument));
        try
        {
            std::vector<char*> environment;
            for (auto& value : job->environment)
                environment.push_back(&value[0]);
            environment.push_back(nullptr);
            char* argv[]          = {&job->wine[0], &job->helper[0], &job->pid[0], &job->address[0], &job->cookie[0], &job->companion[0], nullptr};
            auto  pidfd_spawn_fn  = reinterpret_cast<PidfdSpawn>(dlsym(RTLD_DEFAULT, "pidfd_spawn"));
            auto  pidfd_signal_fn = reinterpret_cast<PidfdSignal>(dlsym(RTLD_DEFAULT, "pidfd_send_signal"));
            if (pidfd_spawn_fn && pidfd_signal_fn)
            {
                int fd    = -1;
                int error = pidfd_spawn_fn(&fd, job->wine.c_str(), nullptr, nullptr, argv, environment.data());
                if (error == 0)
                {
                    supervise_pidfd(fd, pidfd_signal_fn);
                    return nullptr;
                }
                if (error != ENOSYS)
                {
                    unavailable();
                    return nullptr;
                }
            }
            pid_t child;
            if (posix_spawn(&child, job->wine.c_str(), nullptr, nullptr, argv, environment.data()) != 0)
            {
                unavailable();
                return nullptr;
            }
            timespec deadline{};
            bool     timed_out = clock_gettime(CLOCK_MONOTONIC, &deadline) != 0;
            deadline.tv_sec += 30;
            int   status;
            pid_t waited = 0;
            while (!timed_out)
            {
                waited = waitpid(child, &status, WNOHANG);
                if (waited == child || (waited < 0 && errno != EINTR))
                    break;
                timespec now{};
                if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec > deadline.tv_sec
                    || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
                {
                    timed_out = true;
                    break;
                }
                timespec pause{0, 100000000};
                nanosleep(&pause, nullptr);
            }
            if (timed_out)
            {
                unavailable();
                do
                {
                    waited = waitpid(child, &status, 0);
                } while (waited < 0 && errno == EINTR);
                return nullptr;
            }
            if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
                unavailable();
        }
        catch (...)
        {
            unavailable();
        }
        return nullptr;
    }
} // namespace

void vkshade_start_proton_bridge() noexcept
{
    if (attempted.load(std::memory_order_acquire) || starting.test_and_set(std::memory_order_acquire))
        return;
    Guard guard;
    if (attempted.load(std::memory_order_relaxed))
        return;
    try
    {
        const char* marker = getenv("VKSHADE_PROTON_BRIDGE_HELPER");
        if (marker && strcmp(marker, "1") == 0)
            return;
        std::unique_ptr<Job> job(new Job);
        bool                 wine_found = false;
        if (!find_runner(job->wine, wine_found))
        {
            if (wine_found)
            {
                attempted.store(true, std::memory_order_release);
                unavailable();
            }
            return;
        }
        uint32_t pid;
        if (!windows_pid(pid))
            return;
        attempted.store(true, std::memory_order_release);

        Dl_info     info{};
        std::string library;
        if (!dladdr(reinterpret_cast<void*>(&vkshade_start_proton_bridge), &info) || !info.dli_fname
            || !canonical_file(info.dli_fname, R_OK, library))
        {
            unavailable();
            return;
        }
        const auto        slash     = library.rfind('/');
        const std::string directory = library.substr(0, slash + 1);
        if (!canonical_file(directory + "vkshade-proton-injector.exe", R_OK, job->helper)
            || !canonical_file(directory + "vkshade-proton-observer.dll", R_OK, job->companion)
            || !dlopen(library.c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE))
        {
            unavailable();
            return;
        }

        auto    identity = std::unique_ptr<vkshade_proton_identity>(new vkshade_proton_identity{});
        ssize_t received;
        do
        {
            received = getrandom(&identity->cookie, sizeof(identity->cookie), 0);
        } while (received < 0 && errno == EINTR);
        if (received != sizeof(identity->cookie) || !identity->cookie)
        {
            unavailable();
            return;
        }
        identity->callback_address = reinterpret_cast<uintptr_t>(&vkshade_bridge_observe_event);
        char address[2 + sizeof(uintptr_t) * 2 + 1], cookie[19];
        snprintf(address, sizeof(address), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(identity.get())));
        snprintf(cookie, sizeof(cookie), "0x%llx", static_cast<unsigned long long>(identity->cookie));
        job->pid     = std::to_string(pid);
        job->address = address;
        job->cookie  = cookie;
        for (char** item = environ; item && *item; ++item)
        {
            if (strncmp(*item, "VKSHADE_PROTON_BRIDGE_HELPER=", 29) != 0)
                job->environment.emplace_back(*item);
        }
        job->environment.emplace_back("VKSHADE_PROTON_BRIDGE_HELPER=1");
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0)
        {
            unavailable();
            return;
        }
        const int attr_result = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        const int result = attr_result ? attr_result : pthread_create(&thread, &attr, run_helper, job.get());
        pthread_attr_destroy(&attr);
        if (result != 0)
        {
            unavailable();
            return;
        }
        identity.release();
        job.release();
    }
    catch (...)
    {
        unavailable();
    }
}
#else
void vkshade_start_proton_bridge() noexcept
{
}
#endif
