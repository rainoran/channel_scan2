#ifndef DEBUG_MPMC_LOG_BUFFER_HPP
#define DEBUG_MPMC_LOG_BUFFER_HPP
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <new>
#include <utility>
#include <memory>
#include <filesystem>
#include <cwchar>

class DebugMPMCLogBuffer {
public:
    explicit DebugMPMCLogBuffer(
        size_t capacityPow2,
        std::wstring filePath = L"debug.log") noexcept
        : m_path(std::move(filePath))
        , m_running(false)
    {
        init_queue(capacityPow2);
    }

    void PrepareOutput() noexcept
    {
        if (!HasFileOutput()) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_fileMutex);
        (void)EnsureOutputReady_();
    }

    void Start() noexcept
    {
        if (!HasFileOutput()) {
            return;
        }

        PrepareOutput();

        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        try {
            m_writer = std::thread([this]() noexcept { writer_loop(); });
        }
        catch (...) {
            m_running.store(false, std::memory_order_release);
            ::OutputDebugStringW(L"[DebugMPMCLogBuffer] writer thread start failed\n");
        }
    }

    void Stop() noexcept
    {
        bool expected = true;
        if (m_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            {
                std::lock_guard<std::mutex> lk(m_cv_m);
                m_cv.notify_all();
            }

            if (m_writer.joinable()) {
                try {
                    m_writer.join();
                }
                catch (...) {
                }
            }
        }

        FlushToFile();
    }

    ~DebugMPMCLogBuffer()
    {
        // 手動 Stop() 前提
    }

    void Flush() noexcept
    {
        FlushToFile();
    }

    void Flush(bool /*force*/) noexcept
    {
        FlushToFile();
    }

    bool PushLog(std::wstring_view msg) noexcept
    {
        if (!m_slots || !HasFileOutput()) {
            return false;
        }

        if (!m_running.load(std::memory_order_acquire)) {
            Start();
        }

        if (!m_running.load(std::memory_order_acquire)) {
            return DirectAppendOne_(msg);
        }

        size_t pos = m_enqueuePos.load(std::memory_order_relaxed);

        for (;;) {
            Slot* slot = &m_slots[pos & m_mask];
            const size_t seq = slot->seq.load(std::memory_order_acquire);
            const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (dif == 0) {
                if (m_enqueuePos.compare_exchange_weak(
                    pos,
                    pos + 1,
                    std::memory_order_relaxed))
                {
                    try {
                        slot->value.assign(msg.data(), msg.size());
                    }
                    catch (...) {
                        slot->value.clear();
                        return DirectAppendOne_(msg);
                    }

                    slot->seq.store(pos + 1, std::memory_order_release);

                    {
                        std::lock_guard<std::mutex> lk(m_cv_m);
                        m_cv.notify_one();
                    }
                    return true;
                }
            }
            else if (dif < 0) {
                m_dropped.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(m_cv_m);
                    m_cv.notify_one();
                }
                return false;
            }
            else {
                pos = m_enqueuePos.load(std::memory_order_relaxed);
            }
        }
    }

    bool TryPop(std::wstring& out) noexcept
    {
        return try_pop(out);
    }

    void FlushToFile() noexcept
    {
        if (!HasFileOutput()) {
            return;
        }

        try {
            std::lock_guard<std::mutex> lock(m_fileMutex);

            if (!EnsureOutputReady_()) {
                return;
            }

            bool wroteAny = false;
            std::wstring line;

            while (try_pop(line)) {
                if (!AppendWideLineUtf8Unlocked_(line)) {
                    ::OutputDebugStringW(L"[DebugMPMCLogBuffer] FlushToFile append failed\n");
                    break;
                }
                wroteAny = true;
            }

            const std::uint64_t dropped = m_dropped.exchange(0, std::memory_order_relaxed);
            if (dropped > 0) {
                wchar_t buf[160]{};
                swprintf_s(
                    buf,
                    L"[DEBUG] dropped lines: %llu",
                    static_cast<unsigned long long>(dropped));
                (void)AppendWideLineUtf8Unlocked_(buf);
                wroteAny = true;
            }

            if (wroteAny) {
                RotateIfNeeded_();
            }
        }
        catch (...) {
            ::OutputDebugStringW(L"[DebugMPMCLogBuffer] FlushToFile exception\n");
        }
    }

    uint64_t ConsumeDroppedCount() noexcept
    {
        return m_dropped.exchange(0, std::memory_order_relaxed);
    }

    void SetIdleWait(std::chrono::milliseconds ms) noexcept
    {
        m_idleWait = ms;
    }

    size_t ApproxSize() const noexcept
    {
        const size_t deq = m_dequeuePos.load(std::memory_order_acquire);
        const size_t enq = m_enqueuePos.load(std::memory_order_acquire);
        return enq - deq;
    }

    size_t Capacity() const noexcept
    {
        return m_capacity;
    }

    bool HasFileOutput() const noexcept
    {
        return !m_path.empty();
    }

