#include "Memory_policy.hpp"

#if TSCEPG_MEMORY_POLICY_ACTIVE

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <cwchar>
#include <limits>

#pragma comment(lib, "Psapi.lib")

namespace TSCEPG::MemoryPolicy {
    namespace {

        constexpr std::uint32_t kDefaultIntervalMs = 5000;
        constexpr std::uint32_t kMinIntervalMs = 1000;
        constexpr std::size_t kPathChars = 1024;
        constexpr std::size_t kLineChars = 768;
        constexpr std::size_t kUtf8Chars = 2048;

        // SRWLOCK は明示的な破棄処理不要。低メモリ時も mutex の動的確保を避ける。
        SRWLOCK g_fileLock = SRWLOCK_INIT;
        wchar_t g_filePath[kPathChars]{};
        std::atomic_bool g_initialized{ false };
        std::atomic_uint32_t g_intervalMs{ kDefaultIntervalMs };
        std::atomic_uint64_t g_lastPulseMs{ 0 };

        [[nodiscard]] bool IsSimpleFileName_(const wchar_t* name) noexcept
        {
            if (!name || !*name) {
                return false;
            }
            for (const wchar_t* p = name; *p != L'\0'; ++p) {
                if (*p == L'\\' || *p == L'/' || *p == L':') {
                    return false;
                }
            }
            return true;
        }

        // fileName はファイル名のみ。exe ディレクトリを使用し、CWD に依存しない。
        [[nodiscard]] bool BuildPath_(const wchar_t* fileName) noexcept
        {
            wchar_t path[kPathChars]{};
            const DWORD capacity = static_cast<DWORD>(kPathChars);
            const DWORD length = ::GetModuleFileNameW(nullptr, path, capacity);
            if (length == 0 || length >= capacity - 1) {
                return false;
            }

            wchar_t* lastSlash = std::wcsrchr(path, L'\\');
            if (!lastSlash) {
                return false;
            }
            *++lastSlash = L'\0';

            const wchar_t* name = IsSimpleFileName_(fileName)
                ? fileName : L"memory_policy_x86.log";

            const std::size_t prefixLength = static_cast<std::size_t>(lastSlash - path);
            const std::size_t nameLength = std::wcslen(name);
            if (prefixLength + nameLength >= kPathChars) {
                return false;
            }

            std::wmemcpy(path + prefixLength, name, nameLength + 1);
            std::wmemcpy(g_filePath, path, prefixLength + nameLength + 1);
            return true;
        }

        // File handle を常時保持せず、記録のたびに開閉する。
        // 共有モードで開くため、ログをエディターで見ながら診断可能。
        void AppendLine_(const wchar_t* line) noexcept
        {
            if (!line || !g_initialized.load(std::memory_order_acquire)) {
                return;
            }

            char utf8[kUtf8Chars]{};
            const int capacity = static_cast<int>(kUtf8Chars);
            const int converted = ::WideCharToMultiByte(
                CP_UTF8, 0, line, -1, utf8, capacity, nullptr, nullptr);
            if (converted <= 1) {
                return;
            }

            ::AcquireSRWLockExclusive(&g_fileLock);
            const HANDLE file = ::CreateFileW(
                g_filePath,
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                const DWORD byteCount = static_cast<DWORD>(converted - 1);
                (void)::WriteFile(file, utf8, byteCount, &written, nullptr);
                ::CloseHandle(file);
            }
            ::ReleaseSRWLockExclusive(&g_fileLock);
        }

