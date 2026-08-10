#include "core/Log.h"

#include <iostream>

namespace applog
{
    namespace
    {
        // A shipped game logs through here too and never drains the store, so
        // the oldest lines are dropped in blocks once it gets large.
        constexpr std::size_t kMaxEntries = 20000;
        constexpr std::size_t kTrimCount = 5000;

        std::vector<LogEntry> g_entries;
        std::uint32_t g_first_id = 0;
    }

    void Add(LogLevel level, const std::string& text)
    {
        if (g_entries.size() >= kMaxEntries)
        {
            g_entries.erase(g_entries.begin(), g_entries.begin() + kTrimCount);
            g_first_id += static_cast<std::uint32_t>(kTrimCount);
        }

        g_entries.push_back({level, text});

        const char* tag = level == LogLevel::Error   ? "[ERROR]" :
                          level == LogLevel::Warning ? "[WARN] " :
                          level == LogLevel::Build   ? "[BUILD]" :
                          level == LogLevel::Script  ? "[LOG]  " :
                                                       "[INFO] ";
        std::cout << tag << ' ' << text << std::endl;
    }

    const std::vector<LogEntry>& Entries()
    {
        return g_entries;
    }

    std::uint32_t FirstId()
    {
        return g_first_id;
    }

    void Clear()
    {
        g_first_id += static_cast<std::uint32_t>(g_entries.size());
        g_entries.clear();
    }
}
