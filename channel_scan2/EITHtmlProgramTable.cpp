// EITHtmlProgramTable.cpp (REPLACE)  C++20
// - HTML EPG generator (grid + genre view)
// - Start~End time display (uses HTML entity for wave dash to avoid encoding issues)
// - Day label includes weekday (ja 1-char)
// - Active day can be moved (prev/next buttons + ArrowLeft/ArrowRight)
// - Output is split into 2 files:
//     1) htmlOutputPath           : "today and after" (broadcast-day 기준)
//     2) htmlOutputPath*_before.* : "before" (past)
// - Optional: skip regenerate if output newer than inputs (env)
//
// NOTE: 既存の public 関数シグネチャは維持（GenerateEITHtmlProgramTable / Run... / Dump...）
//
// env:
//   EIT_HTML_SKIP_IF_UPTODATE=0/1        (default 0)
//   EIT_HTML_WRITE_BOM=0/1              (default 1)
//   EIT_DAY_BOUNDARY_MINUTES=0..1439    (preferred)   default 240(04:00)
//   EIT_DAY_BOUNDARY_HOUR=0..23         (fallback)    default 4
//   EIT_GRID_SLOT_MINUTES=1..60         (default 5)
//   EIT_GRID_LABEL_INTERVAL_MINUTES=5..240 (default 30)

#include "EITHtmlProgramTable.hpp"
#include "DebugManager.h"

#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <concepts>
#include <type_traits>
#include <tuple>
#include <climits>
#include <functional>
#include <array>
#include <cerrno>
#include <ctime>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

// ---------------- デバッグログマクロ ----------------
#define EIT_HTML_LOG(...) DebugLog(__VA_ARGS__)

// ---------------- 内部ヘルパ ----------------
namespace {

    // ---- env helpers (wstring) ----
    bool GetEnvW(const wchar_t* key, std::wstring& out) noexcept
    {
        out.clear();
        if (!key || !*key) return false;

        DWORD need = GetEnvironmentVariableW(key, nullptr, 0);
        if (need == 0) return false;

        std::wstring buf;
        buf.resize(static_cast<std::size_t>(need)); // need includes '\0'

        DWORD written = GetEnvironmentVariableW(key, buf.data(), need);
        if (written == 0 || written >= need) return false;

        buf.resize(static_cast<std::size_t>(written));
        out = std::move(buf);
        return true;
    }

    bool GetEnvIntClamped(const wchar_t* key, int& out, int lo, int hi) noexcept
    {
        std::wstring s;
        if (!GetEnvW(key, s)) return false;
        try {
            int v = std::stoi(s);
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            out = v;
            return true;
        }
        catch (...) {
            return false;
        }
    }

    // 汎用トリム（BOM(U+FEFF)も除去）
    std::wstring Trim(const std::wstring& s)
    {
        const auto is_space = [](wchar_t ch) {
            return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == 0xFEFF;
            };

        std::size_t start = 0;
        while (start < s.size() && is_space(s[start])) ++start;
        if (start == s.size()) return L"";

        std::size_t end = s.size();
        while (end > start && is_space(s[end - 1])) --end;

        return s.substr(start, end - start);
    }

    // UTF-8 テキストファイル → wstring（先頭BOM除去つき）
    bool LoadUtf8FileToWString(const std::wstring& filePath, std::wstring& outText) noexcept
    {
        outText.clear();

        std::ifstream ifs(std::filesystem::path(filePath), std::ios::binary);
        if (!ifs) {
            DebugLog(L"[EIT-HTML] ファイルオープン失敗: %ls", filePath.c_str());
            return false;
        }

        ifs.seekg(0, std::ios::end);
        const std::streamoff lenOff = ifs.tellg();
        if (lenOff < 0) {
            DebugLog(L"[EIT-HTML] tellg 失敗: %ls", filePath.c_str());
            return false;
        }

        const std::uint64_t len64 = static_cast<std::uint64_t>(lenOff);
        if (len64 == 0) return true;
        if (len64 > static_cast<std::uint64_t>(INT_MAX)) {
            DebugLog(L"[EIT-HTML] ファイルが大きすぎます(INT_MAX超): %ls", filePath.c_str());
            return false;
        }

        std::string bytes;
        bytes.resize(static_cast<std::size_t>(len64));

        ifs.seekg(0, std::ios::beg);
        ifs.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!ifs && static_cast<std::size_t>(ifs.gcount()) != bytes.size()) {
            DebugLog(L"[EIT-HTML] read 失敗: %ls", filePath.c_str());
            return false;
        }

        if (bytes.empty()) return true;

