// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fstream>
#include <sys/sysinfo.h>
#endif

namespace apex::bench
{

struct SystemProfile
{
    uint32_t physical_cores{0};
    uint32_t logical_cores{0};
    size_t total_ram_bytes{0};
    size_t available_ram_bytes{0};

    uint32_t bench_cores() const
    {
        return std::max(1u, logical_cores > 2 ? logical_cores / 2 : logical_cores);
    }

    void print() const
    {
        std::cout << "=== System Profile ===\n"
                  << "Physical cores: " << physical_cores << '\n'
                  << "Logical cores:  " << logical_cores << '\n'
                  << "Total RAM:      " << (total_ram_bytes / (1024 * 1024)) << " MB\n"
                  << "Available RAM:  " << (available_ram_bytes / (1024 * 1024)) << " MB\n"
                  << "Bench cores:    " << bench_cores() << '\n'
                  << "======================\n";
    }
};

inline SystemProfile detect_system_profile()
{
    SystemProfile p;
    p.logical_cores = std::thread::hardware_concurrency();

#ifdef _WIN32
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len > 0)
    {
        std::vector<uint8_t> buf(len);
        if (GetLogicalProcessorInformationEx(
                RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
        {
            uint32_t count = 0;
            DWORD offset = 0;
            while (offset < len)
            {
                auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + offset);
                if (info->Relationship == RelationProcessorCore)
                    ++count;
                offset += info->Size;
            }
            p.physical_cores = count;
        }
    }
    if (p.physical_cores == 0)
        p.physical_cores = std::max(1u, p.logical_cores / 2);

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem))
    {
        p.total_ram_bytes = static_cast<size_t>(mem.ullTotalPhys);
        p.available_ram_bytes = static_cast<size_t>(mem.ullAvailPhys);
    }
#else
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line))
    {
        if (line.find("cpu cores") != std::string::npos)
        {
            auto pos = line.find(':');
            if (pos != std::string::npos)
                p.physical_cores = static_cast<uint32_t>(std::stoul(line.substr(pos + 1)));
            break;
        }
    }
    if (p.physical_cores == 0)
        p.physical_cores = std::max(1u, p.logical_cores / 2);

    struct sysinfo si{};
    if (sysinfo(&si) == 0)
    {
        p.total_ram_bytes = static_cast<size_t>(si.totalram) * si.mem_unit;
        p.available_ram_bytes = static_cast<size_t>(si.freeram + si.bufferram) * si.mem_unit;
    }
#endif

    return p;
}

} // namespace apex::bench
