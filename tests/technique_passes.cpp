#include "effects/technique_passes.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace vkShade;

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

static std::vector<std::string> names(const std::vector<reshadefx::pass_info>& passes)
{
    std::vector<std::string> result;
    for (const auto& pass : passes)
        result.push_back(pass.ps_entry_point);
    return result;
}

int main()
{
    reshadefx::module         module;
    reshadefx::technique_info first, second, empty;
    first.name  = "First";
    second.name = "Second";
    empty.name  = "Empty";
    first.passes.resize(2);
    first.passes[0].ps_entry_point = "first-pass";
    first.passes[1].ps_entry_point = "second-pass";
    second.passes.resize(1);
    second.passes[0].ps_entry_point = "third-pass";
    module.techniques               = {first, second, empty};

    bool ok     = true;
    auto passes = selectTechniquePasses(module, std::nullopt);
    ok &= check(passes && names(*passes) == std::vector<std::string>({"first-pass", "second-pass", "third-pass"}),
                "Legacy effect execution must preserve every technique and pass in declaration order");
    passes = selectTechniquePasses(module, std::string("Second"));
    ok &= check(passes && names(*passes) == std::vector<std::string>({"third-pass"}), "A selected technique must execute only its own passes");
    passes = selectTechniquePasses(module, std::string("Empty"));
    ok &= check(passes && passes->empty(), "An empty technique must remain distinct from a missing technique");
    ok &= check(!selectTechniquePasses(module, std::string("Missing")), "Unknown techniques must not silently execute another technique");
    return ok ? 0 : 1;
}
