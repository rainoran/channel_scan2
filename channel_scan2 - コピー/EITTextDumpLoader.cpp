#include "EITTextDumpLoader.hpp"
#include "DebugManager.h"   // DebugLog / DebugLogFormat 

#include <fstream>
#include <sstream>
#include <cwctype>
#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

// ★ テスト用ログマクロ（必ず DebugLog 経由）
#if ENABLE_EIT_TEXT_DUMP_DEBUG
#define EIT_DUMP_LOG(...) DebugLog(__VA_ARGS__)
#else
#define EIT_DUMP_LOG(...) ((void)0)
#endif

namespace
{
    // 先頭・末尾の空白を削る
    std::wstring Trim(const std::wstring& s)
    {
        const auto is_space = [](wchar_t ch) {
            return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
            };

        std::size_t start = 0;
        while (start < s.size() && is_space(s[start])) ++start;

        if (start == s.size()) return L"";

        std::size_t end = s.size();
        while (end > start && is_space(s[end - 1])) --end;

        return s.substr(start, end - start);
    }

    // 先頭だけ空白を削る
    std::wstring TrimLeft(const std::wstring& s)
    {
        const auto is_space = [](wchar_t ch) {
            return ch == L' ' || ch == L'\t';
            };
        std::size_t start = 0;
        while (start < s.size() && is_space(s[start])) ++start;
        return s.substr(start);
    }

    // "Label: 1234" 形式の 10進整数を読む
    template <class T>
    bool ParseUIntFieldDec(const std::wstring& line,
        const std::wstring& label,
        T& out,
        bool* parsedFlag = nullptr)
    {
        auto pos = line.find(label);
        if (pos == std::wstring::npos) return false;
        pos += label.size();

        while (pos < line.size() && line[pos] == L' ') ++pos;

        std::wstring num;
        while (pos < line.size() && iswdigit(line[pos])) {
            num.push_back(line[pos]);
            ++pos;
        }
        if (num.empty()) return false;

        try {
            unsigned long v = std::stoul(num, nullptr, 10);
            out = static_cast<T>(v);
            if (parsedFlag) *parsedFlag = true;
            return true;
        }
        catch (...) {
            return false;
        }
    }

    // "Label: 0x4E" 形式の 16進整数を読む
    template <class T>
    bool ParseUIntFieldHex(const std::wstring& line,
        const std::wstring& label,
        T& out,
        bool* parsedFlag = nullptr)
    {
        auto pos = line.find(label);
        if (pos == std::wstring::npos) return false;
        pos += label.size();

        while (pos < line.size() && line[pos] == L' ') ++pos;

        if (pos + 2 < line.size() &&
            line[pos] == L'0' &&
            (line[pos + 1] == L'x' || line[pos + 1] == L'X'))
        {
            pos += 2;
        }

        std::wstring num;
        while (pos < line.size() && iswxdigit(line[pos])) {
            num.push_back(line[pos]);
            ++pos;
        }
        if (num.empty()) return false;

        try {
            unsigned long v = std::stoul(num, nullptr, 16);
            out = static_cast<T>(v);
            if (parsedFlag) *parsedFlag = true;
            return true;
        }
        catch (...) {
            return false;
        }
    }

    // "Duration(HH:MM:SS): 00:29:00, RunningStatus: 0, FreeCA: 0"
    void ParseDurationLine(const std::wstring& line, EITTextEvent& ev)
    {
        const std::wstring durLabel = L"Duration(HH:MM:SS):";
        auto pos = line.find(durLabel);
        if (pos != std::wstring::npos) {
            pos += durLabel.size();
            while (pos < line.size() && line[pos] == L' ') ++pos;

            std::wstring timeStr;
            while (pos < line.size() && line[pos] != L',' && line[pos] != L'\0') {
                timeStr.push_back(line[pos]);
                ++pos;
            }
            timeStr = Trim(timeStr);
            if (!timeStr.empty()) {
                ev.durationHHMMSS = timeStr;
                ev.parsed.duration = true;
            }
        }

        if (ParseUIntFieldDec(line, L"RunningStatus:", ev.runningStatus)) {
            ev.parsed.runningStatus = true;
        }

        unsigned tmpFreeCa = 0;
        if (ParseUIntFieldDec(line, L"FreeCA:", tmpFreeCa)) {
            ev.freeCA = (tmpFreeCa != 0);
            ev.parsed.freeCA = true;
        }
    }

