// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/config_utils.hpp>

#include <cstdlib>
#include <regex>

namespace apex::shared
{

std::string expand_env(std::string_view value)
{
    // Quick check -- avoid regex overhead for strings without '$'
    if (value.find('$') == std::string_view::npos)
    {
        return std::string(value);
    }

    static const std::regex env_re(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-(.*?))?\})");

    std::string input(value);
    std::string result;
    result.reserve(input.size());

    std::sregex_iterator it(input.begin(), input.end(), env_re);
    std::sregex_iterator end;
    size_t last_pos = 0;

    for (; it != end; ++it)
    {
        const auto& match = *it;
        result.append(input, last_pos, static_cast<size_t>(match.position()) - last_pos);

        const std::string var_name = match[1].str();
        // MSVC C4996: getenv is standard C++ and safe for read-only use
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        const char* env_val = std::getenv(var_name.c_str());
#ifdef _MSC_VER
#pragma warning(pop)
#endif

        if (env_val)
        {
            result.append(env_val);
        }
        else if (match[2].matched)
        {
            // Default value provided via :- syntax
            result.append(match[2].str());
        }
        else
        {
            // No env var, no default -- keep original
            result.append(match[0].str());
        }

        last_pos = static_cast<size_t>(match.position() + match.length());
    }

    result.append(input, last_pos, input.size() - last_pos);
    return result;
}

} // namespace apex::shared
