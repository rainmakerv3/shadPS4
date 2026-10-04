// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>
#include <queue>

#include <boost/container/static_vector.hpp>

#include "common/interval_set.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_command_recorder.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "vulkan/vulkan.hpp"

namespace tracy {
class VkCtxScope;
}

namespace Vulkan {

class Instance;

struct RenderAttachment {
    vk::ImageView image_view;
    vk::ImageLayout image_layout;
    std::array<u32, 4> clear_value;
    union {
        u32 is_clear;
        struct {
            bool has_depth;
            bool depth_clear;
            bool has_stencil;
            bool stencil_clear;
        };
    };
};
static_assert(std::has_unique_object_representations_v<RenderAttachment>);

struct RenderState {
    std::array<RenderAttachment, 8> color_attachments;
    RenderAttachment depth_stencil_attachment;
    u16 width;
    u16 height;
    u16 num_layers;
    u16 num_color_attachments;

    bool operator==(const RenderState& other) const noexcept {
        return std::memcmp(this, &other, sizeof(RenderState)) == 0;
    }
};
static_assert(std::has_unique_object_representations_v<RenderState>);

struct SubmitInfo {
    std::array<vk::Semaphore, 4> wait_semas;
    std::array<u64, 4> wait_ticks;
    std::array<vk::Semaphore, 4> signal_semas;
    std::array<u64, 4> signal_ticks;
    vk::Fence fence;
    u32 num_wait_semas;
    u32 num_signal_semas;

    void AddWait(vk::Semaphore semaphore, u64 tick = 1) {
        wait_semas[num_wait_semas] = semaphore;
        wait_ticks[num_wait_semas++] = tick;
    }

    void AddSignal(vk::Semaphore semaphore, u64 tick = 1) {
        signal_semas[num_signal_semas] = semaphore;
        signal_ticks[num_signal_semas++] = tick;
    }

    void AddSignal(vk::Fence fence) {
        this->fence = fence;
    }
};

using Viewports = boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS>;
using Scissors = boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS>;
using ColorWriteMasks = std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS>;
struct StencilOps {
    vk::StencilOp fail_op{};
    vk::StencilOp pass_op{};
    vk::StencilOp depth_fail_op{};
    vk::CompareOp compare_op{};

    bool operator==(const StencilOps& other) const {
        return fail_op == other.fail_op && pass_op == other.pass_op &&
               depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
    }
};
struct DynamicState {
    struct {
        bool viewports : 1;
        bool scissors : 1;

        bool depth_test_enabled : 1;
        bool depth_write_enabled : 1;
        bool depth_compare_op : 1;

        bool depth_bounds_test_enabled : 1;
        bool depth_bounds : 1;

        bool depth_bias_enabled : 1;
        bool depth_bias : 1;

        bool stencil_test_enabled : 1;
        bool stencil_front_ops : 1;
        bool stencil_front_reference : 1;
        bool stencil_front_write_mask : 1;
        bool stencil_front_compare_mask : 1;
        bool stencil_back_ops : 1;
        bool stencil_back_reference : 1;
        bool stencil_back_write_mask : 1;
        bool stencil_back_compare_mask : 1;

        bool primitive_restart_enable : 1;
        bool rasterizer_discard_enable : 1;
        bool cull_mode : 1;
        bool front_face : 1;

        bool blend_constants : 1;
        bool color_write_masks : 1;
        bool line_width : 1;
        bool feedback_loop_enabled : 1;
        /// Set by the rasterizer, which keeps the vertex input it last set.
        bool vertex_input : 1;
        bool graphics_pipeline : 1;
        bool compute_pipeline : 1;
        bool graphics_push_constants : 1;
        bool compute_push_constants : 1;
    } dirty_state{};

