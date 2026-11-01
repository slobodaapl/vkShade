#ifndef SHADER_METADATA_LOADER_HPP_INCLUDED
#define SHADER_METADATA_LOADER_HPP_INCLUDED

#include "reshade_parser.hpp"

#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace vkShade
{
    struct ShaderMetadataRequest
    {
        std::string                                      path;
        std::vector<std::string>                         includes;
        std::vector<std::pair<std::string, std::string>> defines;
        std::string                                      effectName;
        std::string                                      profileConfig;
        std::string                                      baseConfig;
        std::string                                      gameName;
    };

    class ShaderMetadataLoader
    {
    public:
        ~ShaderMetadataLoader();
        static std::string      key(const ShaderMetadataRequest& request);
        void                    request(const ShaderMetadataRequest& request, bool priority = false);
        void                    poll();
        void                    clear();
        const ShaderTestResult* result(const ShaderMetadataRequest& request) const;
        size_t                  pending() const;

    private:
        void stop();
        void startNext();

        std::map<std::string, ShaderTestResult> results;
        std::deque<ShaderMetadataRequest>       queue;
        std::unordered_set<std::string>         queuedKeys;
        ShaderMetadataRequest                   active;
        std::string                             output;
        std::chrono::steady_clock::time_point   started;
        int                                     child   = -1;
        int                                     childFd = -1;
        int                                     pipe    = -1;
    };
} // namespace vkShade

#endif
