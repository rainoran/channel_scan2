#include "EpgHtmlChunkWriter.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <system_error>

EpgHtmlChunkWriter::~EpgHtmlChunkWriter() noexcept
{
    // 出力完了は必ず Close() で明示する。
    // デストラクタでは成功扱いの commit は行わず、未完了の一時ファイルだけ片付ける。
    if (opened_ && !closed_) {
        Abort();
    }
}

bool EpgHtmlChunkWriter::Open(const std::filesystem::path& outputPath, const Options& options)
{
    if (opened_) {
        return false;
    }
    if (outputPath.empty()) {
        return false;
    }

    outputPath_ = outputPath;
    options_ = options;
    if (options_.flushThresholdBytes < 16u * 1024u) {
        options_.flushThresholdBytes = 16u * 1024u;
    }

    std::error_code ec;
    const std::filesystem::path parent = outputPath_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    writePath_ = options_.atomicReplace ? MakeTempPath_(outputPath_) : outputPath_;

    out_.open(writePath_, std::ios::binary | std::ios::trunc);
    if (!out_) {
        return false;
    }

    opened_ = true;
    closed_ = false;
    committed_ = false;
    good_ = true;
    wideBuffer_.clear();
    approxBytes_ = 0;

    if (options_.writeUtf8Bom && !WriteBom_()) {
        Abort();
        return false;
    }

    return true;
}

bool EpgHtmlChunkWriter::Append(std::wstring_view text)
{
    if (!opened_ || closed_ || !good_) {
        return false;
    }
    if (text.empty()) {
        return true;
    }

    try {
        wideBuffer_.append(text.data(), text.size());
        approxBytes_ += text.size() * sizeof(wchar_t);
    }
    catch (...) {
        good_ = false;
        return false;
    }

    if (approxBytes_ >= options_.flushThresholdBytes) {
        return FlushWideBuffer_();
    }
    return true;
}

bool EpgHtmlChunkWriter::AppendLine(std::wstring_view text)
{
    return Append(text) && Append(L"\n");
}

bool EpgHtmlChunkWriter::AppendRawUtf8(std::string_view utf8)
{
    if (!opened_ || closed_ || !good_) {
        return false;
    }

    if (!FlushWideBuffer_()) {
        return false;
    }

    if (!utf8.empty()) {
        out_.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        if (!out_) {
            good_ = false;
            return false;
        }
    }
    return true;
}

bool EpgHtmlChunkWriter::AppendEscaped(std::wstring_view text)
{
    if (text.empty()) {
        return true;
    }

    std::wstring out;
    out.reserve(text.size() + 32);

    for (const wchar_t ch : text) {
        switch (ch) {
        case L'&':  out += L"&amp;";  break;
        case L'<':  out += L"&lt;";   break;
        case L'>':  out += L"&gt;";   break;
        case L'\"': out += L"&quot;"; break;
        default:    out.push_back(ch); break;
        }
    }

    return Append(out);
}

bool EpgHtmlChunkWriter::AppendAttrEscaped(std::wstring_view text)
{
    if (text.empty()) {
        return true;
    }

    std::wstring out;
    out.reserve(text.size() + 32);

    for (const wchar_t ch : text) {
        switch (ch) {
        case L'&':  out += L"&amp;";  break;
        case L'<':  out += L"&lt;";   break;
        case L'>':  out += L"&gt;";   break;
        case L'\"': out += L"&quot;"; break;
        case L'\r':
        case L'\n':
        case L'\t': out.push_back(L' '); break;
        default:    out.push_back(ch); break;
        }
    }

    return Append(out);
}

bool EpgHtmlChunkWriter::Flush()
{
    if (!opened_ || closed_ || !good_) {
        return false;
    }
    if (!FlushWideBuffer_()) {
        return false;
    }
    out_.flush();
    if (!out_) {
        good_ = false;
        return false;
    }
    return true;
}