        int wideLen = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            bytes.data(),
            static_cast<int>(bytes.size()),
            nullptr,
            0);
        if (wideLen <= 0) {
            DebugLog(L"[EIT-HTML] UTF-8 -> UTF-16 変換サイズ取得失敗(不正UTF-8?): %ls", filePath.c_str());
            return false;
        }

        outText.resize(static_cast<std::size_t>(wideLen));
        int written = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            bytes.data(),
            static_cast<int>(bytes.size()),
            outText.data(),
            wideLen);
        if (written <= 0) {
            DebugLog(L"[EIT-HTML] UTF-8 -> UTF-16 変換失敗: %ls", filePath.c_str());
            outText.clear();
            return false;
        }

        if (!outText.empty() && outText.front() == 0xFEFF) {
            outText.erase(outText.begin());
        }

        return true;
    }

    // wchar_t → UTF-8 変換
    bool WStringToUtf8(const std::wstring& src, std::string& dst) noexcept
    {
        dst.clear();
        if (src.empty()) return true;

        if (src.size() > static_cast<std::size_t>(INT_MAX)) return false;

        int len = WideCharToMultiByte(
            CP_UTF8,
            0,
            src.data(),
            static_cast<int>(src.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (len <= 0) return false;

        dst.resize(static_cast<std::size_t>(len));
        int written = WideCharToMultiByte(
            CP_UTF8,
            0,
            src.data(),
            static_cast<int>(src.size()),
            dst.data(),
            len,
            nullptr,
            nullptr);
        if (written <= 0) {
            dst.clear();
            return false;
        }

        return true;
    }

    // テキスト用の軽いサニタイズ（制御文字・改行をスペース化）
    std::wstring SanitizeForHtmlText(const std::wstring& wsrc)
    {
        std::wstring tmp;
        tmp.reserve(wsrc.size());
        for (wchar_t ch : wsrc) {
            if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
                tmp.push_back(L' ');
                continue;
            }
            if (ch < 0x20) {
                tmp.push_back(L'?');
                continue;
            }
            tmp.push_back(ch);
        }
        return tmp;
    }

    // UTF-8 文字列中の & < > " ' を HTML エスケープ（テキスト用）
    std::string HtmlEscapeFromUtf8(const std::wstring& wsrc) noexcept
    {
        std::string utf8;
        if (!WStringToUtf8(SanitizeForHtmlText(wsrc), utf8)) return {};

        std::string out;
        out.reserve(utf8.size() + utf8.size() / 8);

        for (unsigned char ch : utf8) {
            switch (ch) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:
                out.push_back(static_cast<char>(ch));
                break;
            }
        }
        return out;
    }

    std::wstring TruncateW(const std::wstring& s, std::size_t maxLen)
    {
        if (s.size() <= maxLen) return s;
        if (maxLen <= 1) return std::wstring(maxLen, L'…');
        return s.substr(0, maxLen - 1) + L"…";
    }

    // HTML属性用（改行/制御文字をスペース化 + エスケープ）
    std::string HtmlEscapeAttrFromUtf8(const std::wstring& wsrc) noexcept
    {
        return HtmlEscapeFromUtf8(SanitizeForHtmlText(wsrc));
    }

    // "～" はソース文字コード依存で崩れることがあるので、HTMLエンティティで出す
    constexpr const char* kWaveDashHtmlEntity = "&#12316;"; // U+301C (WAVE DASH)

    // UTF-8 を BOM 付きでファイルに保存（envで切替）
    bool WriteUtf8File(const std::wstring& path, const std::string& utf8Body) noexcept
    {
        {
            std::error_code ec;
            const auto p = std::filesystem::path(path);
            const auto dir = p.parent_path();
            if (!dir.empty()) {
                std::filesystem::create_directories(dir, ec);
                if (ec) {
                    DebugLog(L"[EIT-HTML] 出力ディレクトリ作成に失敗(続行): %ls (ec=%d)",
                        dir.wstring().c_str(), static_cast<int>(ec.value()));
                }
            }
        }

        std::ofstream ofs(std::filesystem::path(path), std::ios::binary);
        if (!ofs) {
            DebugLog(L"[EIT-HTML] HTML 出力ファイルオープン失敗: %ls", path.c_str());
            return false;
        }

        int writeBom = 1;
        (void)GetEnvIntClamped(L"EIT_HTML_WRITE_BOM", writeBom, 0, 1);

        if (writeBom) {
            const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            ofs.write(reinterpret_cast<const char*>(bom), 3);
        }

        if (!utf8Body.empty()) {
            ofs.write(utf8Body.data(), static_cast<std::streamsize>(utf8Body.size()));
        }

        return ofs.good();
    }

    // ---------------- ファイル更新チェック（任意） ----------------
    bool TryGetLastWriteTime(const std::wstring& path, std::filesystem::file_time_type& out) noexcept
    {
        std::error_code ec;
        out = std::filesystem::last_write_time(std::filesystem::path(path), ec);
        return !ec;
    }

    // file_time_type -> "YYYY-MM-DD HH:MM:SS"
    std::string FormatFileTimeLocal(const std::filesystem::file_time_type& ft) noexcept
    {
        using namespace std::chrono;

        const auto sctp =
            time_point_cast<system_clock::duration>(
                ft - std::filesystem::file_time_type::clock::now() + system_clock::now());

        const std::time_t tt = system_clock::to_time_t(sctp);

        std::tm tm{};
        localtime_s(&tm, &tt);

        std::ostringstream oss;
        oss << std::setfill('0')
            << std::setw(4) << (tm.tm_year + 1900) << '-'
            << std::setw(2) << (tm.tm_mon + 1) << '-'
            << std::setw(2) << tm.tm_mday << ' '
            << std::setw(2) << tm.tm_hour << ':'
            << std::setw(2) << tm.tm_min << ':'
            << std::setw(2) << tm.tm_sec;
        return oss.str();
    }

    bool ShouldSkipRegenerateIfUpToDate(
        const std::wstring& epgDumpPath,
        const std::wstring& channelListPath,
        const std::wstring& htmlOutputPath) noexcept
    {
        int skip = 0;
        if (!GetEnvIntClamped(L"EIT_HTML_SKIP_IF_UPTODATE", skip, 0, 1) || skip == 0) {
            return false;
        }

        std::filesystem::file_time_type outT{};
        if (!TryGetLastWriteTime(htmlOutputPath, outT)) {
            return false; // 出力が無い/取得できない → 作る
        }

        std::filesystem::file_time_type epgT{};
        if (TryGetLastWriteTime(epgDumpPath, epgT)) {
            if (outT < epgT) return false;
        }

        if (!channelListPath.empty()) {
            std::filesystem::file_time_type chT{};
            if (TryGetLastWriteTime(channelListPath, chT)) {
                if (outT < chT) return false;
            }
        }

        DebugLog(L"[EIT-HTML] up-to-date 判定: 出力が入力より新しいため生成をスキップします (env EIT_HTML_SKIP_IF_UPTODATE=1)");
        return true;
    }

    // ---------------- MJD + BCD40bit → 日時 ----------------
    struct ProgramDateTime
    {
        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int minute = 0;
        int second = 0;
    };

    using sys_minutes = std::chrono::sys_time<std::chrono::minutes>;

    void MJDToYMD(int mjd, int& year, int& month, int& day) noexcept
    {
        int64_t jd = static_cast<int64_t>(mjd) + 2400001LL;

        int64_t l = jd + 68569;
        int64_t n = (4 * l) / 146097;
        l = l - (146097 * n + 3) / 4;
        int64_t i = (4000 * (l + 1)) / 1461001;
        l = l - (1461 * i) / 4 + 31;
        int64_t j = (80 * l) / 2447;
        int64_t d = l - (2447 * j) / 80;
        l = j / 11;
        int64_t m = j + 2 - (12 * l);
        int64_t y = 100 * (n - 49) + i + l;

        year = static_cast<int>(y);
        month = static_cast<int>(m);
        day = static_cast<int>(d);
    }

    int BcdToInt(uint8_t b) noexcept
    {
        int hi = (b >> 4) & 0x0F;
        int lo = b & 0x0F;
        if (hi > 9 || lo > 9) return -1;
        return hi * 10 + lo;
    }

    bool TryParseHexULL(std::wstring_view hexDigits, unsigned long long& out) noexcept
    {
        out = 0;
        if (hexDigits.empty()) return false;

        for (wchar_t c : hexDigits) {
            unsigned v = 0;
            if (c >= L'0' && c <= L'9') v = static_cast<unsigned>(c - L'0');
            else if (c >= L'a' && c <= L'f') v = 10u + static_cast<unsigned>(c - L'a');
            else if (c >= L'A' && c <= L'F') v = 10u + static_cast<unsigned>(c - L'A');
            else return false;

            out = (out << 4) | static_cast<unsigned long long>(v);
        }
        return true;
    }

    bool DecodeStartTimeRawHex(const std::wstring& rawHex, ProgramDateTime& dt) noexcept
    {
        dt = ProgramDateTime{};
        if (rawHex.empty()) return false;

        std::wstring token;
        {
            std::size_t pos = 0;
            while (pos < rawHex.size() && ::iswspace(rawHex[pos])) ++pos;
            while (pos < rawHex.size() && !::iswspace(rawHex[pos])) {
                token.push_back(rawHex[pos]);
                ++pos;
            }
        }
        if (token.empty()) return false;

        std::size_t idx = 0;
        if (token.size() >= 2 && token[0] == L'0' && (token[1] == L'x' || token[1] == L'X')) {
            idx = 2;
        }

        std::wstring hexDigits;
        for (; idx < token.size(); ++idx) {
            wchar_t ch = token[idx];
            if ((ch >= L'0' && ch <= L'9') ||
                (ch >= L'a' && ch <= L'f') ||
                (ch >= L'A' && ch <= L'F'))
            {
                hexDigits.push_back(ch);
            }
            else {
                break;
            }
        }

        if (hexDigits.empty()) return false;

        if (hexDigits.size() > 10) {
            hexDigits = hexDigits.substr(hexDigits.size() - 10);
        }

        unsigned long long value = 0;
        if (!TryParseHexULL(hexDigits, value)) return false;

        if (value == 0xFFFFFFFFFFull) return false;

        uint16_t mjd = static_cast<uint16_t>(value >> 24);
        if (mjd == 0xFFFFu) return false;

        uint8_t bcdH = static_cast<uint8_t>((value >> 16) & 0xFF);
        uint8_t bcdM = static_cast<uint8_t>((value >> 8) & 0xFF);
        uint8_t bcdS = static_cast<uint8_t>(value & 0xFF);

        int hour = BcdToInt(bcdH);
        int min = BcdToInt(bcdM);
        int sec = BcdToInt(bcdS);
        if (hour < 0 || min < 0 || sec < 0) return false;

        int y = 0, m = 0, d = 0;
        MJDToYMD(static_cast<int>(mjd), y, m, d);

        dt.year = y;
        dt.month = m;
        dt.day = d;
        dt.hour = hour;
        dt.minute = min;
        dt.second = sec;
        return true;
    }

    bool ParseDurationToMinutes(const std::wstring& s, int& outMinutes) noexcept
    {
        outMinutes = 0;
        if (s.size() < 8) return false;

        int h = 0, m = 0, sec = 0;
        wchar_t c1 = 0, c2 = 0;

        std::wstringstream ss(s);
        ss >> h >> c1 >> m >> c2 >> sec;
        if (!ss || c1 != L':' || c2 != L':') return false;
        if (h < 0 || m < 0) return false;

        outMinutes = h * 60 + m;
        return true;
    }

    bool operator<(const ProgramDateTime& a, const ProgramDateTime& b) noexcept
    {
        if (a.year != b.year) return a.year < b.year;
        if (a.month != b.month) return a.month < b.month;
        if (a.day != b.day) return a.day < b.day;
        if (a.hour != b.hour) return a.hour < b.hour;
        if (a.minute != b.minute) return a.minute < b.minute;
        return a.second < b.second;
    }

    ProgramDateTime AddMinutes(const ProgramDateTime& start, int addMinutes) noexcept
    {
        using namespace std::chrono;

        if (start.year <= 0) return start;

        if (start.month < 1 || start.month > 12 || start.day < 1 || start.day > 31 ||
            start.hour < 0 || start.hour > 23 || start.minute < 0 || start.minute > 59 ||
            start.second < 0 || start.second > 59)
        {
            return start;
        }

        const year y{ start.year };
        const month m{ static_cast<unsigned>(start.month) };
        const day d{ static_cast<unsigned>(start.day) };

        const year_month_day ymd{ y / m / d };
        if (!ymd.ok()) return start;

        sys_seconds tp = sys_seconds{ sys_days{ ymd } }
            + hours{ start.hour }
            + minutes{ start.minute }
        + seconds{ start.second };

        tp += minutes{ addMinutes };

        const auto dp = floor<days>(tp);
        const year_month_day ymd2{ dp };
        const hh_mm_ss hms{ tp - dp };

        ProgramDateTime out{};
        out.year = int(ymd2.year());
        out.month = int(unsigned(ymd2.month()));
        out.day = int(unsigned(ymd2.day()));
        out.hour = int(hms.hours().count());
        out.minute = int(hms.minutes().count());
        out.second = int(hms.seconds().count());
        return out;
    }

    sys_minutes ToSysMinutes(const ProgramDateTime& dt) noexcept
    {
        using namespace std::chrono;

        if (dt.year <= 0) return sys_minutes{};

        if (dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 ||
            dt.hour < 0 || dt.hour > 23 || dt.minute < 0 || dt.minute > 59 ||
            dt.second < 0 || dt.second > 59)
        {
            return sys_minutes{};
        }

        const year y{ dt.year };
        const month m{ static_cast<unsigned>(dt.month) };
        const day d{ static_cast<unsigned>(dt.day) };

        const year_month_day ymd{ y / m / d };
        if (!ymd.ok()) return sys_minutes{};

        const sys_seconds tp = sys_seconds{ sys_days{ ymd } }
            + hours{ dt.hour }
            + minutes{ dt.minute }
        + seconds{ dt.second };

        return floor<minutes>(tp);
    }

    ProgramDateTime FromSysMinutes(const sys_minutes tp) noexcept
    {
        using namespace std::chrono;

        ProgramDateTime out{};
        const auto dp = floor<days>(tp);
        const year_month_day ymd{ dp };
        const hh_mm_ss hms{ tp - dp };

        out.year = int(ymd.year());
        out.month = int(unsigned(ymd.month()));
        out.day = int(unsigned(ymd.day()));
        out.hour = int(hms.hours().count());
        out.minute = int(hms.minutes().count());
        out.second = int(hms.seconds().count());
        return out;
    }

    sys_minutes AlignDownToSlot(const sys_minutes tp, int slotMinutes) noexcept
    {
        using namespace std::chrono;
        if (slotMinutes <= 0) return tp;
        const auto c = tp.time_since_epoch().count();
        const auto s = static_cast<long long>(slotMinutes);
        const auto aligned = (c / s) * s;
        return sys_minutes{ minutes{ aligned } };
    }

    sys_minutes AlignUpToSlot(const sys_minutes tp, int slotMinutes) noexcept
    {
        using namespace std::chrono;
        if (slotMinutes <= 0) return tp;
        const auto c = tp.time_since_epoch().count();
        const auto s = static_cast<long long>(slotMinutes);
        const auto aligned = ((c + (s - 1)) / s) * s;
        return sys_minutes{ minutes{ aligned } };
    }

    // ---------------- 放送日（日付切替） ----------------
    constexpr int kDefaultBroadcastDayBoundaryMinutes = 4 * 60;

    int GetBroadcastDayBoundaryMinutesFromEnv() noexcept
    {
        int v = 0;
        if (GetEnvIntClamped(L"EIT_DAY_BOUNDARY_MINUTES", v, 0, 1439)) return v;

        int h = 0;
        if (GetEnvIntClamped(L"EIT_DAY_BOUNDARY_HOUR", h, 0, 23)) return h * 60;

        return kDefaultBroadcastDayBoundaryMinutes;
    }

    int GetGridSlotMinutesFromEnv(int defVal) noexcept
    {
        int v = defVal;
        if (GetEnvIntClamped(L"EIT_GRID_SLOT_MINUTES", v, 1, 60)) return v;
        return defVal;
    }

    int GetGridLabelIntervalMinutesFromEnv(int defVal) noexcept
    {
        int v = defVal;
        if (GetEnvIntClamped(L"EIT_GRID_LABEL_INTERVAL_MINUTES", v, 5, 240)) return v;
        return defVal;
    }

    std::chrono::sys_days BroadcastDayKey(sys_minutes tp, int boundaryMinutes) noexcept
    {
        using namespace std::chrono;
        return floor<days>(tp - minutes{ boundaryMinutes });
    }

    // ---------------- 曜日（日本語 1 文字） ----------------
    std::wstring WeekdayShortJa(std::chrono::sys_days d) noexcept
    {
        using namespace std::chrono;
        const unsigned wd = weekday{ d }.c_encoding(); // 0=Sun..6=Sat
        static constexpr wchar_t kW[] = L"日月火水木金土";
        if (wd < 7) return std::wstring(1, kW[wd]);
        return L"?";
    }

    ProgramDateTime NowLocalProgramDateTime() noexcept
    {
        std::time_t tt = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &tt);

        ProgramDateTime dt{};
        dt.year = tm.tm_year + 1900;
        dt.month = tm.tm_mon + 1;
        dt.day = tm.tm_mday;
        dt.hour = tm.tm_hour;
        dt.minute = tm.tm_min;
        dt.second = tm.tm_sec;
        return dt;
    }

    // ---------------- ジャンル抽出（できる範囲で） ----------------
    namespace genre {

        inline std::wstring MajorLabel(uint8_t major)
        {
            switch (major & 0x0F) {
            case 0x0: return L"ニュース／報道";
            case 0x1: return L"スポーツ";
            case 0x2: return L"情報／ワイドショー";
            case 0x3: return L"ドラマ";
            case 0x4: return L"音楽";
            case 0x5: return L"バラエティ";
            case 0x6: return L"映画";
            case 0x7: return L"アニメ／特撮";
            case 0x8: return L"ドキュメンタリー／教養";
            case 0x9: return L"劇場／公演";
            case 0xA: return L"趣味／教育";
            case 0xB: return L"福祉";
            case 0xC: return L"予備";
            case 0xD: return L"予備";
            case 0xE: return L"拡張";
            case 0xF: return L"その他";
            default:  return L"その他";
            }
        }

        template <typename T>
        concept WsvLike = requires(const T & v) { std::wstring_view{ v }; };

        template <typename T>
        concept Iterable = requires(const T & v) { v.begin(); v.end(); };

        struct Result {
            bool has = false;
            uint8_t major = 0xFF;
            uint8_t minor = 0xFF;
            std::wstring label; // 可能なら元の表記
        };

        template <class Elem>
        bool TryDecodeNibblePair(const Elem& e, uint8_t& outMajor, uint8_t& outMinor) noexcept
        {
            if constexpr (requires { e.level1; e.level2; }) {
                outMajor = static_cast<uint8_t>(e.level1);
                outMinor = static_cast<uint8_t>(e.level2);
                return true;
            }
            else if constexpr (requires { e.nibble1; e.nibble2; }) {
                outMajor = static_cast<uint8_t>(e.nibble1);
                outMinor = static_cast<uint8_t>(e.nibble2);
                return true;
            }
            else if constexpr (requires { e.major; e.minor; }) {
                outMajor = static_cast<uint8_t>(e.major);
                outMinor = static_cast<uint8_t>(e.minor);
                return true;
            }
            else if constexpr (requires { std::get<0>(e); std::get<1>(e); }) {
                outMajor = static_cast<uint8_t>(std::get<0>(e));
                outMinor = static_cast<uint8_t>(std::get<1>(e));
                return true;
            }
            return false;
        }

        inline bool GuessMajorFromText(const std::wstring& text, uint8_t& outMajor) noexcept
        {
            if (text.empty()) return false;

            for (uint8_t m = 0; m < 16; ++m) {
                const std::wstring lab = MajorLabel(m);
                if (!lab.empty() && text.find(lab) != std::wstring::npos) {
                    outMajor = m;
                    return true;
                }
            }
            return false;
        }

        template <class Ev>
        std::wstring ExtractGenreTextLoose(const Ev& ev) noexcept
        {
            if constexpr (requires(const Ev & x) { x.genreText; }) {
                if constexpr (WsvLike<decltype(ev.genreText)>) {
                    std::wstring_view sv{ ev.genreText };
                    if (!sv.empty()) return std::wstring(sv);
                }
            }
            if constexpr (requires(const Ev & x) { x.genre; }) {
                if constexpr (WsvLike<decltype(ev.genre)>) {
                    std::wstring_view sv{ ev.genre };
                    if (!sv.empty()) return std::wstring(sv);
                }
            }
            if constexpr (requires(const Ev & x) { x.contentGenre; }) {
                if constexpr (WsvLike<decltype(ev.contentGenre)>) {
                    std::wstring_view sv{ ev.contentGenre };
                    if (!sv.empty()) return std::wstring(sv);
                }
            }
            if constexpr (requires(const Ev & x) { x.genresText; }) {
                if constexpr (WsvLike<decltype(ev.genresText)>) {
                    std::wstring_view sv{ ev.genresText };
                    if (!sv.empty()) return std::wstring(sv);
                }
            }
            if constexpr (requires(const Ev & x) { x.contentText; }) {
                if constexpr (WsvLike<decltype(ev.contentText)>) {
                    std::wstring_view sv{ ev.contentText };
                    if (!sv.empty()) return std::wstring(sv);
                }
            }

            if constexpr (requires(const Ev & x) { x.genres; }) {
                const auto& g = ev.genres;
                if constexpr (Iterable<decltype(g)>) {
                    std::wstring joined;
                    bool first = true;

                    for (const auto& elem : g) {
                        if constexpr (WsvLike<decltype(elem)>) {
                            std::wstring_view sv{ elem };
                            if (sv.empty()) continue;
                            if (!first) joined += L", ";
                            joined += sv;
                            first = false;
                            continue;
                        }

                        uint8_t maj = 0xFF, min = 0xFF;
                        if (TryDecodeNibblePair(elem, maj, min)) {
                            const std::wstring lab = MajorLabel(maj);
                            if (!lab.empty()) {
                                if (!first) joined += L", ";
                                joined += lab;
                                first = false;
                            }
                        }
                    }

                    if (!joined.empty()) return joined;
                }
            }

            if constexpr (requires(const Ev & x) { x.contentNibbles; }) {
                const auto& g = ev.contentNibbles;
                if constexpr (Iterable<decltype(g)>) {
                    std::wstring joined;
                    bool first = true;
                    for (const auto& elem : g) {
                        uint8_t maj = 0xFF, min = 0xFF;
                        if (TryDecodeNibblePair(elem, maj, min)) {
                            const std::wstring lab = MajorLabel(maj);
                            if (!lab.empty()) {
                                if (!first) joined += L", ";
                                joined += lab;
                                first = false;
                            }
                        }
                    }
                    if (!joined.empty()) return joined;
                }
            }

            return L"";
        }

        template <class Ev>
        Result Extract(const Ev& ev) noexcept
        {
            Result out{};

            if constexpr (requires(const Ev & x) { x.genreMajor; }) {
                uint8_t maj = static_cast<uint8_t>(ev.genreMajor);
                out.has = true;
                out.major = maj;
                out.label = MajorLabel(maj);
                return out;
            }
            if constexpr (requires(const Ev & x) { x.contentNibbleLevel1; }) {
                uint8_t maj = static_cast<uint8_t>(ev.contentNibbleLevel1);
                out.has = true;
                out.major = maj;
                out.label = MajorLabel(maj);
                return out;
            }

            std::wstring text = ExtractGenreTextLoose(ev);
            text = Trim(text);
            if (!text.empty()) {
                out.has = true;
                out.label = text;

                uint8_t maj = 0xFF;
                if (GuessMajorFromText(text, maj)) out.major = maj;
                return out;
            }

            return out;
        }

    } // namespace genre

    // ---------------- channellist_space0.ch 読み込み ----------------
    struct ServiceEntry
    {
        std::wstring serviceName;
        int          space = 0;
        int          channel = 0;
        uint16_t     serviceType = 0;
        uint16_t     serviceId = 0;
        uint16_t     networkId = 0;
        uint16_t     tsid = 0;
        uint16_t     onid = 0;
        bool         freeCA = false;
        std::wstring provider;
        std::wstring networkName;
        int          remoteControlKeyId = 0;
    };

    bool ParseInt(const std::wstring& s, int& out) noexcept
    {
        out = 0;
        if (s.empty()) return false;

        wchar_t* endp = nullptr;
        errno = 0;
        long v = std::wcstol(s.c_str(), &endp, 10);
        if (endp == s.c_str()) return false;
        if (errno != 0) return false;

        if (v < INT_MIN) v = INT_MIN;
        if (v > INT_MAX) v = INT_MAX;
        out = static_cast<int>(v);
        return true;
    }

    bool ParseUInt16(const std::wstring& s, uint16_t& out) noexcept
    {
        out = 0;
        if (s.empty()) return false;

        wchar_t* endp = nullptr;
        errno = 0;
        unsigned long v = std::wcstoul(s.c_str(), &endp, 10);
        if (endp == s.c_str()) return false;
        if (errno != 0) return false;

        if (v > 0xFFFFul) v = 0xFFFFul;
        out = static_cast<uint16_t>(v);
        return true;
    }

    std::vector<std::wstring> ParseCsvLine(const std::wstring& line)
    {
        std::vector<std::wstring> cols;
        std::wstring cur;
        cur.reserve(line.size());

        bool inQuote = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            wchar_t ch = line[i];
            if (inQuote) {
                if (ch == L'"') {
                    if (i + 1 < line.size() && line[i + 1] == L'"') {
                        cur.push_back(L'"');
                        ++i;
                    }
                    else {
                        inQuote = false;
                    }
                }
                else {
                    cur.push_back(ch);
                }
            }
            else {
                if (ch == L',') {
                    cols.push_back(Trim(cur));
                    cur.clear();
                }
                else if (ch == L'"') {
                    if (cur.empty()) inQuote = true;
                    else cur.push_back(ch);
                }
                else {
                    cur.push_back(ch);
                }
            }
        }
        cols.push_back(Trim(cur));
        return cols;
    }

    bool LoadChannelList(const std::wstring& filePath, std::vector<ServiceEntry>& outList) noexcept
    {
        std::wstring text;
        if (!LoadUtf8FileToWString(filePath, text)) return false;

        std::wistringstream iss(text);
        std::wstring line;
        outList.clear();

        while (std::getline(iss, line)) {
            std::wstring t = Trim(line);
            if (t.empty()) continue;
            if (!t.empty() && t[0] == L';') continue;

            auto cols = ParseCsvLine(t);
            if (cols.size() < 12) {
                DebugLog(L"[EIT-HTML] channellist 行をスキップ: 列数不足 (%zu): %ls",
                    cols.size(), t.c_str());
                continue;
            }

            ServiceEntry se;
            se.serviceName = cols[0];
            ParseInt(cols[1], se.space);
            ParseInt(cols[2], se.channel);
            ParseUInt16(cols[3], se.serviceType);
            ParseUInt16(cols[4], se.serviceId);
            ParseUInt16(cols[5], se.networkId);
            ParseUInt16(cols[6], se.tsid);
            ParseUInt16(cols[7], se.onid);
            {
                int freeCaInt = 0;
                ParseInt(cols[8], freeCaInt);
                se.freeCA = (freeCaInt != 0);
            }
            se.provider = cols[9];
            se.networkName = cols[10];
            ParseInt(cols[11], se.remoteControlKeyId);

            outList.push_back(std::move(se));
        }

        if (outList.empty()) {
            DebugLog(L"[EIT-HTML] channellist が空です: %ls", filePath.c_str());
            return false;
        }

        EIT_HTML_LOG(L"[EIT-HTML] channellist 読み込み: %zu services", outList.size());
        return true;
    }

    uint64_t MakeServiceKey(uint16_t sid, uint16_t tsid, uint16_t onid) noexcept
    {
        return (static_cast<uint64_t>(sid) << 32) |
            (static_cast<uint64_t>(tsid) << 16) |
            static_cast<uint64_t>(onid);
    }

    std::wstring MakeBeforePathW(const std::wstring& htmlOutputPath)
    {
        std::filesystem::path p(htmlOutputPath);
        std::wstring stem = p.stem().wstring();
        std::wstring ext = p.extension().wstring();
        return (p.parent_path() / (stem + L"_before" + ext)).wstring();
    }

    std::wstring MakeLabelWithWeekday(const std::string& dayStr, std::chrono::sys_days dayCal)
    {
        // dayStr is "YYYY/MM/DD"
        std::wstring base(dayStr.begin(), dayStr.end());
        base += L"(" + WeekdayShortJa(dayCal) + L")";
        return base;
    }

    // Split helper for "others" list
    template <class T>
    void SplitByBroadcastDayKey(
        const std::vector<T>& items,
        std::vector<T>& outPast,
        std::vector<T>& outUpcoming,
        const std::chrono::sys_days& todayKey,
        int boundaryMinutes,
        const std::function<bool(const T&, sys_minutes&)>& getStartTp)
    {
        outPast.clear();
        outUpcoming.clear();
        outPast.reserve(items.size() / 2);
        outUpcoming.reserve(items.size() / 2);

        for (const auto& it : items) {
            sys_minutes tp{};
            if (!getStartTp(it, tp)) {
                // 時刻不明は upcoming 側に寄せる（見落とし防止）
                outUpcoming.push_back(it);
                continue;
            }
            const auto k = BroadcastDayKey(tp, boundaryMinutes);
            if (k < todayKey) outPast.push_back(it);
            else outUpcoming.push_back(it);
        }
    }

} // namespace