    Viewports viewports{};
    Scissors scissors{};

    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};

    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};

    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};

    bool stencil_test_enabled{};
    StencilOps stencil_front_ops{};
    u32 stencil_front_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_front_compare_mask{};
    StencilOps stencil_back_ops{};
    u32 stencil_back_reference{};
    u32 stencil_back_write_mask{};
    u32 stencil_back_compare_mask{};

    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};

    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    bool feedback_loop_enabled{};

    vk::Pipeline graphics_pipeline{};
    vk::Pipeline compute_pipeline{};
    /// Push constants last pushed for graphics and for compute stages. Pipelines of each kind all
    /// have the same range of them, and stages keep the ones last pushed for them.
    static constexpr size_t MaxPushConstantsSize = 128;
    std::array<u8, MaxPushConstantsSize> graphics_push_constants{};
    std::array<u8, MaxPushConstantsSize> compute_push_constants{};

    /// Commits the dynamic state to the provided command buffer.
    void Commit(const Instance& instance, const CommandRecorder& cmdbuf);

    /// Invalidates all dynamic state to be flushed into the next command buffer.
    void Invalidate() {
        std::memset(&dirty_state, 0xFF, sizeof(dirty_state));
    }

    void SetViewports(const Viewports& viewports_) {
        if (!std::ranges::equal(viewports, viewports_)) {
            viewports = viewports_;
            dirty_state.viewports = true;
        }
    }

    void SetScissors(const Scissors& scissors_) {
        if (!std::ranges::equal(scissors, scissors_)) {
            scissors = scissors_;
            dirty_state.scissors = true;
        }
    }

    void SetDepthTestEnabled(const bool enabled) {
        if (depth_test_enabled != enabled) {
            depth_test_enabled = enabled;
            dirty_state.depth_test_enabled = true;
        }
    }

    void SetDepthWriteEnabled(const bool enabled) {
        if (depth_write_enabled != enabled) {
            depth_write_enabled = enabled;
            dirty_state.depth_write_enabled = true;
        }
    }

    void SetDepthCompareOp(const vk::CompareOp compare_op) {
        if (depth_compare_op != compare_op) {
            depth_compare_op = compare_op;
            dirty_state.depth_compare_op = true;
        }
    }

    void SetDepthBoundsTestEnabled(const bool enabled) {
        if (depth_bounds_test_enabled != enabled) {
            depth_bounds_test_enabled = enabled;
            dirty_state.depth_bounds_test_enabled = true;
        }
    }

    void SetDepthBounds(const float min, const float max) {
        if (depth_bounds_min != min || depth_bounds_max != max) {
            depth_bounds_min = min;
            depth_bounds_max = max;
            dirty_state.depth_bounds = true;
        }
    }

    void SetDepthBiasEnabled(const bool enabled) {
        if (depth_bias_enabled != enabled) {
            depth_bias_enabled = enabled;
            dirty_state.depth_bias_enabled = true;
        }
    }

    void SetDepthBias(const float constant, const float clamp, const float slope) {
        if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
            depth_bias_slope != slope) {
            depth_bias_constant = constant;
            depth_bias_clamp = clamp;
            depth_bias_slope = slope;
            dirty_state.depth_bias = true;
        }
    }

    void SetStencilTestEnabled(const bool enabled) {
        if (stencil_test_enabled != enabled) {
            stencil_test_enabled = enabled;
            dirty_state.stencil_test_enabled = true;
        }
    }

    void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
        if (stencil_front_ops != front_ops) {
            stencil_front_ops = front_ops;
            dirty_state.stencil_front_ops = true;
        }
        if (stencil_back_ops != back_ops) {
            stencil_back_ops = back_ops;
            dirty_state.stencil_back_ops = true;
        }
    }

    void SetStencilReferences(const u32 front_reference, const u32 back_reference) {
        if (stencil_front_reference != front_reference) {
            stencil_front_reference = front_reference;
            dirty_state.stencil_front_reference = true;
        }
        if (stencil_back_reference != back_reference) {
            stencil_back_reference = back_reference;
            dirty_state.stencil_back_reference = true;
        }
    }

    void SetStencilWriteMasks(const u32 front_write_mask, const u32 back_write_mask) {
        if (stencil_front_write_mask != front_write_mask) {
            stencil_front_write_mask = front_write_mask;
            dirty_state.stencil_front_write_mask = true;
        }
        if (stencil_back_write_mask != back_write_mask) {
            stencil_back_write_mask = back_write_mask;
            dirty_state.stencil_back_write_mask = true;
        }
    }

    void SetStencilCompareMasks(const u32 front_compare_mask, const u32 back_compare_mask) {
        if (stencil_front_compare_mask != front_compare_mask) {
            stencil_front_compare_mask = front_compare_mask;
            dirty_state.stencil_front_compare_mask = true;
        }
        if (stencil_back_compare_mask != back_compare_mask) {
            stencil_back_compare_mask = back_compare_mask;
            dirty_state.stencil_back_compare_mask = true;
        }
    }

    void SetPrimitiveRestartEnabled(const bool enabled) {
        if (primitive_restart_enable != enabled) {
            primitive_restart_enable = enabled;
            dirty_state.primitive_restart_enable = true;
        }
    }

    void SetCullMode(const vk::CullModeFlags cull_mode_) {
        if (cull_mode != cull_mode_) {
            cull_mode = cull_mode_;
            dirty_state.cull_mode = true;
        }
    }

    void SetFrontFace(const vk::FrontFace front_face_) {
        if (front_face != front_face_) {
            front_face = front_face_;
            dirty_state.front_face = true;
        }
    }

    void SetBlendConstants(const std::array<float, 4> blend_constants_) {
        if (blend_constants != blend_constants_) {
            blend_constants = blend_constants_;
            dirty_state.blend_constants = true;
        }
    }

    void SetRasterizerDiscardEnabled(const bool enabled) {
        if (rasterizer_discard_enable != enabled) {
            rasterizer_discard_enable = enabled;
            dirty_state.rasterizer_discard_enable = true;
        }
    }

    void SetColorWriteMasks(const ColorWriteMasks& color_write_masks_) {
        if (!std::ranges::equal(color_write_masks, color_write_masks_)) {
            color_write_masks = color_write_masks_;
            dirty_state.color_write_masks = true;
        }
    }

    void SetLineWidth(const float width) {
        if (line_width != width) {
            line_width = width;
            dirty_state.line_width = true;
        }
    }

    void SetAttachmentFeedbackLoopEnabled(const bool enabled) {
        if (feedback_loop_enabled != enabled) {
            feedback_loop_enabled = enabled;
            dirty_state.feedback_loop_enabled = true;
        }
    }

    /// Binds a graphics pipeline unless it is bound already. Draws mostly use the pipeline the
    /// draw before did, and binding it again for each took a good part of their driver time.
    void BindGraphicsPipeline(const CommandRecorder& cmdbuf, vk::Pipeline pipeline) {
        if (dirty_state.graphics_pipeline || graphics_pipeline != pipeline) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
            graphics_pipeline = pipeline;
            dirty_state.graphics_pipeline = false;
        }
    }

    /// The same for compute pipelines, which every compute pipeline bound in the command buffer
    /// has to be bound with, as dispatches in a row often use the same.
    void BindComputePipeline(const CommandRecorder& cmdbuf, vk::Pipeline pipeline) {
        if (dirty_state.compute_pipeline || compute_pipeline != pipeline) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            compute_pipeline = pipeline;
            dirty_state.compute_pipeline = false;
        }
    }

    /// Pushes constants for all graphics or all compute stages, unless the same were pushed for
    /// them last. They mostly are, draw after draw.
    void PushConstants(const CommandRecorder& cmdbuf, vk::PipelineLayout layout, bool compute,
                       vk::ShaderStageFlags stages, const void* data, u32 size) {
        auto& last = compute ? compute_push_constants : graphics_push_constants;
        const bool dirty =
            compute ? dirty_state.compute_push_constants : dirty_state.graphics_push_constants;
        if (size > last.size() || dirty || std::memcmp(last.data(), data, size) != 0) {
            cmdbuf.pushConstants(layout, stages, 0u, size, data);
            if (size > last.size()) {
                return;
            }
            std::memcpy(last.data(), data, size);
            if (compute) {
                dirty_state.compute_push_constants = false;
            } else {
                dirty_state.graphics_push_constants = false;
            }
        }
    }
};

