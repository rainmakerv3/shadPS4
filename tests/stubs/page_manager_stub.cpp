// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "tests/stubs/page_manager_stub.h"

namespace VideoCore {

std::vector<PageManagerStub::Call> PageManagerStub::calls;

struct PageManager::Impl {};

PageManager::PageManager(Vulkan::Rasterizer*) {}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr, size_t) {}

void PageManager::OnGpuUnmap(VAddr, size_t) {}

void PageManager::UpdatePageWatchers(VAddr, u64, PageOp) const {}

void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, const Bounds& bounds,
                                              const RegionBits& write_mask,
                                              const RegionBits& read_mask, PageOp write_op,
                                              PageOp read_op) const {
    const u64 page_start = bounds.start_word * PAGES_PER_WORD + bounds.start_page;
    const u64 page_end = bounds.end_word * PAGES_PER_WORD + bounds.end_page + 1;
    for (u64 page = page_start; page < page_end; ++page) {
        if (write_op != PageOp::None && write_mask.GetPage(page)) {
            PageManagerStub::calls.push_back({base_addr + page * BYTES_PER_PAGE, write_op});
        }
    }
}

} // namespace VideoCore
