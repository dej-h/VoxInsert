#include "audio/wav_writer.h"

#include "observability/logging.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>

namespace voxinsert {
namespace {

void WriteUint16(std::ofstream& stream, uint16_t value) {
    stream.put(static_cast<char>(value & 0xFF));
    stream.put(static_cast<char>((value >> 8) & 0xFF));
}

void WriteUint32(std::ofstream& stream, uint32_t value) {
    stream.put(static_cast<char>(value & 0xFF));
    stream.put(static_cast<char>((value >> 8) & 0xFF));
    stream.put(static_cast<char>((value >> 16) & 0xFF));
    stream.put(static_cast<char>((value >> 24) & 0xFF));
}

std::filesystem::path BuildRecordingPath(std::wstring& failureReason) {
    std::error_code errorCode;
    std::filesystem::path tempDirectory = std::filesystem::temp_directory_path(errorCode);
    if (errorCode) {
        failureReason = L"std::filesystem::temp_directory_path failed: ";
        failureReason += WideFromUtf8(errorCode.message());
        return {};
    }

    const std::filesystem::path recordingDirectory = tempDirectory / L"VoxInsert";
    std::filesystem::create_directories(recordingDirectory, errorCode);
    if (errorCode) {
        failureReason = L"Failed to create recording output directory: ";
        failureReason += WideFromUtf8(errorCode.message());
        return {};
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t filename[64]{};
    swprintf_s(
        filename,
        L"recording-%04u%02u%02u-%02u%02u%02u.wav",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond);

    return recordingDirectory / filename;
}

} // namespace

bool WavWriter::WritePcm16Mono(
    const std::vector<int16_t>& samples,
    int sampleRate,
    std::filesystem::path& wavPath,
    std::wstring& failureReason) const {
    if (samples.empty()) {
        failureReason = L"Cannot write a WAV file because no audio samples were captured.";
        return false;
    }

    if (sampleRate <= 0) {
        failureReason = L"Cannot write a WAV file because the sample rate is invalid.";
        return false;
    }

    wavPath = BuildRecordingPath(failureReason);
    if (wavPath.empty()) {
        return false;
    }

    std::ofstream output(wavPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        failureReason = L"Failed to open WAV output file at ";
        failureReason += wavPath.wstring();
        return false;
    }

    constexpr uint16_t channelCount = 1;
    constexpr uint16_t bitsPerSample = 16;
    const uint32_t byteRate = static_cast<uint32_t>(sampleRate * channelCount * (bitsPerSample / 8));
    const uint16_t blockAlign = static_cast<uint16_t>(channelCount * (bitsPerSample / 8));
    const uint32_t dataSizeBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));

    output.write("RIFF", 4);
    WriteUint32(output, 36u + dataSizeBytes);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    WriteUint32(output, 16u);
    WriteUint16(output, 1u);
    WriteUint16(output, channelCount);
    WriteUint32(output, static_cast<uint32_t>(sampleRate));
    WriteUint32(output, byteRate);
    WriteUint16(output, blockAlign);
    WriteUint16(output, bitsPerSample);
    output.write("data", 4);
    WriteUint32(output, dataSizeBytes);
    output.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(dataSizeBytes));

    if (!output.good()) {
        failureReason = L"Failed while writing WAV data to ";
        failureReason += wavPath.wstring();
        return false;
    }

    return true;
}

} // namespace voxinsert