using SessionFunc = Common::UniqueFunction<void>;
using SubmitFunc = Common::UniqueFunction<void, SubmitInfo&>;

class Scheduler {
public:
    /// With record_on_thread, the Vulkan commands are recorded and submitted on a thread of the
    /// scheduler's own, from a stream of them the thread using the scheduler fills.
    explicit Scheduler(const Instance& instance, bool record_on_thread = false);
    ~Scheduler();

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush(SubmitInfo& info);

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush();

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish();

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick);

    /// Waits until the work up to the given tick, which was flushed, is submitted to the GPU. Work
    /// submitted to the same queue that waits for it has to come after it, or the queue would
    /// wait for work behind it. Without a recording thread, flushed work is submitted right away.
    void WaitSubmitted(u64 tick);

    /// Attempts to execute operations whose tick the GPU has caught up with.
    void PopPendingOperations();

    /// Starts a new rendering scope with provided state.
    void BeginRendering(const RenderState& new_state);

    /// Ends current rendering scope.
    void EndRendering();

    /// Starts a new session.
    void BeginSession();

    /// Returns the command buffer for uploads, which runs before the current one, between
    /// barriers that order it after all work before and before all work after.
    CommandRecorder UploadCommandBuffer();

    /// Sets a function to be called on every session finalization.
    void SetSessionCallback(SessionFunc&& on_session) {
        this->on_session = std::move(on_session);
    }

    /// Sets a function to be called on every scheduler submission.
    void SetSubmitCallback(SubmitFunc&& on_submit) {
        this->on_submit = std::move(on_submit);
    }

    /// Returns the current render state.
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// Counts a draw or dispatch recorded since the last submission, and lets the recording
    /// thread see the commands recorded so far.
    void CountWork() {
        ++work_since_submit;
        if (stream) {
            stream->Publish(false);
        }
    }

    /// Submits what was recorded so far if there is a lot of it and the next draw renders to
    /// other targets, so it starts a render pass anyway. The GPU then works on it while the rest
    /// of the game's commands are recorded, instead of idling until all of them are, and work
    /// the game waits for, such as readbacks, is done that much sooner.
    void SubmitIfWorkPiledUp(const RenderState& next_state) {
        static constexpr u32 PiledUpWork = 1024;
        if (work_since_submit >= PiledUpWork && !(is_rendering && render_state == next_state)) {
            Flush();
        }
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        return dynamic_state;
    }

    /// Returns the current command buffer.
    CommandRecorder CommandBuffer() const {
        if (stream) {
            return CommandRecorder{*stream, CommandTarget::Primary};
        }
        return CommandRecorder{direct_context.primary};
    }

    /// Identifies the command buffer being recorded.
    [[nodiscard]] u64 SessionId() const noexcept {
        return session_id;
    }

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return work_semaphore.CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (work_semaphore.IsFree(tick)) {
            return true;
        }
        work_semaphore.Refresh();
        return work_semaphore.IsFree(tick);
    }

    /// Returns the scheduler timeline semaphore.
    [[nodiscard]] Semaphore* GetWorkSemaphore() noexcept {
        return &work_semaphore;
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        std::unique_lock lk(pending_ops_mutex);
        pending_ops.emplace(std::move(func), CurrentTick());
        num_pending_ops.store(pending_ops.size(), std::memory_order_release);
    }

    /// Measures the time the GPU spends on the command buffers submitted from now on, for the
    /// perf summary.
    void MeasureGpuTime();

    /// Draws, dispatches, barriers, render passes, switches between draws and dispatches and
    /// barriers right before them.
    using CostCounts = std::array<u64, 6>;

    /// Notes that the work recorded from now on is dispatches, or draws, where the work before
    /// it was the other kind. The GPU time of each run of them is measured, for the perf summary.
    void MarkWorkRun(bool compute);

    /// Where a dispatch is timed on its own, in every few command buffers, for the perf summary:
    /// before the barriers and the end of the render pass that come with it, right before it and
    /// right after it. Each mark waits for all work before it, so the dispatch and what came
    /// before it are timed apart.
    enum class DispatchMark : u32 {
        Begin,
        Start,
        End,
    };

    /// Marks a dispatch, given the program it runs, its workgroups if known and whether draws
    /// came before it.
    void MarkDispatch(DispatchMark mark, u64 program_hash = 0, u32 groups = 0,
                      bool after_draws = false);

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Runs as soon as possible in another thread.
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), CurrentTick());
        }
        priority_pending_ops_cv.notify_one();
    }

    static std::mutex submit_mutex;

