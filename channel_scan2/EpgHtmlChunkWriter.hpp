#pragma once
#ifndef EPG_HTML_CHUNK_WRITER_HPP
#define EPG_HTML_CHUNK_WRITER_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

class EpgHtmlChunkWriter final
{
public:
    struct Options
    {
        std::size_t flushThresholdBytes = 128u * 1024u;
        bool writeUtf8Bom = true;
        bool atomicReplace = true;
    };

    EpgHtmlChunkWriter() = default;
    ~EpgHtmlChunkWriter() noexcept;

    EpgHtmlChunkWriter(const EpgHtmlChunkWriter&) = delete;
    EpgHtmlChunkWriter& operator=(const EpgHtmlChunkWriter&) = delete;
    EpgHtmlChunkWriter(EpgHtmlChunkWriter&&) = delete;
    EpgHtmlChunkWriter& operator=(EpgHtmlChunkWriter&&) = delete;

    bool Open(const std::filesystem::path& outputPath, const Options& options = {});

    bool Append(std::wstring_view text);
    bool AppendLine(std::wstring_view text);
    bool AppendRawUtf8(std::string_view utf8);

    bool AppendEscaped(std::wstring_view text);
    bool AppendAttrEscaped(std::wstring_view text);

    bool Flush();
    bool Close();
    void Abort() noexcept;

    bool Good() const noexcept { return good_; }
    bool IsOpen() const noexcept { return opened_; }
    const std::filesystem::path& OutputPath() const noexcept { return outputPath_; }

private:
    bool WriteBom_();
    bool CommitAtomic_() noexcept;
    bool FlushWideBuffer_();

    static bool WideToUtf8Safe_(std::wstring_view src, std::string& out) noexcept;
    static std::wstring SanitizeUtf16_(std::wstring_view src);
    static std::filesystem::path MakeTempPath_(const std::filesystem::path& outputPath);
    static std::filesystem::path MakeBackupPath_(const std::filesystem::path& outputPath);

private:
    std::ofstream out_;
    std::filesystem::path outputPath_;
    std::filesystem::path writePath_;
    Options options_{};

    std::wstring wideBuffer_;
    std::size_t approxBytes_ = 0;

    bool opened_ = false;
    bool closed_ = false;
    bool good_ = false;
    bool committed_ = false;
};

#endif // EPG_HTML_CHUNK_WRITER_HPP
