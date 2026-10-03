// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "video_core/page_manager.h"

namespace VideoCore {

/// Records the write protection changes asked of the page manager, one entry per page.
struct PageManagerStub {
    struct Call {
        VAddr page;
        PageOp op;
    };
    static std::vector<Call> calls;
};

} // namespace VideoCore