private:
    static constexpr u32 RunMarksPerPair = 32;
    using RunMarks = boost::container::static_vector<bool, RunMarksPerPair>;

    void EndSession();

    void SubmitExecution(SubmitInfo& info);

    /// Runs a function of the command buffers being recorded where they are recorded: right away,
    /// or on the recording thread after the commands recorded before.
    template <typename Func>
    void Run(Func&& func) {
        if (stream) {
            stream->Emit(std::forward<Func>(func));
        } else {
            func(direct_context);
        }
    }

    /// Submits the command buffers ended since the last submission. Called where they are
    /// recorded, with the submit mutex held.
    void SubmitRecorded(const SubmitInfo& info, u64 signal_value);

    void RecordingThread(std::stop_token stoken);

    void PriorityPendingOpsThread(std::stop_token stoken);

    /// Counts the time the GPU spent on submitted command buffers that are done, from timestamps
    /// written at their start and end, for the perf summary.
    void CollectGpuTimes();

    /// Counts the GPU time of the runs of draws and of dispatches in a command buffer, between
    /// start and end.
    void CountWorkRuns(u32 pair, u64 start, u64 end, bool first_run_compute,
                       const RunMarks& run_marks);

    /// Adds the GPU time of a command buffer to the fit of its cost to the work in it, and logs
    /// the fit every few seconds.
    void FitGpuCost(double gpu_us, const CostCounts& counts);

    /// Counts the dispatches timed one by one in a command buffer that is done, and logs where
    /// their time went every few seconds.
    void CollectDispatchTimes();

