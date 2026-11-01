#include "shader_metadata_loader.hpp"

#include "config_paths.hpp"
#include "shader_metadata.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>

extern char** environ;

namespace vkShade
{
    namespace
    {
        constexpr size_t maxOutput = 4 * 1024 * 1024;

        std::string checkerPath()
        {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(&checkerPath), &info) && info.dli_fname)
            {
                const auto sibling = std::filesystem::path(info.dli_fname).parent_path() / "vkshade-shader-check";
                if (access(sibling.c_str(), X_OK) == 0)
                    return sibling.string();
            }
            return std::string(BINDIR) + "/vkshade-shader-check";
        }

        void addPart(std::string& key, const std::string& part)
        {
            key += std::to_string(part.size());
            key += ':';
            key += part;
        }
    } // namespace

    ShaderMetadataLoader::~ShaderMetadataLoader()
    {
        stop();
    }

    std::string ShaderMetadataLoader::key(const ShaderMetadataRequest& request)
    {
        std::string result;
        addPart(result, request.path);
        for (const auto& include : request.includes)
            addPart(result, include);
        result += '|';
        for (const auto& [name, value] : request.defines)
        {
            addPart(result, name);
            addPart(result, value);
        }
        addPart(result, request.effectName);
        addPart(result, request.profileConfig);
        addPart(result, request.baseConfig);
        addPart(result, request.gameName);
        return result;
    }

    void ShaderMetadataLoader::request(const ShaderMetadataRequest& request, bool priority)
    {
        const auto wanted = key(request);
        if (results.count(wanted) || (child >= 0 && key(active) == wanted))
            return;
        if (!queuedKeys.insert(wanted).second)
        {
            if (priority)
            {
                auto found = std::find_if(queue.begin(), queue.end(), [&](const auto& queued) { return key(queued) == wanted; });
                if (found != queue.end() && found != queue.begin())
                {
                    auto promoted = std::move(*found);
                    queue.erase(found);
                    queue.push_front(std::move(promoted));
                }
            }
            return;
        }
        if (priority)
            queue.push_front(request);
        else
            queue.push_back(request);
    }

    const ShaderTestResult* ShaderMetadataLoader::result(const ShaderMetadataRequest& request) const
    {
        auto it = results.find(key(request));
        return it == results.end() ? nullptr : &it->second;
    }

    size_t ShaderMetadataLoader::pending() const
    {
        return queue.size() + (child >= 0 ? 1 : 0);
    }

    void ShaderMetadataLoader::stop()
    {
        if (child >= 0)
        {
            int status;
            if (waitpid(child, &status, WNOHANG) == 0)
            {
                if (childFd >= 0)
                    syscall(SYS_pidfd_send_signal, childFd, SIGKILL, nullptr, 0);
                else
                    kill(child, SIGKILL);
            }
            while (waitpid(child, &status, 0) < 0 && errno == EINTR)
                ;
            child = -1;
        }
        if (childFd >= 0)
        {
            close(childFd);
            childFd = -1;
        }
        if (pipe >= 0)
        {
            close(pipe);
            pipe = -1;
        }
        output.clear();
    }

    void ShaderMetadataLoader::clear()
    {
        stop();
        queue.clear();
        queuedKeys.clear();
        results.clear();
    }

    void ShaderMetadataLoader::startNext()
    {
        if (child >= 0 || queue.empty())
            return;
        active = std::move(queue.front());
        queue.pop_front();
        queuedKeys.erase(key(active));

        std::vector<std::string> args{checkerPath(), active.path, "--metadata"};
        if (!active.gameName.empty())
        {
            args.emplace_back("--game-name");
            args.push_back(active.gameName);
        }
        if (!active.effectName.empty())
        {
            args.emplace_back("--effect-name");
            args.push_back(active.effectName);
        }
        if (!active.profileConfig.empty())
        {
            args.emplace_back("--profile-config");
            args.push_back(active.profileConfig);
        }
        if (!active.baseConfig.empty())
        {
            args.emplace_back("--base-config");
            args.push_back(active.baseConfig);
        }
        for (const auto& include : active.includes)
        {
            args.emplace_back("--include");
            args.push_back(include);
        }
        for (const auto& [name, value] : active.defines)
        {
            args.emplace_back("--define");
            args.push_back(name + '=' + value);
        }
        std::vector<char*> argv;
        for (auto& arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);

        std::vector<std::string> env;
        for (char** entry = environ; *entry; ++entry)
        {
            std::string value(*entry);
            if (value.starts_with("LD_PRELOAD=") || value.starts_with("LD_AUDIT=") || value.starts_with("VKSHADE_LOG_LEVEL=")
                || value.starts_with("VKSHADE_LOG_FILE=") || value.starts_with("ENABLE_VKSHADE="))
                continue;
            env.push_back(std::move(value));
        }
        env.emplace_back("VKSHADE_LOG_LEVEL=none");
        std::vector<char*> envp;
        for (auto& item : env)
            envp.push_back(item.data());
        envp.push_back(nullptr);

        int fds[2];
        if (pipe2(fds, O_CLOEXEC) < 0)
        {
            results[key(active)].errorMessage = std::string("Cannot open shader helper pipe: ") + strerror(errno);
            return;
        }
        for (int& fd : fds)
            if (fd <= STDERR_FILENO)
            {
                const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
                close(fd);
                fd = replacement;
            }
        if (fds[0] < 0 || fds[1] < 0)
        {
            if (fds[0] >= 0)
                close(fds[0]);
            if (fds[1] >= 0)
                close(fds[1]);
            results[key(active)].errorMessage = "Cannot reserve shader helper pipe descriptors";
            return;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
        posix_spawn_file_actions_addclose(&actions, fds[1]);
        pid_t pid;
        int   error = posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        close(fds[1]);
        if (error != 0)
        {
            close(fds[0]);
            results[key(active)].errorMessage = "Cannot start " + args[0] + ": " + strerror(error);
            return;
        }
        pipe = fds[0];
        fcntl(pipe, F_SETFL, fcntl(pipe, F_GETFL) | O_NONBLOCK);
        child   = pid;
        childFd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
        started = std::chrono::steady_clock::now();
        output.clear();
    }

    void ShaderMetadataLoader::poll()
    {
        startNext();
        if (child < 0)
            return;
        bool closed = false;
        char buffer[8192];
        for (;;)
        {
            const ssize_t count = read(pipe, buffer, sizeof(buffer));
            if (count > 0)
            {
                output.append(buffer, static_cast<size_t>(count));
                if (output.size() > maxOutput)
                    break;
            }
            else if (count == 0)
            {
                closed = true;
                break;
            }
            else if (errno != EINTR)
                break;
        }
        if (output.size() > maxOutput || std::chrono::steady_clock::now() - started > std::chrono::seconds(60))
        {
            results[key(active)].errorMessage = "Shader metadata helper exceeded its output or time limit";
            stop();
            return;
        }
        if (!closed)
            return;
        int   status;
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == 0 || (done < 0 && errno == EINTR))
            return;
        auto  parsed = decodeShaderMetadata(output);
        auto& result = results[key(active)];
        if (parsed)
            result = std::move(*parsed);
        else
            result.errorMessage = "Shader metadata helper returned invalid output";
        child = -1;
        if (childFd >= 0)
        {
            close(childFd);
            childFd = -1;
        }
        close(pipe);
        pipe = -1;
        output.clear();
    }
} // namespace vkShade
