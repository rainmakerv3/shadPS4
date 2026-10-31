// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_pipeline_compiler.h"

using Vulkan::PipelineCompileJob;
using Vulkan::PipelineCompiler;

TEST(PipelineCompilerTest, EveryJobRunsExactlyOnce) {
    constexpr int NumJobs = 2000;
    std::vector<std::atomic<int>> runs(NumJobs);
    std::vector<std::shared_ptr<PipelineCompileJob>> jobs;
    {
        PipelineCompiler compiler{4};
        for (int i = 0; i < NumJobs; ++i) {
            jobs.push_back(compiler.Submit([&runs, i] { runs[i].fetch_add(1); }));
        }
        // Waiting takes over jobs no worker has started, racing the workers for them.
        for (int i = NumJobs - 1; i >= 0; i -= 3) {
            jobs[i]->Wait();
        }
        for (auto& job : jobs) {
            job->Wait();
        }
    }
    for (int i = 0; i < NumJobs; ++i) {
        EXPECT_EQ(runs[i].load(), 1) << "job " << i;
        EXPECT_TRUE(jobs[i]->IsDone());
    }
}

TEST(PipelineCompilerTest, WaitRunsPendingJobOnCallingThread) {
    PipelineCompiler compiler{1};
    std::atomic<bool> release{false};
    auto blocker = compiler.Submit([&] {
        while (!release) {
            std::this_thread::yield();
        }
    });
    std::thread::id ran_on{};
    auto job = compiler.Submit([&] { ran_on = std::this_thread::get_id(); });

    // The only worker is busy, so waiting has to run the job here instead of queueing behind it.
    job->Wait();
    EXPECT_TRUE(job->IsDone());
    EXPECT_EQ(ran_on, std::this_thread::get_id());

    release = true;
    blocker->Wait();
}

TEST(PipelineCompilerTest, CancelDropsPendingJob) {
    PipelineCompiler compiler{1};
    std::atomic<bool> release{false};
    auto blocker = compiler.Submit([&] {
        while (!release) {
            std::this_thread::yield();
        }
    });
    std::atomic<int> runs{0};
    auto job = compiler.Submit([&] { runs++; });

    job->Cancel();
    EXPECT_TRUE(job->IsDone());
    release = true;
    blocker->Wait();
    EXPECT_EQ(runs.load(), 0);
}

TEST(PipelineCompilerTest, DestroyingCompilerFinishesOrDropsQueuedJobs) {
    std::vector<std::shared_ptr<PipelineCompileJob>> jobs;
    std::atomic<int> runs{0};
    {
        PipelineCompiler compiler{2};
        for (int i = 0; i < 1000; ++i) {
            jobs.push_back(compiler.Submit([&] { runs++; }));
        }
    }
    for (const auto& job : jobs) {
        EXPECT_TRUE(job->IsDone());
    }
    EXPECT_LE(runs.load(), 1000);
}

TEST(PipelineCompilerTest, NoWorkersRunsInline) {
    PipelineCompiler compiler{0};
    bool ran = false;
    auto job = compiler.Submit([&] { ran = true; });
    EXPECT_TRUE(ran);
    EXPECT_TRUE(job->IsDone());
}
