// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/enum.h"
#include "common/types.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/ajm/ajm_batch.h"

#include <boost/container/small_vector.hpp>

#include <memory>
#include <optional>
#include <tuple>

namespace Libraries::Ajm {

u8 GetPCMSize(AjmFormatEncoding format);

class SparseOutputBuffer {
public:
    SparseOutputBuffer(std::span<std::span<u8>> chunks)
        : m_chunks(chunks.begin(), chunks.end()) {
        for (const auto& chunk : m_chunks) {
            m_total_size += chunk.size();
        }
    }

    template <class T>
    size_t Write(std::span<T> pcm) {
        const auto samples_written = WriteAt(m_bytes_written, pcm);
        if (samples_written < pcm.size()) {
            LOG_ERROR(Lib_Ajm, "Could not write {} samples", pcm.size() - samples_written);
        }
        return samples_written;
    }

    template <class T>
    size_t WriteAt(size_t offset, std::span<T> pcm) {
        const auto pcm_size = pcm.size() * sizeof(T);
        size_t bytes_written = 0;
        size_t chunk_offset = 0;
        for (const auto& chunk : m_chunks) {
            if (bytes_written == pcm_size) {
                break;
            }
            if (offset + bytes_written >= chunk_offset + chunk.size()) {
                chunk_offset += chunk.size();
                continue;
            }
            const auto position = offset + bytes_written - chunk_offset;
            const auto size = std::min(pcm_size - bytes_written, chunk.size() - position);
            std::memcpy(chunk.data() + position,
                        reinterpret_cast<const u8*>(pcm.data()) + bytes_written, size);
            bytes_written += size;
            chunk_offset += chunk.size();
        }
        m_bytes_written += bytes_written;
        return bytes_written / sizeof(T);
    }

    size_t TotalSize() const {
        return m_total_size;
    }

    size_t Size() const {
        return m_total_size - m_bytes_written;
    }

private:
    boost::container::small_vector<std::span<u8>, 8> m_chunks;
    size_t m_total_size{};
    size_t m_bytes_written{};
};

struct AjmInstanceGapless {
    AjmSidebandGaplessDecode init{};
    AjmSidebandGaplessDecode current{};

    bool IsEnd() const {
        return init.total_samples != 0 && current.total_samples == 0;
    }

    void Reset() {
        current.total_samples = init.total_samples;
        current.skip_samples = init.skip_samples;
        current.skipped_samples = 0;
    }
};

struct DecoderResult {
    s32 result = 0;
    s32 internal_result = 0;
    u32 frames_decoded = 0;
    u32 samples_written = 0;
    bool is_reset = false;
};

class AjmCodec {
public:
    virtual ~AjmCodec() = default;

    virtual void Initialize(const void* buffer, u32 buffer_size) = 0;
    virtual void Reset() = 0;
    virtual void GetInfo(void* out_info) const = 0;
    virtual AjmSidebandFormat GetFormat() const = 0;
    virtual u32 GetMinimumInputSize() const = 0;
    virtual u32 GetNextFrameSize(const AjmInstanceGapless& gapless) const = 0;
    virtual DecoderResult ProcessData(std::span<u8>& input, SparseOutputBuffer& output,
                                      AjmInstanceGapless& gapless) = 0;
};

class AjmInstance {
public:
    AjmInstance(AjmCodecType codec_type, AjmInstanceFlags flags);

    void ExecuteJob(AjmJob& job);

private:
    bool HasEnoughSpace(const SparseOutputBuffer& output) const;
    void Reset();

    AjmInstanceFlags m_flags{};
    AjmSidebandFormat m_format{};
    AjmInstanceGapless m_gapless{};
    AjmSidebandResampleParameters m_resample_parameters{};
    u32 m_total_samples{};
    std::unique_ptr<AjmCodec> m_codec;
};

} // namespace Libraries::Ajm