// ---------------- メイン実装 ----------------

// 旧版互換
bool GenerateEITHtmlProgramTable(const std::wstring& epgDumpPath, const std::wstring& htmlOutputPath)
{
    return GenerateEITHtmlProgramTable(epgDumpPath, L"", htmlOutputPath);
}

// ★ 新実装（放送日(04:00切替) + 表示形式(時間表/ジャンル別) + ジャンルフィルタ + 縦横スクロール）
// ★ さらに: before(過去) / 当日以降 の 2HTML に分割して出力
bool GenerateEITHtmlProgramTable(
    const std::wstring& epgDumpPath,
    const std::wstring& channelListPath,
    const std::wstring& htmlOutputPath)
{
    if (ShouldSkipRegenerateIfUpToDate(epgDumpPath, channelListPath, htmlOutputPath)) {
        return true;
    }

    const std::wstring beforePath = MakeBeforePathW(htmlOutputPath);

    EIT_HTML_LOG(L"[EIT-HTML] HTML 番組表生成開始: EPG=%ls, CHLIST=%ls -> %ls (+before=%ls)",
        epgDumpPath.c_str(),
        channelListPath.c_str(),
        htmlOutputPath.c_str(),
        beforePath.c_str());

    const int broadcastBoundaryMinutes = GetBroadcastDayBoundaryMinutesFromEnv();
    EIT_HTML_LOG(L"[EIT-HTML] 放送日切替境界: %d minutes (%02d:%02d)  ※env EIT_DAY_BOUNDARY_HOUR / EIT_DAY_BOUNDARY_MINUTES",
        broadcastBoundaryMinutes,
        broadcastBoundaryMinutes / 60,
        broadcastBoundaryMinutes % 60);

    // --- 0) todayKey (broadcast day) ---
    const ProgramDateTime nowDt = NowLocalProgramDateTime();
    const sys_minutes nowTp = ToSysMinutes(nowDt);
    std::chrono::sys_days todayKey{};
    {
        // ToSysMinutes 실패(0) 가능성 거의 없지만, 안전側: year<=0 のときは全て upcoming 扱い
        if (nowDt.year > 0) todayKey = BroadcastDayKey(nowTp, broadcastBoundaryMinutes);
        else todayKey = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    }

    // --- 1) EIT .epg -> EITTextEvent 群 ---
    std::vector<EITTextEvent> events;
    events.reserve(2048);

    const bool ok = ForEachEITTextEventInDump(
        epgDumpPath,
        [&](const EITTextEvent& ev)
        {
            events.push_back(ev);
        });

    if (!ok) {
        DebugLog(L"[EIT-HTML] EITTextDumpLoader からの読み込みに失敗しました。");
        return false;
    }
    if (events.empty()) {
        DebugLog(L"[EIT-HTML] EIT イベントが 0 件でした。");
        return false;
    }

    EIT_HTML_LOG(L"[EIT-HTML] 読み込みイベント数: %zu", events.size());

    // --- 2) channellist 読み込み ---
    std::vector<ServiceEntry> services;
    std::unordered_map<uint64_t, std::size_t> serviceIndexMap;
    bool hasServiceList = false;

    if (!channelListPath.empty()) {
        if (LoadChannelList(channelListPath, services)) {
            hasServiceList = true;
            serviceIndexMap.reserve(services.size() * 2);

            std::size_t dup = 0;
            for (std::size_t i = 0; i < services.size(); ++i) {
                const auto& s = services[i];
                const uint64_t key = MakeServiceKey(s.serviceId, s.tsid, s.onid);
                auto [it, inserted] = serviceIndexMap.emplace(key, i);
                if (!inserted) ++dup;
            }
            if (dup) {
                DebugLog(L"[EIT-HTML] channellist 重複キー: %zu 件（先勝ちで採用）", dup);
            }

            EIT_HTML_LOG(L"[EIT-HTML] channellist 読み込み成功: %zu services", services.size());
        }
        else {
            DebugLog(L"[EIT-HTML] channellist 読み込み失敗: %ls (サービスリストなしで出力します)",
                channelListPath.c_str());
        }
    }

    // --- 3) ViewItem に変換 ---
    struct ViewItem {
        ProgramDateTime        start{};
        ProgramDateTime        end{};
        const EITTextEvent* src = nullptr;
        int                    serviceIndex = -1;
        bool                   hasStart = false;
        bool                   hasDurMin = false;
        int                    durationMinutes = 0;

        bool                   hasGenre = false;
        uint8_t                genreMajor = 0xFF;   // 0..15, 0xFF=unknown
        std::wstring           genreLabel;          // 可能なら元表記
    };

    std::vector<ViewItem> view;
    view.reserve(events.size());

    std::size_t matchedCount = 0;
    std::size_t unmatchedCount = 0;

    std::size_t genrePresent = 0;
    std::size_t genreMissing = 0;
    std::unordered_map<std::wstring, std::size_t> genreCountMap;
    genreCountMap.reserve(64);

    std::array<std::size_t, 16> majorCount{};
    majorCount.fill(0);
    std::size_t majorUnknownCount = 0;

    for (auto& ev : events) {
        ViewItem item;
        item.src = &ev;

        ProgramDateTime dt{};
        if (DecodeStartTimeRawHex(ev.startTimeRawHex, dt)) {
            item.start = dt;
            item.hasStart = true;

            int minutes = 0;
            if (ParseDurationToMinutes(ev.durationHHMMSS, minutes) && minutes > 0) {
                item.durationMinutes = minutes;
                item.hasDurMin = true;
                item.end = AddMinutes(dt, minutes);
            }
        }

        // ジャンル抽出
        {
            const auto gr = genre::Extract(ev);
            item.hasGenre = gr.has;
            item.genreMajor = gr.major;
            item.genreLabel = Trim(gr.label);

            if (!item.genreLabel.empty()) {
                ++genrePresent;
                genreCountMap[item.genreLabel] += 1;
            }
            else {
                ++genreMissing;
            }

            if (item.genreMajor != 0xFF) {
                majorCount[item.genreMajor & 0x0F] += 1;
            }
            else {
                majorUnknownCount += 1;
            }
        }

        if (hasServiceList) {
            const uint64_t key = MakeServiceKey(ev.serviceId, ev.tsid, ev.onid);
            auto it = serviceIndexMap.find(key);
            if (it != serviceIndexMap.end()) {
                item.serviceIndex = static_cast<int>(it->second);
                ++matchedCount;
            }
            else {
                item.serviceIndex = -1;
                ++unmatchedCount;
            }
        }

        view.push_back(std::move(item));
    }

    if (hasServiceList) {
        EIT_HTML_LOG(L"[EIT-HTML] サービスマッチ結果: matched=%zu, unmatched=%zu",
            matchedCount, unmatchedCount);
    }

    {
        EIT_HTML_LOG(L"[EIT-HTML] ジャンル認識: present=%zu, missing=%zu, unique=%zu, unknownMajor=%zu",
            genrePresent, genreMissing, genreCountMap.size(), majorUnknownCount);

        std::vector<std::pair<std::wstring, std::size_t>> gvec;
        gvec.reserve(genreCountMap.size());
        for (const auto& kv : genreCountMap) gvec.push_back(kv);

        std::sort(gvec.begin(), gvec.end(),
            [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });

        constexpr std::size_t kTop = 24;
        for (std::size_t i = 0; i < gvec.size() && i < kTop; ++i) {
            DebugLog(L"[EIT-HTML] GenreTop[%02zu] %ls : %zu",
                i, gvec[i].first.c_str(), gvec[i].second);
        }
        if (gvec.size() > kTop) {
            DebugLog(L"[EIT-HTML] GenreTop ... (%zu total)", gvec.size());
        }
    }

    // --- 4) 開始時刻でソート（全体） ---
    std::stable_sort(
        view.begin(), view.end(),
        [](const ViewItem& a, const ViewItem& b) {
            if (a.hasStart != b.hasStart)
                return a.hasStart && !b.hasStart;
            if (!a.hasStart && !b.hasStart)
                return false;
            return a.start < b.start;
        });

    // --- 5) サービスごとにグルーピング ---
    std::vector<std::vector<const ViewItem*>> eventsByService;
    std::vector<const ViewItem*> others; // unmatched or no-service-list mode container

    if (hasServiceList) {
        eventsByService.resize(services.size());
        for (const auto& v : view) {
            if (v.serviceIndex >= 0 &&
                static_cast<std::size_t>(v.serviceIndex) < services.size())
            {
                eventsByService[static_cast<std::size_t>(v.serviceIndex)].push_back(&v);
            }
            else {
                others.push_back(&v);
            }
        }
    }
    else {
        for (const auto& v : view) others.push_back(&v);
    }

    // --- 6) 文字列フォーマット ---
    auto FormatYMD = [](const ProgramDateTime& dt) -> std::string
        {
            if (dt.year <= 0) return {};
            std::ostringstream oss;
            oss << dt.year << '/'
                << std::setfill('0') << std::setw(2) << dt.month << '/'
                << std::setfill('0') << std::setw(2) << dt.day;
            return oss.str();
        };

    auto FormatHM = [](const ProgramDateTime& dt) -> std::string
        {
            if (dt.year <= 0) return {};
            std::ostringstream oss;
            oss << std::setfill('0')
                << std::setw(2) << dt.hour << ':'
                << std::setw(2) << dt.minute;
            return oss.str();
        };

    // 全体の時間範囲（表示用：開始～終了）
    ProgramDateTime firstDt{};
    ProgramDateTime lastDt{};
    bool haveRange = false;
    for (const auto& v : view) {
        if (!v.hasStart) continue;
        if (!haveRange) {
            firstDt = v.start;
            lastDt = (v.hasDurMin ? v.end : v.start);
            haveRange = true;
        }
        else {
            if (v.start < firstDt) firstDt = v.start;
            const ProgramDateTime endCand = (v.hasDurMin ? v.end : v.start);
            if (lastDt < endCand) lastDt = endCand;
        }
    }

    // 入力ファイル時刻を埋め込む（更新確認に使える）
    std::string epgMtime;
    std::string chMtime;
    {
        std::filesystem::file_time_type ft{};
        if (TryGetLastWriteTime(epgDumpPath, ft)) epgMtime = FormatFileTimeLocal(ft);
        if (!channelListPath.empty() && TryGetLastWriteTime(channelListPath, ft)) chMtime = FormatFileTimeLocal(ft);
    }

    // --- HTML ドキュメント共通部を組み立てる関数 ---
    struct DayInfo {
        std::string  key;     // data-day 用（YYYY/MM/DD）
        std::wstring labelW;  // 表示用（YYYY/MM/DD(曜)）
    };

    struct DayBuild {
        std::vector<DayInfo> infos;
        std::unordered_set<std::string> keySet;
        std::string sections;
    };

    auto AppendCommonCss = [&](std::string& html)
        {
            html += "<style>\n";
            html += R"CSS(
body{font-family:'Yu Gothic UI','Meiryo',sans-serif;font-size:12px;margin:8px;color:#000;}
h1{font-size:18px;margin:0.5em 0;}
h2{font-size:14px;margin:1.5em 0 0.4em 0;border-left:4px solid #666;padding-left:0.4em;}
h3{font-size:13px;margin:1.0em 0 0.4em 0;border-left:3px solid #999;padding-left:0.4em;color:#222;}
p.range{margin:0 0 0.5em 0;color:#555;font-size:11px;}
p.note{margin:0 0 0.5em 0;color:#555;font-size:11px;}

.top-links{margin:6px 0 10px 0;font-size:12px;}
.top-links a{margin-right:12px;}

.view-bar,.layout-bar,.day-bar,.genre-bar{display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin:6px 0 10px 0;}
.view-bar button,.layout-bar button,.day-bar button,.genre-bar button{padding:4px 10px;border:1px solid #888;border-radius:6px;background:#f7f7f7;cursor:pointer;font-size:12px;}
.view-bar button.active{background:#dfefff;border-color:#4a78c2;}
.layout-bar button.active{background:#fff2cc;border-color:#c28a00;}
.day-bar button.active{background:#e8ffe8;border-color:#2f8f2f;}
.genre-bar button.active{background:#ffe6f2;border-color:#c24a78;}
.view-bar .note,.layout-bar .note,.day-bar .note,.genre-bar .note{color:#555;font-size:11px;margin-left:6px;}

.table-wrap{
  overflow-y:auto;
  overflow-x:scroll;
  scrollbar-gutter:stable both-edges;
  border:1px solid #ccc;
  max-height:75vh;
  max-width:calc(100vw - 24px);
}
table.prog{border-collapse:collapse;width:max-content;min-width:100%;margin:0;table-layout:fixed;}
table.prog th,table.prog td{border:1px solid #ccc;padding:2px 4px;vertical-align:top;}
table.prog th.time,table.prog td.time{white-space:nowrap;width:6em;}
table.prog th.dur,table.prog td.dur{width:3em;text-align:right;}
table.prog th.eid,table.prog td.eid{width:4em;text-align:right;}
table.prog th.title{width:18em;}
table.prog td.title{font-weight:bold;min-width:18em;}
table.prog td.summary{font-size:90%;color:#333;}
table.prog tr.date-sep th{background:#f3f3f3;color:#222;text-align:left;font-weight:bold;}

table.glist th.time,table.glist td.time{white-space:nowrap;width:8em;}
table.glist th.svc,table.glist td.svc{white-space:nowrap;width:12em;}
table.glist th.genre,table.glist td.genre{white-space:nowrap;width:12em;}
table.glist td.title{font-weight:bold;min-width:18em;}
table.glist tr.genre-sep th{background:#fff7e6;color:#222;text-align:left;font-weight:bold;}

.epg-wrap{
  overflow-y:auto;
  overflow-x:scroll;
  scrollbar-gutter:stable both-edges;
  border:1px solid #ccc;
  max-height:80vh;
  max-width:calc(100vw - 24px);
}
table.epg{border-collapse:collapse;table-layout:fixed;width:max-content;min-width:100%;}
table.epg th,table.epg td{border:1px solid #ccc;padding:2px 4px;vertical-align:top;}
table.epg thead th{position:sticky;top:0;background:#f0f0f0;z-index:3;}
table.epg th.time-col{position:sticky;left:0;background:#f8f8f8;z-index:4;white-space:nowrap;width:6.5em;}
table.epg td.empty{background:#fafafa;}
table.epg td.prog-cell{background:#fff;white-space:normal;overflow-wrap:anywhere;word-break:break-word;color:#000;}
table.epg th.svc{width:240px;max-width:240px;}
table.epg td.prog-cell .t{font-weight:bold;display:-webkit-box;-webkit-box-orient:vertical;-webkit-line-clamp:2;overflow:hidden;}
table.epg td.prog-cell .tm{font-size:11px;color:#444;}
table.epg td.prog-cell .s{font-size:11px;color:#333;line-height:1.2;display:-webkit-box;-webkit-box-orient:vertical;-webkit-line-clamp:3;overflow:hidden;}
table.epg td.prog-cell .eid{float:none;display:block;text-align:right;font-size:10px;color:#666;}
table.epg tr.tick th,table.epg tr.tick td{border-top:1px solid #aaa;}
table.epg tr.hour th,table.epg tr.hour td{border-top:2px solid #666;}

body[data-view="main"] .col-sub{display:none;}
body[data-view="sub"] .col-main{display:none;}
body[data-layout="grid"] .genre-view{display:none;}
body[data-layout="genre"] .grid-view{display:none;}

table.glist{border-collapse:collapse;width:max-content;min-width:100%;table-layout:fixed;}
table.glist th,table.glist td{border:1px solid #ccc;padding:2px 4px;vertical-align:top;}
table.glist th.time,table.glist td.time{white-space:nowrap;width:8em;}
table.glist th.svc,table.glist td.svc{white-space:nowrap;width:12em;}
table.glist th.genre,table.glist td.genre{white-space:nowrap;width:12em;}
table.glist td.title{
  font-weight:bold;
  min-width:18em;
}
table.glist tr.genre-sep th{background:#fff7e6;color:#222;text-align:left;font-weight:bold;}

table.prog td.title .epg-inline-switch-host,
table.glist td.title .epg-inline-switch-host{
  margin-top:8px;
  gap:4px;
}
table.prog td.title .epg-inline-switch-btn,
table.glist td.title .epg-inline-switch-btn{
  padding:3px 8px;
  font-size:11px;
}

.genre-dim{opacity:0.18;}
)CSS";
            html += "</style>\n";
        };

    auto AppendCommonJs = [&](std::string& html)
        {
            // JSはASCIIのみ
            html += "<script>\n";
            html += R"JS(
(function(){
  function pageKey(name){
    var p = document.body.getAttribute('data-page') || 'default';
    return 'eit_' + p + '_' + name;
  }

  // ----- view mode -----
  function setMode(m){
    document.body.setAttribute('data-view', m);
    try{ localStorage.setItem(pageKey('view_mode'), m); }catch(e){}
    var btns = document.querySelectorAll('.view-btn');
    for(var i=0;i<btns.length;i++){
      var b = btns[i];
      var on = (b.getAttribute('data-mode') === m);
      if(on) b.classList.add('active'); else b.classList.remove('active');
    }
  }
  function bind(id, mode){
    var el = document.getElementById(id);
    if(!el) return;
    el.addEventListener('click', function(){ setMode(mode); });
  }
  bind('btn-all','all');
  bind('btn-main','main');
  bind('btn-sub','sub');
  (function(){
    var init='all';
    try{ var v = localStorage.getItem(pageKey('view_mode')); if(v) init=v; }catch(e){}
    if(init!=='all' && init!=='main' && init!=='sub') init='all';
    setMode(init);
  })();

  // ----- layout -----
  function setLayout(m){
    document.body.setAttribute('data-layout', m);
    try{ localStorage.setItem(pageKey('layout'), m); }catch(e){}
    var btns = document.querySelectorAll('.layout-btn');
    for(var i=0;i<btns.length;i++){
      var b = btns[i];
      var on = (b.getAttribute('data-layout') === m);
      if(on) b.classList.add('active'); else b.classList.remove('active');
    }
  }
  (function(){
    var g = document.getElementById('btn-layout-grid');
    var r = document.getElementById('btn-layout-genre');
    if(g) g.addEventListener('click', function(){ setLayout('grid'); });
    if(r) r.addEventListener('click', function(){ setLayout('genre'); });
    var initL='grid';
    try{ var v = localStorage.getItem(pageKey('layout')); if(v) initL=v; }catch(e){}
    if(initL!=='grid' && initL!=='genre') initL='grid';
    setLayout(initL);
  })();

  // ----- day filter -----
  function dayOrder(){
    var btns = document.querySelectorAll('.day-btn[data-day]');
    var arr = [];
    for(var i=0;i<btns.length;i++){
      var d = btns[i].getAttribute('data-day') || '';
      if(d && d !== 'all') arr.push(d);
    }
    return arr;
  }

  function setDay(d){
    var secs = document.querySelectorAll('.day-section');
    for(var i=0;i<secs.length;i++){
      var s = secs[i];
      var k = s.getAttribute('data-day') || '';
      s.style.display = (d==='all' || k===d) ? 'block' : 'none';
    }
    var btns = document.querySelectorAll('.day-btn');
    for(var j=0;j<btns.length;j++){
      var b = btns[j];
      var on = (b.getAttribute('data-day') === d);
      if(on) b.classList.add('active'); else b.classList.remove('active');
    }
    try{ localStorage.setItem(pageKey('day'), d); }catch(e){}
  }

  function moveDay(dir){
    var ord = dayOrder();
    if(!ord.length) return;
    var cur = '';
    try{ cur = localStorage.getItem(pageKey('day')) || ''; }catch(e){}
    if(!cur || cur === 'all'){
      setDay(ord[0]);
      return;
    }
    var idx = -1;
    for(var i=0;i<ord.length;i++){
      if(ord[i] === cur){ idx=i; break; }
    }
    if(idx < 0){ setDay(ord[0]); return; }
    var next = idx + dir;
    if(next < 0) next = ord.length - 1;
    if(next >= ord.length) next = 0;
    setDay(ord[next]);
  }

  (function(){
    var dayBtns = document.querySelectorAll('.day-btn');
    for(var i=0;i<dayBtns.length;i++){
      dayBtns[i].addEventListener('click', function(){
        var d = this.getAttribute('data-day') || 'all';
        setDay(d);
      });
    }

    var prev = document.getElementById('btn-day-prev');
    var next = document.getElementById('btn-day-next');
    if(prev) prev.addEventListener('click', function(){ moveDay(-1); });
    if(next) next.addEventListener('click', function(){ moveDay(+1); });

    // ArrowLeft / ArrowRight
    document.addEventListener('keydown', function(ev){
      if(ev.key === 'ArrowLeft'){ moveDay(-1); }
      else if(ev.key === 'ArrowRight'){ moveDay(+1); }
    });

    // init
    var firstSec = document.querySelector('.day-section');
    var firstDay = firstSec ? (firstSec.getAttribute('data-day')||'') : '';
    if(!firstDay){ return; }
    var initDay = '';
    try{ var v = localStorage.getItem(pageKey('day')); if(v) initDay=v; }catch(e){}
    if(!initDay) initDay = firstDay;
    if(initDay !== 'all'){
      var exists = document.querySelector('.day-section[data-day="' + initDay + '"]');
      if(!exists) initDay = firstDay;
    }
    setDay(initDay);
  })();

  // ----- genre filter (major) -----
  function setGenre(g){
    document.body.setAttribute('data-genre', g);
    try{ localStorage.setItem(pageKey('genre'), g); }catch(e){}
    var btns = document.querySelectorAll('.genre-btn');
    for(var i=0;i<btns.length;i++){
      var b = btns[i];
      var on = (b.getAttribute('data-genre') === g);
      if(on) b.classList.add('active'); else b.classList.remove('active');
    }

    // grid view: dim (do NOT hide to avoid rowspan collapse)
    var cells = document.querySelectorAll('td.prog-cell[data-gm]');
    for(var j=0;j<cells.length;j++){
      var c = cells[j];
      var gm = c.getAttribute('data-gm') || 'unknown';
      if(g === 'all'){
        c.classList.remove('genre-dim');
      }else{
        if(gm === g) c.classList.remove('genre-dim');
        else c.classList.add('genre-dim');
      }
    }

    // genre list: hide non-matching rows/headers
    var gitems = document.querySelectorAll('tr.gitem[data-gm]');
    for(var k=0;k<gitems.length;k++){
      var r = gitems[k];
      var gm2 = r.getAttribute('data-gm') || 'unknown';
      if(g === 'all' || gm2 === g) r.style.display = '';
      else r.style.display = 'none';
    }
    var headers = document.querySelectorAll('tr.genre-sep[data-gm]');
    for(var h=0;h<headers.length;h++){
      var hr = headers[h];
      var gm3 = hr.getAttribute('data-gm') || 'unknown';
      if(g === 'all' || gm3 === g) hr.style.display = '';
      else hr.style.display = 'none';
    }
  }

  (function(){
    var btns = document.querySelectorAll('.genre-btn');
    for(var i=0;i<btns.length;i++){
      btns[i].addEventListener('click', function(){
        var g = this.getAttribute('data-genre') || 'all';
        setGenre(g);
      });
    }
    var initG='all';
    try{ var v = localStorage.getItem(pageKey('genre')); if(v) initG=v; }catch(e){}
    setGenre(initG);
  })();

})();
)JS";
            html += "</script>\n";
        };

    auto EmitTableHeader = [&](std::string& dst)
        {
            dst += "<div class=\"table-wrap\">";
            dst += "<table class=\"prog\">\n";
            dst += "<thead><tr>";
            dst += "<th class=\"time\">" + HtmlEscapeFromUtf8(L"開始") + "</th>";
            dst += "<th class=\"time\">" + HtmlEscapeFromUtf8(L"終了") + "</th>";
            dst += "<th class=\"dur\">" + HtmlEscapeFromUtf8(L"分") + "</th>";
            dst += "<th class=\"eid\">EID</th>";
            dst += "<th class=\"title\">" + HtmlEscapeFromUtf8(L"番組名") + "</th>";
            dst += "<th class=\"summary\">" + HtmlEscapeFromUtf8(L"概要") + "</th>";
            dst += "</tr></thead>\n<tbody>\n";
        };

    auto EmitTableFooter = [&](std::string& dst)
        {
            dst += "</tbody></table></div>\n";
        };

    auto EmitDateSepRow = [&](std::string& dst, const std::string& ymd, const std::wstring& ymdLabelW)
        {
            dst += "<tr class=\"date-sep\"><th colspan=\"6\">";
            if (!ymd.empty()) dst += HtmlEscapeFromUtf8(ymdLabelW);
            else dst += "(no date)";
            dst += "</th></tr>\n";
        };

    auto AppendProgramSwitchDataAttrs =
        [&](std::string& dst,
            const ViewItem& item,
            const std::wstring& svcName,
            const std::wstring& titleW,
            const std::wstring& summaryW,
            const std::wstring& genreW)
        {
            if (!item.src) return;

            const EITTextEvent& ev = *item.src;

            const std::string dateStr = item.hasStart ? FormatYMD(item.start) : std::string();
            const std::string startStr = item.hasStart ? FormatHM(item.start) : std::string();
            const std::string endStr =
                (item.hasStart && item.hasDurMin) ? FormatHM(item.end) : std::string();

            std::wstring weekdayW;
            if (item.hasStart) {
                const sys_minutes stp = ToSysMinutes(item.start);
                weekdayW = WeekdayShortJa(std::chrono::floor<std::chrono::days>(stp));
            }

            std::string timeAttr;
            if (!startStr.empty()) {
                timeAttr = startStr;
                timeAttr += "&#12316;";
                timeAttr += (endStr.empty() ? "?" : endStr);
                if (item.hasDurMin) {
                    timeAttr += " (";
                    timeAttr += std::to_string(item.durationMinutes);
                    timeAttr += "m)";
                }
            }

            dst += " data-epg-program=\"1\"";

            dst += " data-title=\"";
            dst += HtmlEscapeAttrFromUtf8(titleW);
            dst += "\"";

            if (!dateStr.empty()) {
                const std::wstring dateW(dateStr.begin(), dateStr.end());
                dst += " data-date=\"";
                dst += HtmlEscapeAttrFromUtf8(dateW);
                dst += "\"";
            }

            if (!weekdayW.empty()) {
                dst += " data-weekday=\"";
                dst += HtmlEscapeAttrFromUtf8(weekdayW);
                dst += "\"";
            }

            if (!timeAttr.empty()) {
                const std::wstring timeW(timeAttr.begin(), timeAttr.end());
                dst += " data-time=\"";
                dst += HtmlEscapeAttrFromUtf8(timeW);
                dst += "\"";
            }

            if (!svcName.empty()) {
                dst += " data-service=\"";
                dst += HtmlEscapeAttrFromUtf8(svcName);
                dst += "\"";
            }

            if (!summaryW.empty()) {
                dst += " data-summary=\"";
                dst += HtmlEscapeAttrFromUtf8(summaryW);
                dst += "\"";
            }

            if (!genreW.empty()) {
                dst += " data-genre=\"";
                dst += HtmlEscapeAttrFromUtf8(genreW);
                dst += "\"";
            }

            dst += " data-network-id=\"";
            dst += std::to_string(static_cast<unsigned>(ev.onid));
            dst += "\"";

            dst += " data-ts-id=\"";
            dst += std::to_string(static_cast<unsigned>(ev.tsid));
            dst += "\"";

            dst += " data-service-id=\"";
            dst += std::to_string(static_cast<unsigned>(ev.serviceId));
            dst += "\"";

            dst += " data-event-id=\"";
            dst += std::to_string(static_cast<unsigned>(ev.eventId));
            dst += "\"";
        };

    auto EmitOneRow = [&](const ViewItem& item, std::string& dst)
        {
            if (!item.src) return;

            const EITTextEvent& ev = *item.src;

            const std::string startStr = item.hasStart ? FormatHM(item.start) : "";
            const std::string endStr = (item.hasStart && item.hasDurMin) ? FormatHM(item.end) : "";
            const std::string durStr = item.hasDurMin ? std::to_string(item.durationMinutes) : "";

            std::wstring titleW = ev.shortTitle;
            if (titleW.empty()) titleW = L"(no title)";

            std::wstring summaryW = ev.shortSummary;
            if (!ev.extendedItems.empty()) {
                if (!summaryW.empty()) summaryW += L"  ";
                summaryW += L"…";
                summaryW += ev.extendedItems.front().second;
            }
            summaryW = TruncateW(summaryW, 220);

            std::wstring svcName = L"(unknown)";
            if (item.serviceIndex >= 0 &&
                static_cast<std::size_t>(item.serviceIndex) < services.size()) {
                svcName = services[static_cast<std::size_t>(item.serviceIndex)].serviceName;
            }

            std::wstring genreCell;
            if (item.hasGenre) {
                genreCell = genre::MajorLabel(static_cast<uint8_t>(item.genreMajor & 0x0F));
                if (!item.genreLabel.empty() && item.genreLabel != genreCell) {
                    genreCell += L" (" + item.genreLabel + L")";
                }
            }

            dst += "<tr>";
            dst += "<td class=\"time\">" + startStr + "</td>";
            dst += "<td class=\"time\">" + endStr + "</td>";
            dst += "<td class=\"dur\">" + durStr + "</td>";
            dst += "<td class=\"eid\">" + std::to_string(ev.eventId) + "</td>";

            dst += "<td class=\"title\"";
            AppendProgramSwitchDataAttrs(dst, item, svcName, titleW, summaryW, genreCell);
            dst += ">";
            dst += HtmlEscapeFromUtf8(titleW);
            dst += "</td>";

            dst += "<td class=\"summary\">";
            if (!summaryW.empty()) dst += HtmlEscapeFromUtf8(summaryW);
            dst += "</td>";

            dst += "</tr>\n";
        };

    // --- HTML doc builder ---
    auto BuildDocument = [&](const char* pageId,
        const std::wstring& titleW,
        const DayBuild& db,
        const std::wstring& linkOtherTextW,
        const std::wstring& linkOtherFileW,
        const std::vector<const ViewItem*>& otherItems,
        bool showMetaNotes) -> std::string
        {
            std::string linkOtherFileUtf8;
            (void)WStringToUtf8(linkOtherFileW, linkOtherFileUtf8);

            std::string html;
            html.reserve(1024 * 256);

            html += "<!DOCTYPE html>\n";
            html += "<html lang=\"ja\">\n";
            html += "<head>\n";
            html += "<meta charset=\"UTF-8\" />\n";
            html += "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n";
            html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />\n";
            html += "<title>";
            html += HtmlEscapeFromUtf8(titleW);
            html += "</title>\n";

            // meta
            html += "<!-- source_epg=" + HtmlEscapeFromUtf8(epgDumpPath) + " ; mtime=" + epgMtime + " -->\n";
            if (!channelListPath.empty()) {
                html += "<!-- source_chlist=" + HtmlEscapeFromUtf8(channelListPath) + " ; mtime=" + chMtime + " -->\n";
            }

            AppendCommonCss(html);
            html += "</head>\n";

            // 初期表示：all / grid / genre=all
            html += "<body data-view=\"all\" data-layout=\"grid\" data-genre=\"all\" data-page=\"";
            html += pageId;
            html += "\">\n";

            // タイトル
            html += "<h1>";
            html += HtmlEscapeFromUtf8(titleW);
            html += "</h1>\n";

            // cross links
            if (!linkOtherFileUtf8.empty()) {
                html += "<div class=\"top-links\">";
                html += "<a href=\"";
                html += linkOtherFileUtf8;
                html += "\">";
                html += HtmlEscapeFromUtf8(linkOtherTextW);
                html += "</a>";
                html += "</div>\n";
            }

            if (haveRange) {
                html += "<p class=\"range\">";
                html += HtmlEscapeFromUtf8(L"対象期間: ");
                html += FormatYMD(firstDt);
                html += " ";
                html += FormatHM(firstDt);
                html += " ~ ";
                html += FormatYMD(lastDt);
                html += " ";
                html += FormatHM(lastDt);
                html += "</p>\n";
            }

            {
                std::wstringstream ss;
                ss << L"放送日区切り: " << std::setw(2) << std::setfill(L'0') << (broadcastBoundaryMinutes / 60)
                    << L":" << std::setw(2) << std::setfill(L'0') << (broadcastBoundaryMinutes % 60)
                    << L"（深夜帯は前日扱い）";
                html += "<p class=\"note\">";
                html += HtmlEscapeFromUtf8(ss.str());
                html += "</p>\n";
            }

            if (showMetaNotes) {
                {
                    std::wstringstream ss;
                    ss << L"ジャンル認識: present=" << genrePresent << L", missing=" << genreMissing
                        << L", unique=" << genreCountMap.size() << L", unknownMajor=" << majorUnknownCount;
                    html += "<p class=\"note\">";
                    html += HtmlEscapeFromUtf8(ss.str());
                    html += "</p>\n";
                }
                if (!epgMtime.empty()) {
                    html += "<p class=\"note\">";
                    html += HtmlEscapeFromUtf8(L"EPG更新日時: ");
                    html += epgMtime;
                    html += "</p>\n";
                }
            }

            // 切替ボタン（メイン/サブ）
            html += "<div class=\"view-bar\">";
            html += "<button type=\"button\" class=\"view-btn\" data-mode=\"all\" id=\"btn-all\">";
            html += HtmlEscapeFromUtf8(L"メイン＋サブ");
            html += "</button>";
            html += "<button type=\"button\" class=\"view-btn\" data-mode=\"main\" id=\"btn-main\">";
            html += HtmlEscapeFromUtf8(L"メインのみ");
            html += "</button>";
            html += "<button type=\"button\" class=\"view-btn\" data-mode=\"sub\" id=\"btn-sub\">";
            html += HtmlEscapeFromUtf8(L"サブのみ");
            html += "</button>";
            html += "<span class=\"note\">";
            html += HtmlEscapeFromUtf8(L"※列表示を切替（内容は同一HTML内）");
            html += "</span>";
            html += "</div>\n";

            // 表示形式（時間表 / ジャンル別）
            html += "<div class=\"layout-bar\">";
            html += "<button type=\"button\" class=\"layout-btn\" data-layout=\"grid\" id=\"btn-layout-grid\">";
            html += HtmlEscapeFromUtf8(L"時間表");
            html += "</button>";
            html += "<button type=\"button\" class=\"layout-btn\" data-layout=\"genre\" id=\"btn-layout-genre\">";
            html += HtmlEscapeFromUtf8(L"表一覧");
            html += "</button>";
            html += "<span class=\"note\">";
            html += HtmlEscapeFromUtf8(L"※ジャンルの認識確認用");
            html += "</span>";
            html += "</div>\n";

            // ジャンルフィルタ（major）
            {
                html += "<div class=\"genre-bar\" id=\"genrebar\">";
                html += "<button type=\"button\" class=\"genre-btn\" data-genre=\"all\" id=\"btn-genre-all\">";
                html += HtmlEscapeFromUtf8(L"全ジャンル");
                html += "</button>";

                for (int m = 0; m < 16; ++m) {
                    if (majorCount[static_cast<std::size_t>(m)] == 0) continue;

                    std::wstring label = genre::MajorLabel(static_cast<uint8_t>(m));
                    std::wstringstream ss;
                    ss << label << L" (" << majorCount[static_cast<std::size_t>(m)] << L")";

                    html += "<button type=\"button\" class=\"genre-btn\" data-genre=\"";
                    html += std::to_string(m);
                    html += "\" id=\"btn-genre-";
                    html += std::to_string(m);
                    html += "\">";
                    html += HtmlEscapeFromUtf8(ss.str());
                    html += "</button>";
                }

                if (majorUnknownCount > 0) {
                    std::wstringstream ss;
                    ss << L"不明 (" << majorUnknownCount << L")";
                    html += "<button type=\"button\" class=\"genre-btn\" data-genre=\"unknown\" id=\"btn-genre-unknown\">";
                    html += HtmlEscapeFromUtf8(ss.str());
                    html += "</button>";
                }

                html += "<span class=\"note\">";
                html += HtmlEscapeFromUtf8(L"※時間表は“対象以外を薄く表示”（rowspan崩れ防止）／ジャンル別は対象以外を非表示");
                html += "</span>";
                html += "</div>\n";
            }

            // day bar
            if (!db.infos.empty()) {
                html += "<div class=\"day-bar\" id=\"daybar\">";
                html += "<button type=\"button\" class=\"day-nav\" id=\"btn-day-prev\">";
                html += HtmlEscapeFromUtf8(L"前日");
                html += "</button>";
                html += "<button type=\"button\" class=\"day-nav\" id=\"btn-day-next\">";
                html += HtmlEscapeFromUtf8(L"翌日");
                html += "</button>";

                html += "<button type=\"button\" class=\"day-btn\" data-day=\"all\" id=\"btn-day-all\">";
                html += HtmlEscapeFromUtf8(L"全日");
                html += "</button>";

                for (std::size_t i = 0; i < db.infos.size(); ++i) {
                    html += "<button type=\"button\" class=\"day-btn\" data-day=\"";
                    html += db.infos[i].key;
                    html += "\" id=\"btn-day-";
                    html += std::to_string(i);
                    html += "\">";
                    html += HtmlEscapeFromUtf8(db.infos[i].labelW);
                    html += "</button>";
                }

                html += "<span class=\"note\">";
                html += HtmlEscapeFromUtf8(L"※放送日(切替時刻あり)を切替（前日/翌日、←/→でも移動）");
                html += "</span>";
                html += "</div>\n";
            }

            // sections
            html += db.sections;

            // others (unmatched)
            if (!otherItems.empty()) {
                html += "<h2>";
                html += HtmlEscapeFromUtf8(L"未マッチ / 未整形イベント");
                html += "</h2>\n";

                EmitTableHeader(html);

                std::string curDate;
                std::wstring curLabelW;
                bool first = true;

                for (const auto* pv : otherItems) {
                    if (!pv) continue;
                    const auto& item = *pv;

                    const std::string d = item.hasStart ? FormatYMD(item.start) : std::string();
                    std::wstring dLabelW = L"(no date)";
                    if (!d.empty()) {
                        // weekday label
                        const sys_minutes stp = ToSysMinutes(item.start);
                        const auto dayCal = std::chrono::floor<std::chrono::days>(stp);
                        dLabelW = MakeLabelWithWeekday(d, dayCal);
                    }

                    if (first || d != curDate) {
                        EmitDateSepRow(html, d, dLabelW);
                        curDate = d;
                        curLabelW = dLabelW;
                        first = false;
                    }
                    EmitOneRow(item, html);
                }

                EmitTableFooter(html);
            }

            AppendCommonJs(html);

            html += "</body>\n</html>\n";
            return html;
        };

    // ---------------- 以降: サービスリストありの場合は grid/genre view を day-by-day で sections 化 ----------------
    EIT_HTML_LOG(L"[EIT-HTML] HTML 本文構築開始.");

    DayBuild buildPast, buildUpcoming;
    buildPast.keySet.reserve(16);
    buildUpcoming.keySet.reserve(16);
    buildPast.sections.reserve(256 * 1024);
    buildUpcoming.sections.reserve(256 * 1024);

    // others split containers
    std::vector<const ViewItem*> othersPast;
    std::vector<const ViewItem*> othersUpcoming;

    // ---- サービスリストあり ----
    if (hasServiceList) {

        // active service list
        std::vector<std::size_t> activeServices;
        activeServices.reserve(services.size());
        for (std::size_t i = 0; i < services.size(); ++i) {
            if (!eventsByService[i].empty())
                activeServices.push_back(i);
        }

        auto NormalizeTitle = [](std::wstring_view s) -> std::wstring {
            std::wstring out;
            out.reserve(s.size());
            for (wchar_t ch : s) {
                if (ch == L' ' || ch == L'\t' || ch == L'　') continue;
                out.push_back(ch);
            }
            return out;
            };

        auto ScoreService = [&](const ServiceEntry& s) -> int {
            int score = 0;
            if (s.remoteControlKeyId > 0) score += 5000 - s.remoteControlKeyId;
            if (s.serviceType == 0x01) score += 100;
            if (s.serviceName.find(L"サブ") == std::wstring::npos) score += 30;
            if (s.serviceName.find(L"副") == std::wstring::npos) score += 30;
            score += 1000 - static_cast<int>(s.serviceId);
            return score;
            };

        auto BuildSigMap = [&](std::size_t svcIndex) {
            std::unordered_map<long long, std::wstring> m;
            m.reserve(eventsByService[svcIndex].size());

            for (const auto* pv : eventsByService[svcIndex]) {
                if (!pv || !pv->hasStart || !pv->hasDurMin || pv->durationMinutes <= 0) continue;
                const auto tp = ToSysMinutes(pv->start);
                const long long k = tp.time_since_epoch().count();
                const auto& ev = *pv->src;
                std::wstring title = NormalizeTitle(ev.shortTitle);
                m[k] = title + L"|" + std::to_wstring(pv->durationMinutes);
            }
            return m;
            };

        auto Similarity =
            [&](const std::unordered_map<long long, std::wstring>& a,
                const std::unordered_map<long long, std::wstring>& b) -> double
            {
                if (a.empty() || b.empty()) return 0.0;

                const auto& smallerMap = (a.size() <= b.size()) ? a : b;
                const auto& largerMap = (a.size() <= b.size()) ? b : a;

                std::size_t match = 0;
                for (const auto& kv : smallerMap) {
                    auto it = largerMap.find(kv.first);
                    if (it != largerMap.end() && it->second == kv.second) {
                        ++match;
                    }
                }
                const std::size_t denom = (std::max)(a.size(), b.size());
                return (denom > 0) ? (static_cast<double>(match) / static_cast<double>(denom)) : 0.0;
            };

        struct PhysKey { int space; int ch; };
        struct PhysKeyHash {
            std::size_t operator()(const PhysKey& k) const noexcept {
                return (static_cast<std::size_t>(k.space) << 16) ^ static_cast<std::size_t>(k.ch);
            }
        };
        struct PhysKeyEq {
            bool operator()(const PhysKey& a, const PhysKey& b) const noexcept {
                return a.space == b.space && a.ch == b.ch;
            }
        };

        // group by physical channel
        std::unordered_map<PhysKey, std::vector<std::size_t>, PhysKeyHash, PhysKeyEq> byPhys;
        byPhys.reserve(activeServices.size());
        for (auto idx : activeServices) {
            const auto& s = services[idx];
            byPhys[{ s.space, s.channel }].push_back(idx);
        }

        // drop simulcast duplicates
        std::vector<std::size_t> filtered;
        filtered.reserve(activeServices.size());

        constexpr double kSimulcastThreshold = 0.95;
        constexpr std::size_t kMinCompareEvents = 10;

        std::vector<bool> dropped(services.size(), false);

        for (auto& g : byPhys) {
            auto& v = g.second;
            if (v.empty()) continue;
            if (v.size() == 1) {
                filtered.push_back(v[0]);
                continue;
            }

            std::size_t rep = v[0];
            for (auto idx : v) {
                if (ScoreService(services[idx]) > ScoreService(services[rep])) rep = idx;
            }

            auto repMap = BuildSigMap(rep);
            if (repMap.size() < kMinCompareEvents) {
                for (auto idx : v) filtered.push_back(idx);
                continue;
            }

            for (auto idx : v) {
                if (idx == rep) continue;

                auto m = BuildSigMap(idx);
                if (m.size() < kMinCompareEvents) continue;

                const double sim = Similarity(repMap, m);
                if (sim >= kSimulcastThreshold) {
                    dropped[idx] = true;
                }
            }

            filtered.push_back(rep);
            for (auto idx : v) {
                if (idx == rep) continue;
                if (!dropped[idx]) filtered.push_back(idx);
            }
        }

        std::sort(filtered.begin(), filtered.end());
        filtered.erase(std::unique(filtered.begin(), filtered.end()), filtered.end());
        activeServices.swap(filtered);

        // rebuild byPhys after drop
        std::unordered_map<PhysKey, std::vector<std::size_t>, PhysKeyHash, PhysKeyEq> byPhys2;
        byPhys2.reserve(activeServices.size());
        for (auto idx : activeServices) {
            const auto& s = services[idx];
            byPhys2[{ s.space, s.channel }].push_back(idx);
        }

        std::vector<bool> isMain(services.size(), false);
        std::vector<bool> isSub(services.size(), false);

        auto SortByServiceId = [&](std::vector<std::size_t>& v) {
            std::sort(v.begin(), v.end(), [&](std::size_t a, std::size_t b) {
                return services[a].serviceId < services[b].serviceId;
                });
            };

        // order: main then subs per physical channel
        std::vector<std::size_t> ordered;
        ordered.reserve(activeServices.size());

        std::vector<PhysKey> physKeys;
        physKeys.reserve(byPhys2.size());
        for (const auto& kv : byPhys2) physKeys.push_back(kv.first);
        std::sort(physKeys.begin(), physKeys.end(), [](const PhysKey& a, const PhysKey& b) {
            if (a.space != b.space) return a.space < b.space;
            return a.ch < b.ch;
            });

        for (const auto& pk : physKeys) {
            auto& v = byPhys2[pk];
            if (v.empty()) continue;

            std::size_t mainIdx = v[0];
            for (auto idx : v) {
                if (ScoreService(services[idx]) > ScoreService(services[mainIdx])) mainIdx = idx;
            }

            isMain[mainIdx] = true;

            std::vector<std::size_t> subs;
            subs.reserve(v.size());
            for (auto idx : v) {
                if (idx == mainIdx) continue;
                isSub[idx] = true;
                subs.push_back(idx);
            }
            SortByServiceId(subs);

            ordered.push_back(mainIdx);
            for (auto sidx : subs) ordered.push_back(sidx);
        }

        activeServices.swap(ordered);

        // ---------- 放送日ごとにグリッド/ジャンル別を生成 ----------
        if (!activeServices.empty()) {

            const int kSlotMinutes = GetGridSlotMinutesFromEnv(5);
            const int kTimeLabelIntervalMinutes = GetGridLabelIntervalMinutesFromEnv(30);

            const int labelEvery =
                (kTimeLabelIntervalMinutes <= 0)
                ? 1
                : (std::max)(1, kTimeLabelIntervalMinutes / kSlotMinutes);

            using namespace std::chrono;
            sys_minutes globalMin{};
            sys_minutes globalMax{};
            bool haveGlobal = false;

            std::size_t invalidTimeCount = 0;

            for (std::size_t col = 0; col < activeServices.size(); ++col) {
                const std::size_t svcIndex = activeServices[col];
                for (const auto* pv : eventsByService[svcIndex]) {
                    if (!pv) continue;
                    if (!pv->hasStart || !pv->hasDurMin || pv->durationMinutes <= 0) {
                        ++invalidTimeCount;
                        continue;
                    }
                    const auto s = ToSysMinutes(pv->start);
                    const auto e = ToSysMinutes(pv->end);
                    if (e <= s) {
                        ++invalidTimeCount;
                        continue;
                    }
                    if (!haveGlobal) {
                        globalMin = s;
                        globalMax = e;
                        haveGlobal = true;
                    }
                    else {
                        if (s < globalMin) globalMin = s;
                        if (e > globalMax) globalMax = e;
                    }
                }
            }

            if (invalidTimeCount > 0) {
                DebugLog(L"[EIT-HTML] start/duration 不整合イベント: %zu 件（グリッド対象外）", invalidTimeCount);
            }

            auto TruncateW = [](std::wstring_view s, std::size_t maxChars) -> std::wstring
                {
                    if (s.size() <= maxChars) return std::wstring(s);
                    std::wstring out(s.substr(0, maxChars));
                    out += L"…";
                    return out;
                };

            auto BuildSummary = [&](const EITTextEvent& ev) -> std::wstring
                {
                    std::wstring summaryW = ev.shortSummary;
                    if (!ev.extendedItems.empty()) {
                        if (!summaryW.empty()) summaryW += L"  ";
                        summaryW += L"…";
                        summaryW += ev.extendedItems.front().second;
                    }
                    return summaryW;
                };

            auto EventWeight = [](const EITTextEvent& ev) -> std::size_t
                {
                    std::size_t w = ev.shortTitle.size() + ev.shortSummary.size();
                    for (const auto& kv : ev.extendedItems) {
                        w += kv.first.size();
                        w += kv.second.size();
                    }
                    return w;
                };

            if (haveGlobal) {
                const sys_days firstDay = BroadcastDayKey(globalMin, broadcastBoundaryMinutes);
                const sys_days lastDay = BroadcastDayKey(globalMax, broadcastBoundaryMinutes);

                for (sys_days day = sys_days{ firstDay }; day <= sys_days{ lastDay }; day += days{ 1 }) {

                    const sys_minutes dayStart = sys_minutes{ day } + minutes{ broadcastBoundaryMinutes };
                    const sys_minutes dayEnd = dayStart + minutes{ 24 * 60 };

                    sys_minutes rangeStart{};
                    sys_minutes rangeEnd{};
                    bool haveGridRange = false;

                    for (std::size_t col = 0; col < activeServices.size(); ++col) {
                        const std::size_t svcIndex = activeServices[col];
                        for (const auto* pv : eventsByService[svcIndex]) {
                            if (!pv || !pv->hasStart || !pv->hasDurMin || pv->durationMinutes <= 0) continue;

                            const auto s = ToSysMinutes(pv->start);
                            const auto e = ToSysMinutes(pv->end);
                            if (e <= s) continue;

                            if (e <= dayStart || s >= dayEnd) continue;

                            const auto s2 = (s < dayStart) ? dayStart : s;
                            const auto e2 = (e > dayEnd) ? dayEnd : e;

                            if (!haveGridRange) {
                                rangeStart = s2;
                                rangeEnd = e2;
                                haveGridRange = true;
                            }
                            else {
                                if (s2 < rangeStart) rangeStart = s2;
                                if (e2 > rangeEnd)   rangeEnd = e2;
                            }
                        }
                    }

                    if (!haveGridRange) continue;

                    rangeStart = AlignDownToSlot(rangeStart, kSlotMinutes);
                    rangeEnd = AlignUpToSlot(rangeEnd, kSlotMinutes);
                    if (rangeEnd <= rangeStart) {
                        rangeEnd = rangeStart + minutes{ kSlotMinutes };
                    }

                    const auto totalMinutes = (rangeEnd - rangeStart).count();
                    const int rows = (totalMinutes > 0)
                        ? static_cast<int>((totalMinutes + (kSlotMinutes - 1)) / kSlotMinutes)
                        : 1;

                    struct CellStart {
                        int rowspan = 0;
                        const ViewItem* item = nullptr;
                    };

                    std::vector<std::vector<CellStart>> grid;
                    grid.resize(activeServices.size());
                    for (auto& g : grid) g.assign(rows, {});

                    std::size_t overlapDrops = 0;

                    for (std::size_t col = 0; col < activeServices.size(); ++col) {
                        const std::size_t svcIndex = activeServices[col];
                        int lastEndRow = 0;

                        for (const auto* pv : eventsByService[svcIndex]) {
                            if (!pv || !pv->hasStart || !pv->hasDurMin || pv->durationMinutes <= 0) continue;

                            const auto s = ToSysMinutes(pv->start);
                            const auto e = ToSysMinutes(pv->end);
                            if (e <= s) continue;

                            if (e <= dayStart || s >= dayEnd) continue;

                            const auto s2 = (s < dayStart) ? dayStart : s;
                            const auto e2 = (e > dayEnd) ? dayEnd : e;
                            if (e2 <= s2) continue;

                            const long long sOff = (s2 - rangeStart).count();
                            const long long eOff = (e2 - rangeStart).count();

                            int sRow = static_cast<int>(sOff / kSlotMinutes);
                            int eRow = static_cast<int>((eOff + (kSlotMinutes - 1)) / kSlotMinutes);

                            if (eRow <= 0 || sRow >= rows) continue;
                            if (sRow < 0) sRow = 0;
                            if (eRow > rows) eRow = rows;
                            if (eRow <= sRow) continue;

                            if (sRow < lastEndRow) {
                                ++overlapDrops;
                                sRow = lastEndRow;
                                if (eRow <= sRow) continue;
                            }

                            const int span = eRow - sRow;
                            if (span <= 0) continue;

                            auto& cell = grid[col][sRow];
                            if (cell.item) {
                                const auto* curEv = cell.item->src;
                                const auto* newEv = pv->src;
                                if (curEv && newEv && EventWeight(*newEv) > EventWeight(*curEv)) {
                                    cell.rowspan = span;
                                    cell.item = pv;
                                }
                            }
                            else {
                                cell.rowspan = span;
                                cell.item = pv;
                            }

                            lastEndRow = eRow;
                        }
                    }

                    if (overlapDrops > 0) {
                        DebugLog(L"[EIT-HTML] グリッド生成時に重複(オーバーラップ)を検出: %zu 件 (後続優先で整列)", overlapDrops);
                    }

                    const ProgramDateTime dayLabelDt = FromSysMinutes(dayStart);
                    const std::string dayStr = FormatYMD(dayLabelDt); // "YYYY/MM/DD"
                    const std::chrono::sys_days dayCal = std::chrono::floor<std::chrono::days>(dayStart);
                    const std::wstring dayLabelW = MakeLabelWithWeekday(dayStr, dayCal);

                    // past/upcoming dispatch
                    const bool isPast = (day < todayKey);
                    DayBuild& target = isPast ? buildPast : buildUpcoming;

                    if (target.keySet.insert(dayStr).second) {
                        target.infos.push_back(DayInfo{ dayStr, dayLabelW });
                    }

                    std::string& sec = target.sections;

                    sec += "<section class=\"day-section\" data-day=\"";
                    sec += dayStr;
                    sec += "\">\n";

                    sec += "<h3>";
                    sec += HtmlEscapeFromUtf8(dayLabelW);
                    sec += "</h3>\n";

                    // ----- grid view -----
                    sec += "<div class=\"grid-view\">\n";
                    sec += "<div class=\"epg-wrap\">\n";
                    sec += "<table class=\"epg\">\n";

                    // thead
                    sec += "<thead><tr>";
                    sec += "<th class=\"time-col\">";
                    sec += HtmlEscapeFromUtf8(L"時間");
                    sec += "</th>";

                    for (std::size_t col = 0; col < activeServices.size(); ++col) {
                        const auto svcIndex = activeServices[col];
                        const auto& svc = services[svcIndex];

                        const bool main = (svcIndex < isMain.size()) ? isMain[svcIndex] : true;
                        const char* colClass = main ? "col-main" : "col-sub";

                        sec += "<th class=\"svc ";
                        sec += colClass;
                        sec += "\">";
                        sec += HtmlEscapeFromUtf8(svc.serviceName);
                        sec += "</th>";
                    }
                    sec += "</tr></thead>\n";
                    sec += "<tbody>\n";

                    std::vector<int> skip(activeServices.size(), 0);

                    for (int r = 0; r < rows; ++r) {
                        const auto tp = rangeStart + std::chrono::minutes{ static_cast<long long>(kSlotMinutes) * r };
                        const ProgramDateTime dt = FromSysMinutes(tp);

                        const bool isHourTick = (dt.minute == 0);
                        const bool isLabelTick = (r == 0) || (r % labelEvery == 0);

                        sec += "<tr class=\"slot";
                        if (isHourTick) sec += " hour";
                        else if (isLabelTick) sec += " tick";
                        sec += "\">";

                        std::string timeLabel;
                        if (isLabelTick) {
                            timeLabel = FormatHM(dt);
                            if (r == 0) {
                                const auto ymd = FormatYMD(dt);
                                if (!ymd.empty()) timeLabel = ymd + " " + timeLabel;
                            }
                        }
                        else {
                            timeLabel = "&nbsp;";
                        }

                        sec += "<th class=\"time-col\">";
                        sec += timeLabel;
                        sec += "</th>";

                        for (std::size_t col = 0; col < activeServices.size(); ++col) {

                            if (skip[col] > 0) {
                                --skip[col];
                                continue;
                            }

                            const auto svcIndex = activeServices[col];
                            const bool main = (svcIndex < isMain.size()) ? isMain[svcIndex] : true;
                            const char* colClass = main ? "col-main" : "col-sub";

                            const auto& cell = grid[col][r];
                            if (cell.item && cell.rowspan > 0) {
                                skip[col] = cell.rowspan - 1;

                                const auto& vi = *cell.item;
                                const auto& ev = *vi.src;

                                std::wstring titleW = ev.shortTitle;
                                if (titleW.empty()) titleW = L"(no title)";

                                std::string startStr = vi.hasStart ? FormatHM(vi.start) : "";
                                std::string endStr = (vi.hasStart && vi.hasDurMin) ? FormatHM(vi.end) : "";

                                std::wstring summaryW = TruncateW(BuildSummary(ev), 140);

                                // genre major string
                                std::string gm = "unknown";
                                if (vi.genreMajor != 0xFF) gm = std::to_string(int(vi.genreMajor & 0x0F));

                                sec += "<td class=\"prog-cell ";
                                sec += colClass;
                                sec += "\" data-gm=\"";
                                sec += gm;
                                sec += "\" rowspan=\"";
                                sec += std::to_string(cell.rowspan);
                                sec += "\" title=\"";
                                sec += HtmlEscapeAttrFromUtf8(titleW);
                                sec += "\">";

                                sec += "<span class=\"eid\">";
                                sec += std::to_string(ev.eventId);
                                sec += "</span>";

                                sec += "<div class=\"t\">";
                                sec += HtmlEscapeFromUtf8(titleW);
                                sec += "</div>";

                                // time: start~end
                                sec += "<div class=\"tm\">";
                                sec += startStr;
                                sec += kWaveDashHtmlEntity;
                                sec += (endStr.empty() ? "?" : endStr);
                                if (vi.hasDurMin) {
                                    sec += " (";
                                    sec += std::to_string(vi.durationMinutes);
                                    sec += "m)";
                                }
                                sec += "</div>";

                                if (!summaryW.empty()) {
                                    sec += "<div class=\"s\">";
                                    sec += HtmlEscapeFromUtf8(summaryW);
                                    sec += "</div>";
                                }

                                sec += "</td>";
                            }
                            else {
                                sec += "<td class=\"empty ";
                                sec += colClass;
                                sec += "\"></td>";
                            }
                        }

                        sec += "</tr>\n";
                    }

                    sec += "</tbody></table>\n</div>\n";
                    sec += "</div>\n";

                    // ----- genre view (majorでグループ化) -----
                    {
                        std::vector<const ViewItem*> dayItems;
                        dayItems.reserve(2048);

                        for (std::size_t col = 0; col < activeServices.size(); ++col) {
                            const std::size_t svcIndex = activeServices[col];
                            for (const auto* pv : eventsByService[svcIndex]) {
                                if (!pv || !pv->hasStart || !pv->hasDurMin || pv->durationMinutes <= 0) continue;
                                const auto s = ToSysMinutes(pv->start);
                                const auto e = ToSysMinutes(pv->end);
                                if (e <= s) continue;
                                if (e <= dayStart || s >= dayEnd) continue;
                                dayItems.push_back(pv);
                            }
                        }

                        auto MajorKeyStr = [](const ViewItem* v) -> std::string {
                            if (!v) return "unknown";
                            if (v->genreMajor == 0xFF) return "unknown";
                            return std::to_string(int(v->genreMajor & 0x0F));
                            };

                        auto MajorLabelW = [](const ViewItem* v) -> std::wstring {
                            if (!v || v->genreMajor == 0xFF) return L"不明";
                            return genre::MajorLabel(static_cast<uint8_t>(v->genreMajor & 0x0F));
                            };

                        std::sort(dayItems.begin(), dayItems.end(),
                            [&](const ViewItem* a, const ViewItem* b) {
                                const std::wstring ga = MajorLabelW(a);
                                const std::wstring gb = MajorLabelW(b);
                                if (ga != gb) return ga < gb;

                                const auto ta = ToSysMinutes(a->start);
                                const auto tb = ToSysMinutes(b->start);
                                if (ta != tb) return ta < tb;

                                int sa = (a ? a->serviceIndex : -1);
                                int sb = (b ? b->serviceIndex : -1);
                                if (sa != sb) return sa < sb;

                                const auto* ea = a ? a->src : nullptr;
                                const auto* eb = b ? b->src : nullptr;
                                const uint16_t ida = ea ? ea->eventId : 0;
                                const uint16_t idb = eb ? eb->eventId : 0;
                                return ida < idb;
                            });

                        sec += "<div class=\"genre-view\">\n";
                        sec += "<div class=\"table-wrap\">\n";
                        sec += "<table class=\"glist\">\n";
                        sec += "<thead><tr>";
                        sec += "<th class=\"time\">" + HtmlEscapeFromUtf8(L"時間") + "</th>";
                        sec += "<th class=\"svc\">" + HtmlEscapeFromUtf8(L"局") + "</th>";
                        sec += "<th class=\"genre\">" + HtmlEscapeFromUtf8(L"ジャンル") + "</th>";
                        sec += "<th>" + HtmlEscapeFromUtf8(L"番組名") + "</th>";
                        sec += "<th>" + HtmlEscapeFromUtf8(L"概要") + "</th>";
                        sec += "</tr></thead>\n<tbody>\n";

                        std::string curMajor = "";
                        std::unordered_map<std::string, std::size_t> cnt;
                        cnt.reserve(32);
                        for (auto* v : dayItems) cnt[MajorKeyStr(v)] += 1;

                        auto FlushMajorHeader = [&](const std::string& gm, const std::wstring& labelW) {
                            sec += "<tr class=\"genre-sep\" data-gm=\"";
                            sec += gm;
                            sec += "\"><th colspan=\"5\">";
                            sec += HtmlEscapeFromUtf8(labelW);
                            sec += " (";
                            sec += std::to_string(cnt[gm]);
                            sec += ")";
                            sec += "</th></tr>\n";
                            };

                        for (auto* v : dayItems) {
                            if (!v || !v->src) continue;
                            const EITTextEvent& ev = *v->src;

                            const std::string gm = MajorKeyStr(v);
                            const std::wstring glabW = MajorLabelW(v);

                            if (curMajor != gm) {
                                FlushMajorHeader(gm, glabW);
                                curMajor = gm;
                            }

                            const std::string startStr = v->hasStart ? FormatHM(v->start) : "";
                            const std::string endStr = (v->hasStart && v->hasDurMin) ? FormatHM(v->end) : "";

                            std::string timeRange;
                            if (!startStr.empty()) {
                                timeRange = startStr;
                                timeRange += kWaveDashHtmlEntity;
                                timeRange += (endStr.empty() ? "?" : endStr);
                            }

                            std::wstring svcName = L"(unknown)";
                            if (v->serviceIndex >= 0 && static_cast<std::size_t>(v->serviceIndex) < services.size()) {
                                svcName = services[static_cast<std::size_t>(v->serviceIndex)].serviceName;
                            }

                            std::wstring titleW = ev.shortTitle;
                            if (titleW.empty()) titleW = L"(no title)";

                            std::wstring summaryW = TruncateW(BuildSummary(ev), 220);

                            std::wstring genreCell = glabW;
                            if (!v->genreLabel.empty() && v->genreLabel != glabW) {
                                genreCell += L" (" + v->genreLabel + L")";
                            }

                            sec += "<tr class=\"gitem\" data-gm=\"";
                            sec += gm;
                            sec += "\">";
                            sec += "<td class=\"time\">" + timeRange + "</td>";
                            sec += "<td class=\"svc\">" + HtmlEscapeFromUtf8(svcName) + "</td>";
                            sec += "<td class=\"genre\">" + HtmlEscapeFromUtf8(genreCell) + "</td>";
                            sec += "<td class=\"title\"";
                            AppendProgramSwitchDataAttrs(sec, *v, svcName, titleW, summaryW, genreCell);
                            sec += ">";
                            sec += HtmlEscapeFromUtf8(titleW);
                            sec += "</td>";

                            sec += "<td>";
                            sec += HtmlEscapeFromUtf8(summaryW);
                            sec += "</td>";
                            sec += "</tr>\n";
                        }

                        sec += "</tbody></table>\n</div>\n</div>\n";
                    }

                    sec += "</section>\n";
                }
            }
            else {
                DebugLog(L"[EIT-HTML] グリッド生成に必要な start/duration が揃ったイベントがありませんでした。");
            }
        }
        else {
            DebugLog(L"[EIT-HTML] サービスリストはあるが、イベントが 0 件でした。");
        }

        // unmatched -> split
        SplitByBroadcastDayKey<const ViewItem*>(
            others,
            othersPast,
            othersUpcoming,
            todayKey,
            broadcastBoundaryMinutes,
            [&](const ViewItem* p, sys_minutes& outTp) -> bool {
                if (!p || !p->hasStart) return false;
                outTp = ToSysMinutes(p->start);
                return p->start.year > 0;
            });

    }
    else {
        // ---- サービスリスト無し: 全イベントを "others" として扱い、table表示で split ----
        // ここでは grid は作らず、日付区切りの list テーブルを past/upcoming へ
        std::vector<const ViewItem*> all = others;

        SplitByBroadcastDayKey<const ViewItem*>(
            all,
            othersPast,
            othersUpcoming,
            todayKey,
            broadcastBoundaryMinutes,
            [&](const ViewItem* p, sys_minutes& outTp) -> bool {
                if (!p || !p->hasStart) return false;
                outTp = ToSysMinutes(p->start);
                return p->start.year > 0;
            });

        auto BuildSimpleListSections = [&](const std::vector<const ViewItem*>& items, DayBuild& outBuild)
            {
                // group by date label
                std::string sec;
                sec.reserve(256 * 1024);

                if (!items.empty()) {
                    sec += "<h2>";
                    sec += HtmlEscapeFromUtf8(L"全イベント");
                    sec += "</h2>\n";

                    EmitTableHeader(sec);

                    std::string curDate;
                    bool first = true;

                    for (const auto* pv : items) {
                        if (!pv) continue;
                        const auto& item = *pv;

                        const std::string d = item.hasStart ? FormatYMD(item.start) : std::string();
                        std::wstring dLabelW = L"(no date)";
                        if (!d.empty()) {
                            const sys_minutes stp = ToSysMinutes(item.start);
                            const auto dayCal = std::chrono::floor<std::chrono::days>(stp);
                            dLabelW = MakeLabelWithWeekday(d, dayCal);

                            if (outBuild.keySet.insert(d).second) {
                                outBuild.infos.push_back(DayInfo{ d, dLabelW });
                            }
                        }

                        if (first || d != curDate) {
                            EmitDateSepRow(sec, d, dLabelW);
                            curDate = d;
                            first = false;
                        }
                        EmitOneRow(item, sec);
                    }

                    EmitTableFooter(sec);
                }

                outBuild.sections += sec;
            };

        BuildSimpleListSections(othersUpcoming, buildUpcoming);
        BuildSimpleListSections(othersPast, buildPast);

        // in no-service-list mode, do not duplicate "others" again below
        othersUpcoming.clear();
        othersPast.clear();
    }

    // ---------------- ドキュメント生成（2本） ----------------
    // link filenames (relative)
    const std::wstring beforeFileNameW = std::filesystem::path(beforePath).filename().wstring();
    const std::wstring upcomingFileNameW = std::filesystem::path(htmlOutputPath).filename().wstring();

    const bool hasPastDoc = (!buildPast.sections.empty()) || (!othersPast.empty());
    const bool hasUpcomingDoc = (!buildUpcoming.sections.empty()) || (!othersUpcoming.empty());

    // upcoming doc
    std::string htmlUpcoming;
    if (hasUpcomingDoc) {
        htmlUpcoming = BuildDocument(
            "upcoming",
            L"EIT 番組表（当日以降）",
            buildUpcoming,
            hasPastDoc ? L"過去(before)を開く" : L"",
            hasPastDoc ? beforeFileNameW : L"",
            othersUpcoming,
            true);
    }

    // past doc
    std::string htmlPast;
    if (hasPastDoc) {
        htmlPast = BuildDocument(
            "before",
            L"EIT 番組表（before/過去）",
            buildPast,
            hasUpcomingDoc ? L"当日以降を開く" : L"",
            hasUpcomingDoc ? upcomingFileNameW : L"",
            othersPast,
            false);
    }

    // write
    bool wroteAny = false;

    if (hasUpcomingDoc) {
        if (!WriteUtf8File(htmlOutputPath, htmlUpcoming)) {
            DebugLog(L"[EIT-HTML] HTML ファイルの書き出しに失敗しました: %ls", htmlOutputPath.c_str());
            return false;
        }
        wroteAny = true;
        EIT_HTML_LOG(L"[EIT-HTML] HTML 出力完了(当日以降): %ls", htmlOutputPath.c_str());
    }
    else {
        DebugLog(L"[EIT-HTML] 当日以降(放送日 기준)の出力対象がありませんでした。");
    }

    if (hasPastDoc) {
        if (!WriteUtf8File(beforePath, htmlPast)) {
            DebugLog(L"[EIT-HTML] HTML(before) ファイルの書き出しに失敗しました: %ls", beforePath.c_str());
            return false;
        }
        wroteAny = true;
        EIT_HTML_LOG(L"[EIT-HTML] HTML 出力完了(before/過去): %ls", beforePath.c_str());
    }
    else {
        DebugLog(L"[EIT-HTML] before(過去)の出力対象がありませんでした。");
    }

    return wroteAny;
}

// 既存の簡易テスト
void RunEITHtmlProgramTableTest(const std::wstring& epgDumpPath)
{
    const std::wstring outPath = L"EIT_ProgramTable_simple.html";
    (void)GenerateEITHtmlProgramTable(epgDumpPath, outPath);
}

// サービスリスト付きテスト
void RunEITHtmlProgramTableTest_WithServiceList(
    const std::wstring& epgDumpPath,
    const std::wstring& channelListPath)
{
    const std::wstring outPath = L"EIT_ProgramTable_withService.html";
    (void)GenerateEITHtmlProgramTable(epgDumpPath, channelListPath, outPath);
}

// ---------------- デバッグ: サービス＋時間順ソートの確認 ----------------
bool DumpEpgSortedByServiceAndTime(
    const std::wstring& epgDumpPath,
    const std::wstring& channelListPath)
{
    std::vector<EITTextEvent> events;
    events.reserve(2048);

    const bool ok = ForEachEITTextEventInDump(epgDumpPath, [&](const EITTextEvent& ev) {
        events.push_back(ev);
        });

    if (!ok) {
        DebugLog(L"[EIT-SORT] EPG 読み込み失敗: %ls", epgDumpPath.c_str());
        return false;
    }
    if (events.empty()) {
        DebugLog(L"[EIT-SORT] イベント0件: %ls", epgDumpPath.c_str());
        return false;
    }

    std::vector<ServiceEntry> services;
    if (!LoadChannelList(channelListPath, services)) {
        DebugLog(L"[EIT-SORT] channellist 読み込み失敗: %ls", channelListPath.c_str());
        return false;
    }

    std::unordered_map<uint64_t, const ServiceEntry*> svcMap;
    svcMap.reserve(services.size() * 2);
    for (const auto& s : services) {
        svcMap[MakeServiceKey(s.serviceId, s.tsid, s.onid)] = &s;
    }

    struct Row {
        const ServiceEntry* svc = nullptr;
        ProgramDateTime start{};
        bool hasStart = false;
        int durMin = 0;
        bool hasDur = false;
        const EITTextEvent* ev = nullptr;
    };

    std::vector<Row> rows;
    rows.reserve(events.size());

    std::size_t noSvc = 0;
    std::size_t badTime = 0;

    for (const auto& ev : events) {
        Row r;
        r.ev = &ev;

        auto it = svcMap.find(MakeServiceKey(ev.serviceId, ev.tsid, ev.onid));
        if (it != svcMap.end()) r.svc = it->second;
        else ++noSvc;

        ProgramDateTime dt{};
        if (DecodeStartTimeRawHex(ev.startTimeRawHex, dt)) {
            r.start = dt;
            r.hasStart = true;

            int m = 0;
            if (ParseDurationToMinutes(ev.durationHHMMSS, m) && m > 0) {
                r.durMin = m;
                r.hasDur = true;
            }
        }
        else {
            ++badTime;
        }

        rows.push_back(r);
    }

    std::sort(rows.begin(), rows.end(),
        [&](const Row& a, const Row& b) {
            auto svcLess = [&](const ServiceEntry* x, const ServiceEntry* y) {
                if (x == nullptr && y != nullptr) return false;
                if (x != nullptr && y == nullptr) return true;
                if (x == nullptr && y == nullptr) return false;
                if (x->space != y->space) return x->space < y->space;
                if (x->channel != y->channel) return x->channel < y->channel;
                if (x->serviceId != y->serviceId) return x->serviceId < y->serviceId;
                return x->serviceName < y->serviceName;
                };

            if (a.svc != b.svc) return svcLess(a.svc, b.svc);

            if (a.hasStart != b.hasStart) return a.hasStart && !b.hasStart;
            if (!a.hasStart && !b.hasStart) return false;
            return a.start < b.start;
        });

    DebugLog(L"[EIT-SORT] rows=%zu, noSvc=%zu, badTime=%zu", rows.size(), noSvc, badTime);

    const ServiceEntry* cur = nullptr;
    std::size_t printed = 0;
    constexpr std::size_t kMaxLines = 20000;

    auto PrintHeader = [&](const ServiceEntry* s) {
        if (!s) {
            DebugLog(L"[EIT-SORT] ---- (UNKNOWN SERVICE) ----");
            return;
        }
        DebugLog(L"[EIT-SORT] ---- %ls (space=%d ch=%d sid=%u tsid=%u onid=%u) ----",
            s->serviceName.c_str(), s->space, s->channel,
            (unsigned)s->serviceId, (unsigned)s->tsid, (unsigned)s->onid);
        };

    for (const auto& r : rows) {
        if (printed >= kMaxLines) {
            DebugLog(L"[EIT-SORT] ... truncated (kMaxLines=%zu)", kMaxLines);
            break;
        }

        if (r.svc != cur) {
            cur = r.svc;
            PrintHeader(cur);
        }

        std::wstring timeW = L"(no time)";
        if (r.hasStart) {
            std::wstringstream ss;
            ss << std::setfill(L'0')
                << std::setw(2) << r.start.hour << L":"
                << std::setw(2) << r.start.minute;

            if (r.hasDur && r.durMin > 0) {
                const ProgramDateTime endDt = AddMinutes(r.start, r.durMin);
                ss << L"\u301C"  // "～" をUCNで（ソース文字コード依存を避ける）
                    << std::setw(2) << endDt.hour << L":"
                    << std::setw(2) << endDt.minute;
            }
            timeW = ss.str();
        }

        const std::wstring titleW = r.ev ? r.ev->shortTitle : L"";
        DebugLog(L"[EIT-SORT] %ls  %4dm  EID=%u  %ls",
            timeW.c_str(),
            r.hasDur ? r.durMin : -1,
            r.ev ? (unsigned)r.ev->eventId : 0u,
            titleW.c_str());

        ++printed;
    }

    return true;
}
