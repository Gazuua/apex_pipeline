// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "system_profile.hpp"
#include <benchmark/benchmark.h>

int main(int argc, char** argv)
{
    auto profile = apex::bench::detect_system_profile();
    profile.print();

    benchmark::Initialize(&argc, argv);
    benchmark::AddCustomContext("physical_cores", std::to_string(profile.physical_cores));
    benchmark::AddCustomContext("logical_cores", std::to_string(profile.logical_cores));
    benchmark::AddCustomContext("total_ram_mb", std::to_string(profile.total_ram_bytes / (1024 * 1024)));

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
