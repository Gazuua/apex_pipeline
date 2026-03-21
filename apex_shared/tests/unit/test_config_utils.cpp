// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/config_utils.hpp>

#include <gtest/gtest.h>

#include <cstdlib>

namespace
{

// Helper: cross-platform setenv/unsetenv
void set_env(const char* name, const char* value)
{
#ifdef _MSC_VER
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unset_env(const char* name)
{
#ifdef _MSC_VER
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

class ConfigUtilsTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        // Clean up any env vars we set
        unset_env("APEX_TEST_VAR");
        unset_env("APEX_TEST_VAR2");
    }
};

TEST_F(ConfigUtilsTest, PlainStringUnchanged)
{
    EXPECT_EQ(apex::shared::expand_env("hello world"), "hello world");
    EXPECT_EQ(apex::shared::expand_env("no-dollar-sign"), "no-dollar-sign");
}

TEST_F(ConfigUtilsTest, ExpandsSetVariable)
{
    set_env("APEX_TEST_VAR", "my_value");
    EXPECT_EQ(apex::shared::expand_env("${APEX_TEST_VAR}"), "my_value");
}

TEST_F(ConfigUtilsTest, ExpandsWithDefault_VarSet)
{
    set_env("APEX_TEST_VAR", "actual");
    EXPECT_EQ(apex::shared::expand_env("${APEX_TEST_VAR:-fallback}"), "actual");
}

TEST_F(ConfigUtilsTest, ExpandsWithDefault_VarUnset)
{
    unset_env("APEX_TEST_VAR");
    EXPECT_EQ(apex::shared::expand_env("${APEX_TEST_VAR:-fallback}"), "fallback");
}

TEST_F(ConfigUtilsTest, UnsetVarNoDefault_KeepsOriginal)
{
    unset_env("APEX_TEST_VAR");
    EXPECT_EQ(apex::shared::expand_env("${APEX_TEST_VAR}"), "${APEX_TEST_VAR}");
}

TEST_F(ConfigUtilsTest, MultipleVarsInString)
{
    set_env("APEX_TEST_VAR", "host");
    set_env("APEX_TEST_VAR2", "5432");
    EXPECT_EQ(apex::shared::expand_env("host=${APEX_TEST_VAR} port=${APEX_TEST_VAR2}"), "host=host port=5432");
}

TEST_F(ConfigUtilsTest, EmptyInput)
{
    EXPECT_EQ(apex::shared::expand_env(""), "");
}

TEST_F(ConfigUtilsTest, DollarWithoutBraces_Unchanged)
{
    // $VAR (no braces) should pass through unchanged -- only ${VAR} syntax is supported
    EXPECT_EQ(apex::shared::expand_env("$NOT_BRACED"), "$NOT_BRACED");
    EXPECT_EQ(apex::shared::expand_env("prefix$VAR suffix"), "prefix$VAR suffix");
}

TEST_F(ConfigUtilsTest, EmptyVarName_Unchanged)
{
    // ${} has no valid variable name -- regex requires [A-Za-z_] start, so no match
    EXPECT_EQ(apex::shared::expand_env("${}"), "${}");
}

TEST_F(ConfigUtilsTest, DefaultValueEmpty_ExpandsToEmpty)
{
    // ${VAR:-} with empty default -- unset var should expand to empty string
    unset_env("APEX_TEST_VAR");
    EXPECT_EQ(apex::shared::expand_env("${APEX_TEST_VAR:-}"), "");
}

TEST_F(ConfigUtilsTest, VarEmbeddedInText)
{
    // Variable surrounded by text with no spaces -- tests last_pos append logic
    set_env("APEX_TEST_VAR", "world");
    EXPECT_EQ(apex::shared::expand_env("hello${APEX_TEST_VAR}!"), "helloworld!");
}

} // anonymous namespace