private:
    struct Slot {
        std::atomic<size_t> seq;
        std::wstring value;

        Slot() noexcept
            : seq(0)
            , value()
        {
        }
    };

    static void OutputPathError_(const wchar_t* label, const std::filesystem::path& p) noexcept
    {
        try {
            wchar_t buf[1024]{};
            const DWORD err = ::GetLastError();
            swprintf_s(
                buf,
                L"[DebugMPMCLogBuffer] %ls failed. path=%ls gle=%lu\n",
                label,
                p.c_str(),
                static_cast<unsigned long>(err));
            ::OutputDebugStringW(buf);
        }
        catch (...) {
            ::OutputDebugStringW(L"[DebugMPMCLogBuffer] path error reporting failed\n");
        }
    }

    void init_queue(size_t capacityPow2) noexcept
    {
        if (capacityPow2 < 2 || (capacityPow2 & (capacityPow2 - 1)) != 0) {
            capacityPow2 = 1024;
        }

        m_capacity = capacityPow2;
        m_mask = capacityPow2 - 1;

        try {
            std::unique_ptr<Slot[]> buf{ new (std::nothrow) Slot[capacityPow2] };
            if (!buf) {
                m_slots = nullptr;
                m_capacity = 0;
                m_mask = 0;
            }
            else {
                m_storage = std::move(buf);
                m_slots = m_storage.get();
                for (size_t i = 0; i < capacityPow2; ++i) {
                    m_slots[i].seq.store(i, std::memory_order_relaxed);
                }
            }
        }
        catch (...) {
            m_storage.reset();
            m_slots = nullptr;
            m_capacity = 0;
            m_mask = 0;
        }

        m_enqueuePos.store(0, std::memory_order_relaxed);
        m_dequeuePos.store(0, std::memory_order_relaxed);
        m_dropped.store(0, std::memory_order_relaxed);
    }

    bool try_pop(std::wstring& out) noexcept
    {
        if (!m_slots) {
            return false;
        }

        size_t pos = m_dequeuePos.load(std::memory_order_relaxed);

        for (;;) {
            Slot* slot = &m_slots[pos & m_mask];
            const size_t seq = slot->seq.load(std::memory_order_acquire);
            const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (dif == 0) {
                if (m_dequeuePos.compare_exchange_weak(
                    pos,
                    pos + 1,
                    std::memory_order_relaxed))
                {
                    try {
                        out = std::move(slot->value);
                        slot->value.clear();
                    }
                    catch (...) {
                        out.clear();
                    }

                    slot->seq.store(pos + m_capacity, std::memory_order_release);
                    return true;
                }
            }
            else if (dif < 0) {
                return false;
            }
            else {
                pos = m_dequeuePos.load(std::memory_order_relaxed);
            }
        }
    }

    std::filesystem::path GetOutputDir_() const noexcept
    {
        try {
            wchar_t modulePath[MAX_PATH]{};
            const DWORD n = ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                return std::filesystem::path(modulePath).parent_path() / L"debug";
            }
        }
        catch (...) {
        }

        return std::filesystem::current_path() / L"debug";
    }

    std::wstring GetStemName_() const noexcept
    {
        try {
            const std::filesystem::path p(m_path);
            std::wstring stem = p.stem().wstring();
            if (stem.empty()) {
                stem = p.filename().wstring();
            }
            if (stem.empty()) {
                stem = L"debug";
            }
            return stem;
        }
        catch (...) {
            return L"debug";
        }
    }

    static unsigned ExtractIndex_(const std::filesystem::path& path, const std::wstring& stem) noexcept
    {
        try {
            const std::wstring name = path.filename().wstring();
            constexpr wchar_t suffix[] = L".log";

            if (name.size() <= stem.size() + 4 + wcslen(suffix)) {
                return 0;
            }

            if (name.rfind(stem, 0) != 0) {
                return 0;
            }

            if (name.substr(name.size() - wcslen(suffix)) != suffix) {
                return 0;
            }

            const std::wstring numPart =
                name.substr(stem.size(), name.size() - stem.size() - wcslen(suffix));

            if (numPart.size() != 4) {
                return 0;
            }

            for (wchar_t ch : numPart) {
                if (ch < L'0' || ch > L'9') {
                    return 0;
                }
            }

            return static_cast<unsigned>(std::stoul(numPart));
        }
        catch (...) {
            return 0;
        }
    }

    bool EnsureOutputReady_() noexcept
    {
        if (m_outputReady) {
            return true;
        }

        if (!HasFileOutput()) {
            return false;
        }

        const std::filesystem::path outDir = GetOutputDir_();

        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        if (ec) {
            ::OutputDebugStringW(L"[DebugMPMCLogBuffer] create_directories failed\n");
            return false;
        }

        const std::wstring stem = GetStemName_();

        unsigned maxIndex = 0;
        std::uintmax_t lastSize = 0;

        for (const auto& entry : std::filesystem::directory_iterator(outDir, ec)) {
            if (ec) {
                break;
            }

            if (!entry.is_regular_file()) {
                continue;
            }

            const unsigned idx = ExtractIndex_(entry.path(), stem);
            if (idx >= maxIndex) {
                maxIndex = idx;
                std::error_code sec;
                lastSize = std::filesystem::file_size(entry.path(), sec);
                if (sec) {
                    lastSize = 0;
                }
            }
        }

        if (maxIndex == 0) {
            m_currentIndex = 1;
        }
        else if (lastSize >= m_maxFileBytes) {
            m_currentIndex = maxIndex + 1;
        }
        else {
            m_currentIndex = maxIndex;
        }

        if (!EnsureFileExistsWithBomUnlocked_()) {
            m_outputReady = false;
            return false;
        }

        m_outputReady = true;
        return true;
    }

    std::filesystem::path GetCurrentLogPath_() const noexcept
    {
        wchar_t fileName[128]{};
        swprintf_s(fileName, L"%ls%04u.log", GetStemName_().c_str(), m_currentIndex);
        return GetOutputDir_() / fileName;
    }

    void RotateIfNeeded_() noexcept
    {
        try {
            const std::filesystem::path p = GetCurrentLogPath_();
            std::error_code ec;
            const std::uintmax_t size = std::filesystem::file_size(p, ec);

            if (!ec && size >= m_maxFileBytes) {
                ++m_currentIndex;
                m_outputReady = false;
                (void)EnsureOutputReady_();
            }
        }
        catch (...) {
        }
    }

    static bool WideToUtf8_(std::wstring_view src, std::string& out) noexcept
    {
        out.clear();

        if (src.empty()) {
            return true;
        }

        const int needed = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            src.data(),
            static_cast<int>(src.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (needed <= 0) {
            return false;
        }

        out.resize(static_cast<size_t>(needed));

        const int written = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            src.data(),
            static_cast<int>(src.size()),
            out.data(),
            needed,
            nullptr,
            nullptr);

        return written == needed;
    }

    bool EnsureFileExistsWithBomUnlocked_() noexcept
    {
        const std::filesystem::path p = GetCurrentLogPath_();

        HANDLE h = ::CreateFileW(
            p.c_str(),
            GENERIC_READ | FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (h == INVALID_HANDLE_VALUE) {
            OutputPathError_(L"CreateFileW(EnsureFileExists)", p);
            return false;
        }

        LARGE_INTEGER size{};
        const BOOL gotSize = ::GetFileSizeEx(h, &size);

        if (gotSize && size.QuadPart == 0) {
            static constexpr unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            DWORD written = 0;
            const BOOL ok = ::WriteFile(h, bom, 3, &written, nullptr);
            if (!ok || written != 3) {
                ::CloseHandle(h);
                OutputPathError_(L"WriteFile(BOM)", p);
                return false;
            }
        }

        ::CloseHandle(h);
        return true;
    }

    bool AppendWideLineUtf8Unlocked_(std::wstring_view msg) noexcept
    {
        const std::filesystem::path p = GetCurrentLogPath_();

        HANDLE h = ::CreateFileW(
            p.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (h == INVALID_HANDLE_VALUE) {
            OutputPathError_(L"CreateFileW(Append)", p);
            return false;
        }

        LARGE_INTEGER size{};
        if (::GetFileSizeEx(h, &size) && size.QuadPart == 0) {
            static constexpr unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            DWORD writtenBom = 0;
            const BOOL okBom = ::WriteFile(h, bom, 3, &writtenBom, nullptr);
            if (!okBom || writtenBom != 3) {
                ::CloseHandle(h);
                OutputPathError_(L"WriteFile(BOM append path)", p);
                return false;
            }
        }

        std::wstring line(msg);
        if (line.empty() || (line.back() != L'\n' && line.back() != L'\r')) {
            line.push_back(L'\n');
        }

        std::string utf8;
        if (!WideToUtf8_(line, utf8)) {
            ::CloseHandle(h);
            ::OutputDebugStringW(L"[DebugMPMCLogBuffer] WideToUtf8 failed\n");
            return false;
        }

        DWORD written = 0;
        const BOOL ok = ::WriteFile(
            h,
            utf8.data(),
            static_cast<DWORD>(utf8.size()),
            &written,
            nullptr);

        ::CloseHandle(h);

        if (!ok || written != static_cast<DWORD>(utf8.size())) {
            OutputPathError_(L"WriteFile(Append body)", p);
            return false;
        }

        return true;
    }

    bool DirectAppendOne_(std::wstring_view msg) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(m_fileMutex);

            if (!EnsureOutputReady_()) {
                return false;
            }

            const bool ok = AppendWideLineUtf8Unlocked_(msg);
            if (ok) {
                RotateIfNeeded_();
            }
            return ok;
        }
        catch (...) {
            return false;
        }
    }

    void writer_loop() noexcept
    {
        while (m_running.load(std::memory_order_acquire)) {
            const bool wrote = drain_once();
            if (wrote) {
                continue;
            }

            std::unique_lock<std::mutex> lk(m_cv_m);
            m_cv.wait_for(
                lk,
                m_idleWait,
                [this]() noexcept {
                    return !m_running.load(std::memory_order_acquire)
                        || ApproxSize() > 0
                        || m_dropped.load(std::memory_order_relaxed) > 0;
                });
        }

        drain_once();
    }

    bool drain_once() noexcept
    {
        if (!HasFileOutput()) {
            return false;
        }

        try {
            if (ApproxSize() == 0 && m_dropped.load(std::memory_order_relaxed) == 0) {
                return false;
            }

            std::lock_guard<std::mutex> lock(m_fileMutex);

            if (!EnsureOutputReady_()) {
                return false;
            }

            bool any = false;
            std::wstring line;

            for (size_t i = 0; i < m_capacity; ++i) {
                if (!try_pop(line)) {
                    break;
                }

                if (AppendWideLineUtf8Unlocked_(line)) {
                    any = true;
                }
                else {
                    break;
                }
            }

            const std::uint64_t dropped = m_dropped.exchange(0, std::memory_order_relaxed);
            if (dropped > 0) {
                wchar_t buf[160]{};
                swprintf_s(
                    buf,
                    L"[DEBUG] dropped lines: %llu",
                    static_cast<unsigned long long>(dropped));
                (void)AppendWideLineUtf8Unlocked_(buf);
                any = true;
            }

            if (any) {
                RotateIfNeeded_();
            }

            return any;
        }
        catch (...) {
            return false;
        }
    }