    // "Version: 2, Section: 0/1"
    void ParseVersionSectionLine(const std::wstring& line, EITTextEvent& ev)
    {
        bool ok = false;
        if (ParseUIntFieldDec(line, L"Version:", ev.version)) {
            ok = true;
        }

        const std::wstring secLabel = L"Section:";
        auto pos = line.find(secLabel);
        if (pos != std::wstring::npos) {
            pos += secLabel.size();

            while (pos < line.size() && line[pos] == L' ') ++pos;

            // first (sectionNumber)
            std::wstring num1;
            while (pos < line.size() && iswdigit(line[pos])) {
                num1.push_back(line[pos]);
                ++pos;
            }
            if (!num1.empty()) {
                try {
                    ev.sectionNumber = static_cast<uint8_t>(
                        std::stoul(num1, nullptr, 10));
                    ok = true;
                }
                catch (...) {
                }
            }

            if (pos < line.size() && line[pos] == L'/') {
                ++pos;
                std::wstring num2;
                while (pos < line.size() && iswdigit(line[pos])) {
                    num2.push_back(line[pos]);
                    ++pos;
                }
                if (!num2.empty()) {
                    try {
                        ev.lastSectionNumber = static_cast<uint8_t>(
                            std::stoul(num2, nullptr, 10));
                        ok = true;
                    }
                    catch (...) {
                    }
                }
            }
        }

        if (ok) {
            ev.parsed.versionSection = true;
        }
    }

    // "[短形式タイトル] xxx" / "[短形式概要] xxx"
    void ParseBracketLine(const std::wstring& line,
        const std::wstring& tag,
        std::wstring& dst,
        bool& parsedFlag)
    {
        if (line.rfind(tag, 0) != 0) return; // 先頭一致でなければ無視

        auto pos = line.find(L']');
        if (pos == std::wstring::npos) return;
        ++pos;

        std::wstring rest = line.substr(pos);
        rest = Trim(rest);
        if (!rest.empty()) {
            dst = std::move(rest);
            parsedFlag = true;
        }
    }

    // "[ジャンル] primary=0x2, secondary=0x4 (user1=15, user2=15)"
    void ParseGenreLine(const std::wstring& line, EITTextEvent& ev)
    {
        bool any = false;
        if (ParseUIntFieldHex(line, L"primary=", ev.contentNibbleLevel1)) any = true;
        if (ParseUIntFieldHex(line, L"secondary=", ev.contentNibbleLevel2)) any = true;
        if (ParseUIntFieldDec(line, L"user1=", ev.userNibble1))         any = true;
        if (ParseUIntFieldDec(line, L"user2=", ev.userNibble2))         any = true;

        if (any) {
            ev.parsed.genre = true;
        }
    }

    // "(0) ラベル: テキスト" を extendedItems に追加
    void ParseExtendedItemLine(const std::wstring& line, EITTextEvent& ev)
    {
        if (line.empty() || line[0] != L'(') return;

        auto closePos = line.find(L')');
        if (closePos == std::wstring::npos) return;

        std::size_t pos = closePos + 1;
        // ") " を飛ばす
        if (pos < line.size() && line[pos] == L' ') ++pos;

        auto colonPos = line.find(L':', pos);
        if (colonPos == std::wstring::npos) {
            // ラベルなしで全部本文扱い
            std::wstring textOnly = Trim(line.substr(pos));
            if (!textOnly.empty()) {
                ev.extendedItems.emplace_back(L"", textOnly);
                ev.parsed.extendedItems = true;
            }
            return;
        }

        std::wstring label = Trim(line.substr(pos, colonPos - pos));
        std::wstring text = Trim(line.substr(colonPos + 1));

        if (!label.empty() || !text.empty()) {
            ev.extendedItems.emplace_back(std::move(label), std::move(text));
            ev.parsed.extendedItems = true;
        }
    }

