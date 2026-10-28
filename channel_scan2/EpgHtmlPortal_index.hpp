#pragma once
#ifndef EPG_HTML_PORTAL_INDEX_HPP
#define EPG_HTML_PORTAL_INDEX_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <filesystem>
#include <string_view>

#include "EpgHtmlPortal.hpp"

namespace EpgHtmlPortal_index
{
    // 番組表HTMLが未生成の場合に表示する簡易プレースホルダーHTMLを生成する。
    //
    // htmlPath:
    //   出力先HTMLパス。
    //
    // title:
    //   プレースホルダーのページタイトル。
    //
    // message:
    //   表示メッセージ。
    bool EnsurePlaceholderHtml(
        const std::filesystem::path& htmlPath,
        std::wstring_view title,
        std::wstring_view message);

    // EPGポータルの index.html を生成する。
    //
    // 現在の実装では index 側で以下を担当する。
    // - 上位切替メニュー
    // - 番組表の下位切替
    // - 放送波切替
    // - 現在/過去切替
    // - 時間割/一覧切替
    // - 予約一覧/検索ページへの切替
    //
    // 時間割/一覧の実ファイル切替は RuntimeData の SPACE_DATA に
    // currentTimetableSrc/currentListSrc/beforeTimetableSrc/beforeListSrc が
    // 追加されると有効になる。
    // 未対応時は currentSrc/beforeSrc にフォールバックする。
    bool WritePortalIndexHtml(
        const EpgHtmlPortal::WriteOptions& opt);
}

#endif // EPG_HTML_PORTAL_INDEX_HPP