#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace voxinsert {

class WavWriter {
public:
    bool WritePcm16Mono(
        const std::vector<int16_t>& samples,
        int sampleRate,
        std::filesystem::path& wavPath,
        std::wstring& failureReason) const;
};

} // namespace voxinsert