    // UTF-8 テキストファイルを丸ごと読み込んで UTF-16 (std::wstring) に変換
    bool LoadUtf8FileToWString(const std::wstring& filePath, std::wstring& outText)
    {
        std::ifstream ifs(filePath, std::ios::binary);
        if (!ifs) {
            DebugLog(L"[EIT-DUMP] ファイルオープン失敗: %ls", filePath.c_str());
            return false;
        }

        std::string bytes;
        ifs.seekg(0, std::ios::end);
        const std::streamoff len = ifs.tellg();
        if (len < 0) {
            DebugLog(L"[EIT-DUMP] tellg 失敗: %ls", filePath.c_str());
            return false;
        }
        bytes.resize(static_cast<std::size_t>(len));
        ifs.seekg(0, std::ios::beg);
        if (len > 0) {
            ifs.read(bytes.data(), len);
        }

        if (bytes.empty()) {
            outText.clear();
            return true;
        }

        // UTF-8 → UTF-16 (wchar_t)
        int wideLen = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            bytes.data(),
            static_cast<int>(bytes.size()),
            nullptr,
            0);
        if (wideLen <= 0) {
            DebugLog(L"[EIT-DUMP] UTF-8 -> UTF-16 変換サイズ取得に失敗しました: %ls", filePath.c_str());
            return false;
        }

        outText.resize(wideLen);
        int written = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            bytes.data(),
            static_cast<int>(bytes.size()),
            outText.data(),
            wideLen);
        if (written <= 0) {
            DebugLog(L"[EIT-DUMP] UTF-8 -> UTF-16 変換に失敗しました: %ls", filePath.c_str());
            outText.clear();
            return false;
        }

        return true;
    }

} // namespace

bool ForEachEITTextEventInDump(
    const std::wstring& filePath,
    const EITEventHandler& handler)
{
    if (!handler) {
        DebugLog(L"[EIT-DUMP] handler が無効です。");
        return false;
    }

    std::wstring text;
    if (!LoadUtf8FileToWString(filePath, text)) {
        // LoadUtf8FileToWString 内で DebugLog 済み
        return false;
    }

    std::wistringstream iss(text);
    std::wstring line;

    EITTextEvent cur;
    bool inBlock = false;
    bool inExtendedItems = false;

    while (std::getline(iss, line)) {
        std::wstring raw = TrimLeft(line);

        // 空行 → ブロック終了扱い
        if (raw.empty()) {
            if (inBlock) {
                handler(cur);
                cur = EITTextEvent{};
            }
            inBlock = false;
            inExtendedItems = false;
            continue;
        }

        // ヘッダー行 "# EIT Dump ..." は無視
        if (raw.rfind(L"# EIT Dump", 0) == 0) {
            continue;
        }

        // "TableID: ..." で新しいイベント開始
        if (raw.rfind(L"TableID:", 0) == 0) {
            if (inBlock) {
                // 前のイベントを確定
                handler(cur);
                cur = EITTextEvent{};
            }
            inBlock = true;
            inExtendedItems = false;

            (void)ParseUIntFieldHex(raw, L"TableID:", cur.tableId, &cur.parsed.headerTableId);
            (void)ParseUIntFieldDec(raw, L"ServiceID:", cur.serviceId, &cur.parsed.headerServiceId);
            (void)ParseUIntFieldDec(raw, L"EventID:", cur.eventId, &cur.parsed.headerEventId);
            (void)ParseUIntFieldDec(raw, L"TSID:", cur.tsid, &cur.parsed.headerTsid);
            (void)ParseUIntFieldDec(raw, L"ONID:", cur.onid, &cur.parsed.headerOnid);
            continue;
        }

        // まだイベント開始していない行はスキップ
        if (!inBlock)
            continue;

        // Version / Section
        if (raw.rfind(L"Version:", 0) == 0) {
            ParseVersionSectionLine(raw, cur);
            continue;
        }

        // StartTime(raw MJD+BCD40bit): 0xEE51...
        if (raw.rfind(L"StartTime(raw MJD+BCD40bit):", 0) == 0) {
            auto pos = raw.find(L':');
            if (pos != std::wstring::npos) {
                std::wstring rest = Trim(raw.substr(pos + 1));
                if (!rest.empty()) {
                    cur.startTimeRawHex = rest; // "0xEE51..."
                    cur.parsed.startTime = true;
                }
            }
            continue;
        }

        // Duration / RunningStatus / FreeCA
        if (raw.rfind(L"Duration(HH:MM:SS):", 0) == 0) {
            ParseDurationLine(raw, cur);
            continue;
        }

        // 短形式タイトル
        if (raw.rfind(L"[短形式タイトル]", 0) == 0) {
            ParseBracketLine(raw, L"[短形式タイトル]", cur.shortTitle, cur.parsed.shortTitle);
            continue;
        }

        // 短形式概要
        if (raw.rfind(L"[短形式概要]", 0) == 0) {
            ParseBracketLine(raw, L"[短形式概要]", cur.shortSummary, cur.parsed.shortSummary);
            continue;
        }

        // 拡張items 開始
        if (raw.rfind(L"[拡張items]", 0) == 0) {
            inExtendedItems = true;
            continue;
        }

        // ジャンル
        if (raw.rfind(L"[ジャンル]", 0) == 0) {
            ParseGenreLine(raw, cur);
            continue;
        }

        // 拡張items 本文
        if (inExtendedItems) {
            ParseExtendedItemLine(raw, cur);
            continue;
        }

        // それ以外の行は今のところ無視
    }

    // EOF 時に最後のブロックが残っていたら確定
    if (inBlock) {
        handler(cur);
    }

    return true;
}

