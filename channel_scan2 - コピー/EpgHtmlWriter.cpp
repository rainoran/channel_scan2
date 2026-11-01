#include "EpgHtmlWriter.hpp"
#include "EncodingConfig.hpp"
#include "DebugManager.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <locale>
#include <Windows.h>

// ★ イベント詳細デバッグのオン/オフスイッチ
//   0: 無効（通常運用）
//   1: 有効（対象イベントを詳細ログ出力）
#ifndef EPG_HTML_DEBUG_VERBOSE
#define EPG_HTML_DEBUG_VERBOSE 1
#endif

// --- ローカルヘルパ ---
namespace {

    // wstring → 任意コードページのマルチバイト文字列
    inline std::string WToMultiByte(const std::wstring& ws, UINT codePage)
    {
        if (ws.empty()) return {};

        int len = ::WideCharToMultiByte(
            codePage,
            0,
            ws.c_str(),
            static_cast<int>(ws.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (len <= 0) return {};

        std::string out(len, '\0');
        ::WideCharToMultiByte(
            codePage,
            0,
            ws.c_str(),
            static_cast<int>(ws.size()),
            out.data(),
            len,
            nullptr,
            nullptr
        );
        return out;
    }

    // 簡易: wstring → UTF-8 string 変換
    inline std::string WToUtf8(const std::wstring& ws)
    {
        if (ws.empty()) return {};

        int len = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            ws.c_str(),
            static_cast<int>(ws.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (len <= 0) return {};

        std::string utf8(len, '\0');
        ::WideCharToMultiByte(
            CP_UTF8,
            0,
            ws.c_str(),
            static_cast<int>(ws.size()),
            utf8.data(),
            len,
            nullptr,
            nullptr
        );
        return utf8;
    }

    // HTML エスケープ（最低限）
    inline std::string EscapeHtml(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());

        for (char c : s) {
            switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out.push_back(c); break;
            }
        }
        return out;
    }

    inline std::string WToUtf8Html(const std::wstring& ws)
    {
        return EscapeHtml(WToUtf8(ws));
    }

    inline std::string FormatTimeRange(const EpgEvent& ev)
    {
        // 開始
        int sh = ev.start_hour;
        int sm = ev.start_minute;

        // 終了: 分単位で足してから正規化（簡易版）
        int totalStartMin = sh * 60 + sm;
        int totalEndMin = totalStartMin + ev.duration_minutes;

        int eh = (totalEndMin / 60) % 24;
        int em = totalEndMin % 60;

        std::ostringstream oss;

        // ロケールを C ロケールに固定して、3桁区切り(,)などを無効化
        oss.imbue(std::locale::classic());

        oss << std::setfill('0')
            << std::setw(4) << ev.year << "/"
            << std::setw(2) << ev.month << "/"
            << std::setw(2) << ev.day << " "
            << std::setw(2) << sh << ":" << std::setw(2) << sm
            << " - "
            << std::setw(2) << eh << ":" << std::setw(2) << em;
        return oss.str();
    }

} // namespace

// 互換用ラッパ（デフォルト UTF-8）
bool WriteEpgHtmlSimple(
    const std::wstring& filePath,
    const EpgTable& epg,
    const std::vector<ChannelListEntry>& channels)
{
    DebugLog(L"[EPG][HTML] WriteEpgHtmlSimple(UTF-8) 呼び出し: file=\"%s\", epg.services=%zu, channels=%zu",
        filePath.c_str(), epg.size(), channels.size());

    return WriteEpgHtmlSimple(
        filePath,
        epg,
        channels,
        TextEncoding::Utf8   // 既定はUTF-8
    );
}

// エンコード指定版（本体）
bool WriteEpgHtmlSimple(
    const std::wstring& filePath,
    const EpgTable& epg,
    const std::vector<ChannelListEntry>& channels,
    TextEncoding encoding)
{
    // エンコード種別 → コードページ / charset名 / BOM有無 を決定
    UINT codePage = CP_UTF8;
    const char* charsetName = "UTF-8";
    bool writeBom = true;

    if (encoding == TextEncoding::ShiftJis) {
        codePage = 932;              // CP932
        charsetName = "Shift_JIS";
        writeBom = false;            // SJIS では BOM 付けない
    }

    DebugLog(L"[EPG][HTML] 出力開始: file=\"%s\", epg.services=%zu, channels=%zu, encoding=%s",
        filePath.c_str(), epg.size(), channels.size(),
        (encoding == TextEncoding::Utf8) ? L"UTF-8" : L"Shift_JIS");

    // バイナリモードでオープン
    std::ofstream ofs(filePath, std::ios::binary);
    if (!ofs) {
        DebugLog(L"[EPG][HTML][ERROR] ファイルを開けませんでした: \"%s\"", filePath.c_str());
        return false;
    }

    // UTF-8 のときだけ BOM を出力
    if (writeBom) {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        ofs.write(reinterpret_cast<const char*>(bom), 3);
    }

    // --- HTML ヘッダ ---
    ofs <<
        "<!DOCTYPE html>\n"
        "<html lang=\"ja\">\n"
        "<head>\n"
        "  <meta http-equiv=\"Content-Type\" content=\"text/html; charset="
        << charsetName << "\">\n"
        "  <title>EPG Viewer</title>\n"
        "  <style>\n"
        "    body { font-family: sans-serif; }\n"
        "    .channel-block { margin-bottom: 2em; }\n"
        "    .channel-title { font-size: 1.4em; font-weight: bold; margin-bottom: 0.2em; }\n"
        "    .channel-meta { color: #666666; font-size: 0.9em; margin-bottom: 0.5em; }\n"
        "    table.epg { border-collapse: collapse; width: 100%; }\n"
        "    table.epg th, table.epg td {\n"
        "      border: 1px solid #cccccc;\n"
        "      padding: 2px 4px;\n"
        "      vertical-align: top;\n"
        "      font-size: 0.9em;\n"
        "    }\n"
        "    table.epg th { background-color: #f0f0f0; }\n"
        "    .time-cell { white-space: nowrap; }\n"
        "    .title-cell { font-weight: bold; }\n"
        "    .genre-badge {\n"
        "      display: inline-block;\n"
        "      margin-left: 0.5em;\n"
        "      padding: 0 4px;\n"
        "      font-size: 0.8em;\n"
        "      border-radius: 3px;\n"
        "      background-color: #eef5ff;\n"
        "      color: #003366;\n"
        "    }\n"
        "    .rating-badge {\n"
        "      display: inline-block;\n"
        "      margin-left: 0.3em;\n"
        "      padding: 0 4px;\n"
        "      font-size: 0.8em;\n"
        "      border-radius: 3px;\n"
        "      background-color: #ffe8e8;\n"
        "      color: #660000;\n"
        "    }\n"
        "    .series-info {\n"
        "      display: block;\n"
        "      margin-top: 0.2em;\n"
        "      font-size: 0.8em;\n"
        "      color: #444444;\n"
        "    }\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n";

    // タイトル
    const std::wstring titleW = L"EPG 番組表（簡易版）";
    ofs << "<h1>" << WToMultiByte(titleW, codePage) << "</h1>\n";

    std::size_t totalWrittenEvents = 0;
    std::size_t writtenChannels = 0;

    // --- channellist.ch の順番でチャンネルを走査 ---
    for (const auto& ch : channels) {
        // EPG テーブルのキーを作成（ONID/TSID/SID）
        const std::uint64_t key = MakeServiceKey(
            ch.networkId,
            ch.transportStreamId,
            ch.serviceId
        );

        auto it = epg.find(key);
        if (it == epg.end()) {
            // このサービスは EIT がまだ取れていない
            DebugLog(L"[EPG][HTML] EPG なしサービス: serviceName=\"%s\", ONID=%u, TSID=%u, SID=%u",
                ch.serviceName.c_str(),
                ch.networkId,
                ch.transportStreamId,
                ch.serviceId);
            continue;
        }

        const EpgServiceSchedule& sched = it->second;
        const std::wstring& serviceNameW =
            !sched.service_name.empty()
            ? sched.service_name
            : ch.serviceName; // まだ補完していないなら channellist の名前を使う

        DebugLog(L"[EPG][HTML] チャンネル出力: serviceName=\"%s\", events=%zu",
            serviceNameW.c_str(),
            sched.events.size());

        ofs << "<div class=\"channel-block\">\n";

        // チャンネル名 (ServiceName)
        ofs << "  <div class=\"channel-title\">"
            << WToMultiByte(serviceNameW, codePage)
            << "</div>\n";

        // メタ情報: 物理 ch / リモコン ID / ONID/TSID/SID
        {
            std::ostringstream meta;
            meta.imbue(std::locale::classic());

            meta << "space=" << ch.space
                << ", ch=" << ch.channel;
            if (ch.remoteControlKeyId > 0) {
                meta << ", remoteID=" << ch.remoteControlKeyId;
            }
            meta << " / ONID=" << ch.networkId
                << ", TSID=" << ch.transportStreamId
                << ", SID=" << ch.serviceId;

            ofs << "  <div class=\"channel-meta\">"
                << meta.str()
                << "</div>\n";
        }

        // 番組表テーブル
        ofs << "  <table class=\"epg\">\n";

        const std::wstring colTimeW = L"放送時間";
        const std::wstring colTitleW = L"番組名";
        const std::wstring colDescW = L"概要";

        ofs << "    <tr>\n";
        ofs << "      <th>" << WToMultiByte(colTimeW, codePage) << "</th>\n";
        ofs << "      <th>" << WToMultiByte(colTitleW, codePage) << "</th>\n";
        ofs << "      <th>" << WToMultiByte(colDescW, codePage) << "</th>\n";
        ofs << "    </tr>\n";

        for (const auto& ev : sched.events) {

#if EPG_HTML_DEBUG_VERBOSE
            // ★ 特定イベント（例: MX1, ServiceID=23608, EventID=46814）を詳細ログ
            //   必要に応じて値を差し替えて使ってください。
            const int dbgONID = 32391;
            const int dbgTSID = 32391;
            const int dbgSID = 23608;
            const int dbgEID = 46814;

            if (ev.original_network_id == dbgONID &&
                ev.transport_stream_id == dbgTSID &&
                ev.service_id == dbgSID &&
                ev.event_id == dbgEID)
            {
                DebugLog(
                    L"[EPG][HTML-DBG] TargetEvent: "
                    L"ONID=0x%04X TSID=0x%04X SID=0x%04X EID=0x%04X "
                    L"date=%04d/%02d/%02d time=%02d:%02d dur=%d "
                    L"title=\"%s\" shortLen=%zu extLen=%zu",
                    ev.original_network_id,
                    ev.transport_stream_id,
                    ev.service_id,
                    ev.event_id,
                    ev.year, ev.month, ev.day,
                    ev.start_hour, ev.start_minute,
                    ev.duration_minutes,
                    ev.event_name.c_str(),
                    ev.short_text.size(),
                    ev.extended_text.size()
                );
            }
#endif // EPG_HTML_DEBUG_VERBOSE

            ofs << "    <tr>\n";

            // 1列目: 放送時間
            ofs << "      <td class=\"time-cell\">";
            ofs << FormatTimeRange(ev);  // 数字と記号だけなので codePage 変換不要
            ofs << "</td>\n";

            // 2列目: タイトル + ジャンル + 年齢制限
            ofs << "      <td class=\"title-cell\">";
            if (!ev.event_name.empty()) {
                ofs << WToMultiByte(ev.event_name, codePage);
            }

            {
                std::wstring genreLabelW = GetGenreLabel(ev);
                if (!genreLabelW.empty()) {
                    ofs << " <span class=\"genre-badge\">"
                        << WToMultiByte(genreLabelW, codePage)
                        << "</span>";
                }
            }

            {
                std::wstring ratingLabelW = GetParentalRatingLabel(ev);
                if (!ratingLabelW.empty()) {
                    ofs << " <span class=\"rating-badge\">"
                        << WToMultiByte(ratingLabelW, codePage)
                        << "</span>";
                }
            }

            ofs << "</td>\n";

            // 3列目: 概要（short + extended + シリーズ + ジャンル + 年齢制限）
            ofs << "      <td>";

            bool firstLine = true;
            auto writeLine = [&](const std::wstring& line)
                {
                    if (line.empty()) return;
                    if (!firstLine) {
                        ofs << "<br>";
                    }
                    ofs << WToMultiByte(line, codePage);
                    firstLine = false;
                };

            // 1) 短形式概要
            if (!ev.short_text.empty()) {
                writeLine(ev.short_text);
            }

            // 2) 拡張概要
            if (!ev.extended_text.empty()) {
                writeLine(ev.extended_text);
            }

            // 2) 拡張概要（[拡張items]）
#if EPG_HTML_ENABLE_EXTENDED_TEXT
            if (!ev.extended_text.empty()) {
                writeLine(ev.extended_text);
            }
#endif

            // 3) シリーズ情報
            if (ev.has_series) {
                std::wstring line = L"【シリーズ】";

                if (!ev.series_name.empty()) {
                    line += ev.series_name;
                    line += L" ";
                }

                if (ev.episode_number > 0) {
                    std::wostringstream woss;
                    woss.imbue(std::locale::classic());
                    woss << static_cast<unsigned>(ev.episode_number);
                    line += woss.str();
                    line += L"話";

                    if (ev.last_episode_number > 0) {
                        std::wostringstream woss2;
                        woss2.imbue(std::locale::classic());
                        woss2 << static_cast<unsigned>(ev.last_episode_number);
                        line += L"/";
                        line += woss2.str();
                        line += L"話";
                    }
                }

                writeLine(line);
            }

            // 4) ジャンル（概要欄にも文字として入れる場合）
            if (ev.has_genre) {
                std::wstring line = L"【ジャンル】" + GetGenreLabel(ev);
                writeLine(line);
            }

            // 5) 年齢制限
            if (ev.has_parental_rating) {
                std::wstring line = L"【年齢制限】" + GetParentalRatingLabel(ev);
                writeLine(line);
            }

            ofs << "</td>\n";

            ofs << "    </tr>\n";

            ++totalWrittenEvents;
        }

        ofs << "  </table>\n";
        ofs << "</div>\n";

        ++writtenChannels;
    }

    ofs << "</body>\n</html>\n";

    DebugLog(L"[EPG][HTML] 出力完了: file=\"%s\", 書き出しチャンネル数=%zu, 書き出しイベント総数=%zu",
        filePath.c_str(), writtenChannels, totalWrittenEvents);

    return true;
}
