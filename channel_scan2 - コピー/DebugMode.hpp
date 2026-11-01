#pragma once
// DebugMode.hpp
#pragma once
#ifndef DEBUGMODE_HPP
#define DEBUGMODE_HPP

#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <algorithm>

namespace debugmode {

    // ----------------------------
    // サニタイズ（ログ用）
    // ----------------------------
    inline std::wstring SanitizeForLog(std::wstring_view src, std::size_t maxLen = 512)
    {
        std::wstring out;
        out.reserve((std::min)(src.size(), maxLen));

        for (wchar_t ch : src) {
            if (out.size() >= maxLen) {
                out.push_back(L'…');
                break;
            }
            if (ch < 0x20) {
                if (ch == L'\n' || ch == L'\r' || ch == L'\t') out.push_back(ch);
                else out.push_back(L'?');
                continue;
            }
            // surrogate はそのまま通す（元コード互換）
            if (ch >= 0xD800 && ch <= 0xDFFF) {
                out.push_back(ch);
                continue;
            }
            out.push_back(ch);
        }
        return out;
    }

    // ----------------------------
    // y/n 入力（元コード互換）
    // ----------------------------
    inline bool AskYesNo(std::wistream& in, std::wostream& out, std::wstring_view prompt)
    {
        while (true) {
            out << prompt;
            out.flush();

            wchar_t resp = 0;
            if (!(in >> resp)) {
                in.clear();
                in.ignore((std::numeric_limits<std::streamsize>::max)(), L'\n');
                return false;
            }

            switch (resp) {
            case L'y': case L'Y': return true;
            case L'n': case L'N': return false;
            default:
                out << L"[WARN] y か n で入力してください。\n";
                break;
            }
        }
    }

    // ----------------------------
    // テスト用：選択結果
    // ----------------------------
    struct FilterDebugSelection {
        bool enableSDTDebug = false;
        bool enableNITDebug = false;
    };

    struct DumpSelection {
        bool enableTsDump = false;
        bool enableRingDump = false;
        bool enableEitDump = true;
        bool enableAribTrace = false;
    };

    struct DebugSelections {
        FilterDebugSelection filter;
        DumpSelection dump;
    };

    struct PromptConfig {
        bool forceEitDump = false;          // true の場合、EIT dump を強制 ON
        bool showForceEitMessage = true;    // 強制ONの理由メッセージを出す
    };

    // ----------------------------
    // まとめて質問して返す
    // ----------------------------
    inline DebugSelections PromptEpgTestSelections(
        std::wistream& in,
        std::wostream& out,
        const PromptConfig& cfg = {})
    {
        DebugSelections sel{};

        // --- フィルター詳細ログ（SDT/NIT） ---
        sel.filter.enableSDTDebug = AskYesNo(in, out, L"[INPUT] SDTFilter の詳細デバッグログを有効にしますか？ (y/n): ");
        sel.filter.enableNITDebug = AskYesNo(in, out, L"[INPUT] NITFilter の詳細デバッグログを有効にしますか？ (y/n): ");

        // --- ダンプ系 ---
        sel.dump.enableTsDump = AskYesNo(in, out, L"[INPUT] TSストリームの保存を行いますか？ (y/n): ");
        sel.dump.enableRingDump = AskYesNo(in, out, L"[INPUT] リングバッファ内のTSストリームの保存を行いますか？ (y/n): ");

        if (cfg.forceEitDump) {
            sel.dump.enableEitDump = true;
            if (cfg.showForceEitMessage) {
                out << L"[INFO] このテストでは EIT(.epg) 出力は強制的に有効です（後段処理に必要）。\n";
            }
        }
        else {
            sel.dump.enableEitDump = AskYesNo(in, out, L"[INPUT] EIT(番組情報)をテキストファイルに出力しますか？ (y/n): ");
        }

        sel.dump.enableAribTrace = AskYesNo(in, out, L"[INPUT] EIT ARIB デコード詳細トレースを有効にしますか？ (y/n): ");

        return sel;
    }

} // namespace debugmode

#endif // DEBUGMODE_HPP