void RunEITTextDumpFieldTest(const std::wstring& filePath)
{
    EIT_DUMP_LOG(L"[EIT-DUMP] フィールドテスト開始: %ls", filePath.c_str());

    std::size_t count = 0;

    auto fieldMark = [](bool ok) -> const wchar_t* {
        return ok ? L"OK" : L"--";
        };

    ForEachEITTextEventInDump(
        filePath,
        [&](const EITTextEvent& ev)
        {
            ++count;
            EIT_DUMP_LOG(L"[EIT-DUMP][EVENT] #%zu", count);

            // ヘッダ
            EIT_DUMP_LOG(L"  [HEADER] TID=%s SID=%s EID=%s TSID=%s ONID=%s",
                fieldMark(ev.parsed.headerTableId),
                fieldMark(ev.parsed.headerServiceId),
                fieldMark(ev.parsed.headerEventId),
                fieldMark(ev.parsed.headerTsid),
                fieldMark(ev.parsed.headerOnid));
            EIT_DUMP_LOG(L"           TableID=0x%02X SID=%u EID=%u TSID=%u ONID=%u",
                ev.tableId,
                ev.serviceId,
                ev.eventId,
                ev.tsid,
                ev.onid);

            // セクション
            EIT_DUMP_LOG(L"  [SECT]   Version/Section=%s  (v=%u sec=%u/%u)",
                fieldMark(ev.parsed.versionSection),
                ev.version,
                ev.sectionNumber,
                ev.lastSectionNumber);

            // 時刻・時間
            EIT_DUMP_LOG(L"  [TIME]   Start=%s  Dur=%s  RS=%s  FreeCA=%s",
                fieldMark(ev.parsed.startTime),
                fieldMark(ev.parsed.duration),
                fieldMark(ev.parsed.runningStatus),
                fieldMark(ev.parsed.freeCA));
            EIT_DUMP_LOG(L"           StartTime(raw)=%ls  Dur=%ls  RS=%u FreeCA=%d",
                ev.startTimeRawHex.c_str(),
                ev.durationHHMMSS.c_str(),
                ev.runningStatus,
                ev.freeCA ? 1 : 0);

            // タイトル / 概要 / 拡張
            EIT_DUMP_LOG(L"  [TEXT]   ShortTitle=%s  ShortSummary=%s  ExtItems=%s",
                fieldMark(ev.parsed.shortTitle),
                fieldMark(ev.parsed.shortSummary),
                fieldMark(ev.parsed.extendedItems));
            EIT_DUMP_LOG(L"           Title  : %ls",
                ev.shortTitle.c_str());
            EIT_DUMP_LOG(L"           Summary: %ls",
                ev.shortSummary.c_str());

            for (std::size_t i = 0; i < ev.extendedItems.size(); ++i) {
                const auto& it = ev.extendedItems[i];
                EIT_DUMP_LOG(L"           Ext[%zu] \"%ls\": %ls",
                    i,
                    it.first.c_str(),
                    it.second.c_str());
            }

            // ジャンル
            EIT_DUMP_LOG(L"  [GENRE]  %s  primary=0x%X secondary=0x%X (user1=%u user2=%u)",
                fieldMark(ev.parsed.genre),
                ev.contentNibbleLevel1,
                ev.contentNibbleLevel2,
                ev.userNibble1,
                ev.userNibble2);
        });

    EIT_DUMP_LOG(L"[EIT-DUMP] フィールドテスト終了: %zu events", count);
}