        void ScanAddressSpace_(Snapshot& snap) noexcept
        {
            SYSTEM_INFO info{};
            ::GetSystemInfo(&info);

            const std::uint64_t minimum = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress));
            const std::uint64_t maximum = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress));
            if (minimum > maximum) {
                snap.virtualQueryError = ERROR_INVALID_PARAMETER;
                return;
            }

            std::uint64_t address = minimum;
            const std::uint64_t limit = maximum + 1ULL;
            const std::uint64_t granularity =
                info.dwAllocationGranularity ? info.dwAllocationGranularity : 65536ULL;
            std::uint32_t errors = 0;
            std::uint32_t regionCount = 0;

            while (address < limit) {
                // 異常なアドレス区間の走査で無限ループしない。
                if (++regionCount > 250000U) {
                    snap.virtualQueryError = ERROR_GEN_FAILURE;
                    return;
                }

                MEMORY_BASIC_INFORMATION mbi{};
                const SIZE_T queried = ::VirtualQuery(
                    reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)),
                    &mbi,
                    sizeof(mbi));
                if (queried == 0) {
                    if (errors == 0) {
                        snap.virtualQueryError = ::GetLastError();
                    }
                    ++errors;
                    address = (address + granularity < limit)
                        ? address + granularity : limit;
                    continue;
                }

                const std::uint64_t regionBase = static_cast<std::uint64_t>(
                    reinterpret_cast<std::uintptr_t>(mbi.BaseAddress));
                const std::uint64_t regionLength = static_cast<std::uint64_t>(mbi.RegionSize);
                if (regionLength == 0 || regionBase > (std::numeric_limits<std::uint64_t>::max)() - regionLength) {
                    snap.virtualQueryError = ERROR_INVALID_DATA;
                    return;
                }

                const std::uint64_t regionEnd = regionBase + regionLength;
                const std::uint64_t end = (regionEnd < limit) ? regionEnd : limit;
                if (end <= address) {
                    snap.virtualQueryError = ERROR_INVALID_DATA;
                    return;
                }
                const std::uint64_t bytesInRange = end - address;

                switch (mbi.State) {
                case MEM_FREE:
                    snap.virtualFreeBytes += bytesInRange;
                    if (bytesInRange > snap.largestFreeBlockBytes) {
                        snap.largestFreeBlockBytes = bytesInRange;
                    }
                    break;
                case MEM_RESERVE:
                    snap.virtualReservedBytes += bytesInRange;
                    break;
                case MEM_COMMIT:
                    snap.virtualCommittedBytes += bytesInRange;
                    break;
                default:
                    break;
                }
                address = end;
            }

            // 仮想領域の一部で VirtualQuery が失敗した場合は、値を参考値としてのみ残す。
            snap.virtualValid = (errors == 0);
        }

        void EmitSnapshot_(const wchar_t* kind, const wchar_t* stage, bool scanVirtual) noexcept
        {
            if (!g_initialized.load(std::memory_order_acquire)) {
                return;
            }

            Snapshot s{};
            (void)ReadSnapshot(s, scanVirtual);
            constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
            wchar_t line[kLineChars]{};
            const wchar_t* safeKind = (kind && *kind) ? kind : L"CHECK";
            const wchar_t* safeStage = (stage && *stage) ? stage : L"(unspecified)";

            // Win32 での巨大な raw string を避け、固定サイズのフォーマットを使用。
            const int n = ::swprintf_s(
                line,
                kLineChars,
                L"[MEMORY_POLICY] tick=%llu kind=%.*ls stage=%.*ls "
                L"private=%lluMiB ws=%lluMiB pagefile=%lluMiB "
                L"freeVA=%lluMiB maxFreeVA=%lluMiB reservedVA=%lluMiB committedVA=%lluMiB "
                L"processOK=%u vaOK=%u processErr=%u vaErr=%u\r\n",
                static_cast<unsigned long long>(s.elapsedMs),
                24, safeKind,
                96, safeStage,
                static_cast<unsigned long long>(s.privateBytes / kMiB),
                static_cast<unsigned long long>(s.workingSetBytes / kMiB),
                static_cast<unsigned long long>(s.pagefileBytes / kMiB),
                static_cast<unsigned long long>(s.virtualFreeBytes / kMiB),
                static_cast<unsigned long long>(s.largestFreeBlockBytes / kMiB),
                static_cast<unsigned long long>(s.virtualReservedBytes / kMiB),
                static_cast<unsigned long long>(s.virtualCommittedBytes / kMiB),
                s.processValid ? 1U : 0U,
                s.virtualValid ? 1U : 0U,
                static_cast<unsigned>(s.processQueryError),
                static_cast<unsigned>(s.virtualQueryError));
            if (n > 0) {
                AppendLine_(line);
            }
        }

    } // namespace

    bool Initialize(const wchar_t* fileName) noexcept
    {
        ::AcquireSRWLockExclusive(&g_fileLock);
        const bool ok = BuildPath_(fileName);
        g_lastPulseMs.store(0, std::memory_order_release);
        g_initialized.store(ok, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&g_fileLock);

        if (ok) {
            EmitSnapshot_(L"START", L"APP_INITIALIZED", true);
        }
        return ok;
    }

    void Shutdown() noexcept
    {
        if (!g_initialized.load(std::memory_order_acquire)) {
            return;
        }
        EmitSnapshot_(L"STOP", L"APP_SHUTDOWN", false);
        g_initialized.store(false, std::memory_order_release);
    }

    void SetPulseIntervalMs(std::uint32_t intervalMs) noexcept
    {
        g_intervalMs.store(
            intervalMs < kMinIntervalMs ? kMinIntervalMs : intervalMs,
            std::memory_order_relaxed);
    }

    bool ReadSnapshot(Snapshot& out, bool scanVirtualAddressSpace) noexcept
    {
        out = Snapshot{};
        out.elapsedMs = static_cast<std::uint64_t>(::GetTickCount64());

        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (::GetProcessMemoryInfo(
            ::GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
            out.privateBytes = static_cast<std::uint64_t>(counters.PrivateUsage);
            out.workingSetBytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
            out.pagefileBytes = static_cast<std::uint64_t>(counters.PagefileUsage);
            out.processValid = true;
        }
        else {
            out.processQueryError = ::GetLastError();
        }

        if (scanVirtualAddressSpace) {
            ScanAddressSpace_(out);
        }
        return out.processValid;
    }

    void Checkpoint(const wchar_t* stage, bool scanVirtualAddressSpace) noexcept
    {
        EmitSnapshot_(L"CHECKPOINT", stage, scanVirtualAddressSpace);
    }

    void Pulse(const wchar_t* stage) noexcept
    {
        if (!g_initialized.load(std::memory_order_acquire)) {
            return;
        }

        const std::uint64_t now = static_cast<std::uint64_t>(::GetTickCount64());
        const std::uint64_t interval = g_intervalMs.load(std::memory_order_relaxed);
        std::uint64_t last = g_lastPulseMs.load(std::memory_order_relaxed);
        for (;;) {
            if (last != 0 && now >= last && now - last < interval) {
                return;
            }
            if (g_lastPulseMs.compare_exchange_weak(
                last, now, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                break;
            }
        }
        EmitSnapshot_(L"PULSE", stage, true);
    }

    void AllocationFailure(const wchar_t* stage) noexcept
    {
        EmitSnapshot_(L"BAD_ALLOC", stage, true);
    }

} // namespace TSCEPG::MemoryPolicy

#else // TSCEPG_MEMORY_POLICY_ACTIVE

// x64 では完全な no-op。Windows API や Psapi.lib への依存も追加しない。
namespace TSCEPG::MemoryPolicy {

    bool Initialize(const wchar_t*) noexcept { return false; }
    void Shutdown() noexcept {}
    void SetPulseIntervalMs(std::uint32_t) noexcept {}
    bool ReadSnapshot(Snapshot& out, bool) noexcept
    {
        out = Snapshot{};
        return false;
    }
    void Checkpoint(const wchar_t*, bool) noexcept {}
    void Pulse(const wchar_t*) noexcept {}
    void AllocationFailure(const wchar_t*) noexcept {}

} // namespace TSCEPG::MemoryPolicy

#endif // TSCEPG_MEMORY_POLICY_ACTIVE
