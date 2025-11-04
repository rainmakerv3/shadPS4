// SPDX-FileCopyrightText: Copyright 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/assert.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/logging/log_entry.h"
#include "common/logging/text_formatter.h"

namespace Common::Log {

std::string FormatLogMessage(const Entry& entry) {
    const u32 time_seconds = static_cast<u32>(entry.timestamp.count() / 1000000);
    const u32 time_fractional = static_cast<u32>(entry.timestamp.count() % 1000000);

    const char* class_name = GetLogClassName(entry.log_class);
    const char* level_name = GetLevelName(entry.log_level);

    return fmt::format("[{}] <{}> {}:{} {}: {}", class_name, level_name, entry.filename,
                       entry.line_num, entry.function, entry.message);
}

void PrintMessage(const Entry& entry) {
    const auto str = FormatLogMessage(entry).append(1, '\n');
    fputs(str.c_str(), stdout);
}

void PrintColoredMessage(const Entry& entry) {

#define ESC "\x1b"
    const char* color = "";
    switch (entry.log_level) {
    case Level::Trace: // Grey
        color = ESC "[1;30m";
        break;
    case Level::Debug: // Cyan
        color = ESC "[0;36m";
        break;
    case Level::Info: // Bright gray
        color = ESC "[0;37m";
        break;
    case Level::Warning: // Bright yellow
        color = ESC "[1;33m";
        break;
    case Level::Error: // Bright red
        color = ESC "[1;31m";
        break;
    case Level::Critical: // Bright magenta
        color = ESC "[1;35m";
        break;
    case Level::Count:
        UNREACHABLE();
    }

    fputs(color, stdout);
    PrintMessage(entry);

    fputs(ESC "[0m", stdout);
#undef ESC
}

} // namespace Common::Log