bool EpgHtmlChunkWriter::Close()
{
    if (!opened_) {
        return false;
    }
    if (closed_) {
        return committed_;
    }

    if (!FlushWideBuffer_()) {
        Abort();
        return false;
    }

    out_.flush();
    const bool streamOk = static_cast<bool>(out_);
    out_.close();
    closed_ = true;

    if (!streamOk) {
        good_ = false;
        Abort();
        return false;
    }

    if (options_.atomicReplace) {
        committed_ = CommitAtomic_();
    }
    else {
        committed_ = true;
    }

    good_ = committed_;
    return committed_;
}

void EpgHtmlChunkWriter::Abort() noexcept
{
    try {
        if (out_.is_open()) {
            out_.close();
        }
        if (options_.atomicReplace && !writePath_.empty()) {
            std::error_code ec;
            std::filesystem::remove(writePath_, ec);
        }
    }
    catch (...) {
    }

    closed_ = true;
    good_ = false;
    committed_ = false;
}

bool EpgHtmlChunkWriter::WriteBom_()
{
    static constexpr unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    out_.write(reinterpret_cast<const char*>(bom), 3);
    return static_cast<bool>(out_);
}

bool EpgHtmlChunkWriter::CommitAtomic_() noexcept
{
    try {
        std::error_code ec;
        const std::filesystem::path backupPath = MakeBackupPath_(outputPath_);

        std::filesystem::remove(backupPath, ec);
        ec.clear();

        if (std::filesystem::exists(outputPath_, ec)) {
            ec.clear();
            std::filesystem::rename(outputPath_, backupPath, ec);
            if (ec) {
                // rename が失敗する環境では、最終手段として削除してから置換する。
                ec.clear();
                std::filesystem::remove(outputPath_, ec);
                if (ec) {
                    return false;
                }
            }
        }

        ec.clear();
        std::filesystem::rename(writePath_, outputPath_, ec);
        if (ec) {
            // 失敗時は可能なら旧ファイルを戻す。
            std::error_code restoreEc;
            if (std::filesystem::exists(backupPath, restoreEc)) {
                std::filesystem::rename(backupPath, outputPath_, restoreEc);
            }
            return false;
        }

        ec.clear();
        std::filesystem::remove(backupPath, ec);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool EpgHtmlChunkWriter::FlushWideBuffer_()
{
    if (wideBuffer_.empty()) {
        approxBytes_ = 0;
        return true;
    }

    std::string utf8;
    if (!WideToUtf8Safe_(wideBuffer_, utf8)) {
        good_ = false;
        return false;
    }

    out_.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    if (!out_) {
        good_ = false;
        return false;
    }

    wideBuffer_.clear();
    approxBytes_ = 0;
    return true;
}

bool EpgHtmlChunkWriter::WideToUtf8Safe_(std::wstring_view src, std::string& out) noexcept
{
    out.clear();
    if (src.empty()) {
        return true;
    }

    std::wstring safe;
    std::wstring_view use = src;

    // 単独サロゲートがあると変換に失敗しやすいので、事前に潰す。
    for (const wchar_t ch : src) {
        if (ch >= 0xD800 && ch <= 0xDFFF) {
            safe = SanitizeUtf16_(src);
            use = safe;
            break;
        }
    }

    const int needed = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        use.data(),
        static_cast<int>(use.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (needed <= 0) {
        return false;
    }

    try {
        out.resize(static_cast<std::size_t>(needed));
    }
    catch (...) {
        return false;
    }

    const int written = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        use.data(),
        static_cast<int>(use.size()),
        out.data(),
        needed,
        nullptr,
        nullptr);

    return written == needed;
}

std::wstring EpgHtmlChunkWriter::SanitizeUtf16_(std::wstring_view src)
{
    std::wstring out;
    out.reserve(src.size());

    for (const wchar_t ch : src) {
        if (ch >= 0xD800 && ch <= 0xDFFF) {
            out.push_back(0xFFFD);
        }
        else {
            out.push_back(ch);
        }
    }

    return out;
}

std::filesystem::path EpgHtmlChunkWriter::MakeTempPath_(const std::filesystem::path& outputPath)
{
    std::filesystem::path tmp = outputPath;
    tmp += L".tmp";
    return tmp;
}

std::filesystem::path EpgHtmlChunkWriter::MakeBackupPath_(const std::filesystem::path& outputPath)
{
    std::filesystem::path bak = outputPath;
    bak += L".bak";
    return bak;
}