private:
    const Instance& instance;
    Semaphore work_semaphore;
    CommandPool command_pool;
    DynamicState dynamic_state;
    SessionFunc on_session{};
    SubmitFunc on_submit{};
    struct Session {
        /// Whether anything was recorded into the upload command buffer.
        bool has_upload{};
        /// The pair of timestamp queries written around the primary command buffer, if any.
        u32 timestamp_pair = NoTimestamps;
        /// The work counted when it began, and the thread that counted it.
        CostCounts counts{};
        std::thread::id thread{};
        /// Whether the run of work the command buffer began in was dispatches.
        bool first_run_compute{};
        /// Where runs of the other kind of work began in it, with timestamps written there, and
        /// whether more began than could be marked.
        RunMarks run_marks;
        bool run_marks_overflow{};
        /// Whether its dispatches are timed one by one.
        bool time_dispatches{};
    };
    std::vector<Session> sessions;
    u64 session_id{};
    static constexpr u32 NoTimestamps = ~0u;
    static constexpr u32 NumTimestampPairs = 256;
    /// Timestamp queries, two for each command buffer measured, used in turn.
    vk::UniqueQueryPool timestamp_pool;
    /// Timestamp queries where runs of draws and dispatches begin, this many for each pair.
    vk::UniqueQueryPool run_pool;
    double timestamp_period_ns{};
    u64 timestamp_mask{};
    u32 next_timestamp_pair{};
    /// Whether the work recorded last was dispatches.
    bool run_compute{};
    struct SubmittedTimestamps {
        u64 tick;
        u32 pair;
        /// What the command buffer held, or nothing known if counted on several threads.
        std::optional<CostCounts> counts;
        bool first_run_compute{};
        RunMarks run_marks;
        bool run_marks_overflow{};
    };
    /// Submitted pairs whose results aren't read yet, in the order they were submitted.
    std::deque<SubmittedTimestamps> submitted_timestamps;
    /// The latest end of GPU work counted, so overlapping command buffers aren't counted twice.
    u64 counted_gpu_end{};
    /// Sums for a least squares fit of the GPU time of command buffers to the work counted in
    /// them and a cost of their own.
    static constexpr size_t NumCostTerms = std::tuple_size_v<CostCounts> + 1;
    std::array<std::array<double, NumCostTerms>, NumCostTerms> cost_xx{};
    std::array<double, NumCostTerms> cost_xy{};
    double cost_yy{};
    u64 cost_samples{};
    std::chrono::steady_clock::time_point last_cost_report{};

    /// One command buffer in this many has its dispatches timed one by one. Their marks keep
    /// the GPU from overlapping them with other work, so few are.
    static constexpr u64 TimedDispatchInterval = 64;
    static constexpr u32 MaxTimedDispatches = 1024;
    /// Three timestamps for each dispatch timed.
    vk::UniqueQueryPool dispatch_pool;
    struct TimedDispatch {
        u64 program_hash;
        u32 groups;
        bool after_draws;
        /// Which of its marks were written.
        u32 marks;
    };
    /// The dispatches timed in the command buffer being recorded, and in the one submitted
    /// whose times aren't read yet, with its tick.
    std::vector<TimedDispatch> timed_dispatches;
    std::vector<TimedDispatch> submitted_dispatches;
    u64 submitted_dispatches_tick{};
    struct DispatchTimes {
        u64 dispatch_ns;
        u64 count;
        u64 groups;
    };
    /// Since the last report: the time of the dispatches of each program, and of what came
    /// before dispatches after draws and after other dispatches.
    std::unordered_map<u64, DispatchTimes> dispatch_times;
    std::array<DispatchTimes, 2> dispatch_times_by_kind{};
    std::array<u64, 2> before_dispatch_ns{};
    u64 timed_command_buffers{};
    std::chrono::steady_clock::time_point last_dispatch_report{};
    std::condition_variable_any event_cv;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    /// The size of pending_ops, checked for every draw without taking the lock.
    std::atomic<size_t> num_pending_ops{};
    std::queue<PendingOp> priority_pending_ops;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    std::jthread priority_pending_ops_thread;
    RenderState render_state;
    bool is_rendering = false;
    u32 work_since_submit = 0;
    tracy::VkCtxScope* profiler_scope{};

    /// Commands for the recording thread, if there is one.
    std::unique_ptr<CommandStream> stream;
    /// The command buffers being recorded when they are recorded right away.
    RecordingContext direct_context;
    /// Command buffers ended and not submitted yet, where they are recorded.
    std::vector<vk::CommandBuffer> recorded;
    /// The last tick the recording thread submitted work for.
    std::atomic<u64> submitted_tick{};
    std::jthread recording_thread;
};

} // namespace Vulkan