private:
    std::unique_ptr<Slot[]>   m_storage;
    Slot* m_slots{ nullptr };
    size_t                    m_capacity{ 0 };
    size_t                    m_mask{ 0 };
    std::atomic<size_t>       m_enqueuePos{ 0 };
    std::atomic<size_t>       m_dequeuePos{ 0 };

    std::wstring              m_path;
    std::atomic<bool>         m_running{ false };
    std::thread               m_writer;
    std::condition_variable   m_cv;
    std::mutex                m_cv_m;
    std::chrono::milliseconds m_idleWait{ 10 };

    std::atomic<uint64_t>     m_dropped{ 0 };

    std::mutex                m_fileMutex;
    bool                      m_outputReady{ false };
    unsigned                  m_currentIndex{ 1 };

    static constexpr std::uintmax_t m_maxFileBytes =
        200ULL * 1024ULL * 1024ULL;

    DebugMPMCLogBuffer(const DebugMPMCLogBuffer&) = delete;
    DebugMPMCLogBuffer& operator=(const DebugMPMCLogBuffer&) = delete;
    DebugMPMCLogBuffer(DebugMPMCLogBuffer&&) = delete;
    DebugMPMCLogBuffer& operator=(DebugMPMCLogBuffer&&) = delete;
};

#endif // DEBUG_MPMC_LOG_BUFFER_HPP