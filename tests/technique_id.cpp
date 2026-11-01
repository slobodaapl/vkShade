#include "technique_id.hpp"

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

int main()
{
    const std::vector<TechniqueId> ids = {
        {TechniqueOwner::BuiltIn, "cas", ""},
        {TechniqueOwner::File, "/shader dir/a:b@c%#.fx", "First look"},
        {TechniqueOwner::Instance, "effect.2", "Second"},
        {TechniqueOwner::File, "/shaders/žltý.fx", "Éffect"},
    };
    bool ok = true;
    for (const auto& id : ids)
    {
        const auto encoded = encodeTechniqueId(id);
        ok &= check(encoded.find(':') == std::string::npos, "Technique IDs must fit colon-separated profile lists");
        ok &= check(decodeTechniqueId(encoded) == id, "Technique identity must survive profile serialization");
    }
    ok &=
        check(encodeTechniqueId(ids[1]) == "F@/shader%20dir/a%3Ab%40c%25%23.fx@First%20look", "The serialized technique identity must remain stable");
    for (const auto& invalid : {"", "X@a@b", "F@@b", "F@a", "F@a@b@c", "F@a%2@b", "F@a%GG@b", "F@a%00@b", "F@a:b@c"})
        ok &= check(!decodeTechniqueId(invalid), "Malformed technique identities must be rejected");
    return ok ? 0 : 1;
}
