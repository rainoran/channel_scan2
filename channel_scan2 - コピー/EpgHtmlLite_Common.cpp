#include "EpgHtmlLite_Common.hpp"

#include <climits>
#include <sstream>
#include <utility>

namespace
{
    std::wstring HtmlEscape(const std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 16);

        for (const wchar_t ch : s) {
            switch (ch) {
            case L'&': out += L"&amp;";  break;
            case L'<': out += L"&lt;";   break;
            case L'>': out += L"&gt;";   break;
            case L'"': out += L"&quot;"; break;
            default:   out.push_back(ch); break;
            }
        }

        return out;
    }

    std::wstring MakeSpaceSuffix(const int space)
    {
        return L"_space" + std::to_wstring(space);
    }

    std::wstring AppendQuery(const std::wstring& base, const std::wstring& query)
    {
        if (query.empty()) {
            return base;
        }

        if (base.find(L'?') == std::wstring::npos) {
            return base + L"?" + query;
        }

        return base + L"&" + query;
    }

    void AppendUtf8CodePoint_(
        std::string& out,
        const std::uint32_t codePoint)
    {
        if (codePoint <= 0x7FU) {
            out.push_back(static_cast<char>(codePoint));
            return;
        }

        if (codePoint <= 0x7FFU) {
            out.push_back(static_cast<char>(
                0xC0U | ((codePoint >> 6U) & 0x1FU)));
            out.push_back(static_cast<char>(
                0x80U | (codePoint & 0x3FU)));
            return;
        }

        if (codePoint <= 0xFFFFU) {
            out.push_back(static_cast<char>(
                0xE0U | ((codePoint >> 12U) & 0x0FU)));
            out.push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(
                0x80U | (codePoint & 0x3FU)));
            return;
        }

        out.push_back(static_cast<char>(
            0xF0U | ((codePoint >> 18U) & 0x07U)));
        out.push_back(static_cast<char>(
            0x80U | ((codePoint >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(
            0x80U | ((codePoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(
            0x80U | (codePoint & 0x3FU)));
    }

    std::string ToUtf8_(
        const std::wstring_view text)
    {
        std::string out;
        out.reserve(text.size() * 3U + 8U);

        for (std::size_t i = 0; i < text.size(); ++i) {
            std::uint32_t codePoint =
                static_cast<std::uint32_t>(text[i]);

#if WCHAR_MAX <= 0xFFFF
            if (codePoint >= 0xD800U &&
                codePoint <= 0xDBFFU &&
                (i + 1U) < text.size()) {
                const std::uint32_t low =
                    static_cast<std::uint32_t>(text[i + 1U]);

                if (low >= 0xDC00U &&
                    low <= 0xDFFFU) {
                    codePoint =
                        0x10000U +
                        (((codePoint - 0xD800U) << 10U) |
                            (low - 0xDC00U));
                    ++i;
                }
            }
#endif

            AppendUtf8CodePoint_(out, codePoint);
        }

        return out;
    }

    bool IsUnreservedUrlByte_(
        const unsigned char ch) noexcept
    {
        return
            (ch >= static_cast<unsigned char>('A') &&
                ch <= static_cast<unsigned char>('Z')) ||
            (ch >= static_cast<unsigned char>('a') &&
                ch <= static_cast<unsigned char>('z')) ||
            (ch >= static_cast<unsigned char>('0') &&
                ch <= static_cast<unsigned char>('9')) ||
            ch == static_cast<unsigned char>('-') ||
            ch == static_cast<unsigned char>('.') ||
            ch == static_cast<unsigned char>('_') ||
            ch == static_cast<unsigned char>('~');
    }

    std::wstring UrlEncode_(
        const std::wstring_view text)
    {
        static constexpr wchar_t kHex[] =
            L"0123456789ABCDEF";

        const std::string utf8 = ToUtf8_(text);

        std::wstring out;
        out.reserve(utf8.size() * 3U);

        for (const char value : utf8) {
            const unsigned char ch =
                static_cast<unsigned char>(value);

            if (IsUnreservedUrlByte_(ch)) {
                out.push_back(static_cast<wchar_t>(ch));
                continue;
            }

            out.push_back(L'%');
            out.push_back(kHex[(ch >> 4U) & 0x0FU]);
            out.push_back(kHex[ch & 0x0FU]);
        }

        return out;
    }

    std::wstring RenderLinksOnly(const std::vector<EpgHtmlLite::Common::NavLink>& links)
    {
        if (links.empty()) {
            return L"";
        }

        std::wostringstream oss;
        for (const auto& link : links) {
            oss << LR"HTML(<a class="lite-switch-link )HTML"
                << (link.active ? L"active" : L"")
                << (link.implemented ? L"" : L" placeholder")
                << LR"HTML(" href=")HTML" << HtmlEscape(link.href) << LR"HTML(">)HTML"
                << HtmlEscape(link.label)
                << LR"HTML(</a>
)HTML";
        }
        return oss.str();
    }

    std::wstring RenderLinkRow(
        const std::wstring& label,
        const std::vector<EpgHtmlLite::Common::NavLink>& links,
        const wchar_t* extraClass = L"")
    {
        if (links.empty()) {
            return L"";
        }

        std::wostringstream oss;
        oss << LR"HTML(<div class="lite-switch-row )HTML" << extraClass << LR"HTML(">
<div class="lite-switch-label">)HTML" << HtmlEscape(label) << LR"HTML(</div>
<div class="lite-switch-links">
)HTML";

        oss << RenderLinksOnly(links);

        oss << LR"HTML(</div>
</div>
)HTML";

        return oss.str();
    }

    std::wstring RenderGuideControlRow(
        const std::vector<EpgHtmlLite::Common::NavLink>& broadcastLinks,
        const std::vector<EpgHtmlLite::Common::NavLink>& scopeLinks,
        const std::vector<EpgHtmlLite::Common::NavLink>& viewLinks,
        const std::vector<EpgHtmlLite::Common::NavLink>& timeLinks)
    {
        if (broadcastLinks.empty() && scopeLinks.empty() && viewLinks.empty() && timeLinks.empty()) {
            return L"";
        }

        std::wostringstream oss;
        oss << LR"HTML(<div class="lite-switch-row guide-control-row">
<div class="lite-switch-label">切替</div>
<div class="guide-control-wrap">
)HTML";

        auto emitGroup = [&](const wchar_t* title, const std::vector<EpgHtmlLite::Common::NavLink>& links) {
            if (links.empty()) return;
            oss << LR"HTML(<div class="guide-control-group">
<div class="guide-control-name">)HTML" << HtmlEscape(title) << LR"HTML(</div>
<div class="guide-control-links">
)HTML";
            oss << RenderLinksOnly(links);
            oss << LR"HTML(</div>
</div>
)HTML";
            };

        emitGroup(L"放送", broadcastLinks);
        emitGroup(L"対象", scopeLinks);
        emitGroup(L"表示", viewLinks);
        emitGroup(L"時間", timeLinks);

        oss << LR"HTML(</div>
</div>
)HTML";
        return oss.str();
    }

    bool IsSpaceChar(
        wchar_t ch) noexcept
    {
        return ch == L' ' ||
            ch == L'\t' ||
            ch == L'\r' ||
            ch == L'\n' ||
            ch == L'　';
    }

    std::wstring TrimText(
        std::wstring_view src)
    {
        std::size_t begin = 0;
        std::size_t end = src.size();

        while (begin < end &&
            IsSpaceChar(src[begin])) {
            ++begin;
        }

        while (end > begin &&
            IsSpaceChar(src[end - 1])) {
            --end;
        }

        return std::wstring(
            src.substr(begin, end - begin));
    }

    std::wstring ToLowerAscii(
        std::wstring_view src)
    {
        std::wstring out(src);

        for (wchar_t& ch : out) {
            if (ch >= L'A' && ch <= L'Z') {
                ch = static_cast<wchar_t>(
                    L'a' + (ch - L'A'));
            }
        }

        return out;
    }

    int NormalizeGenreCode(
        int major) noexcept
    {
        major &= 0x0F;

        switch (major) {
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x3:
        case 0x4:
        case 0x5:
        case 0x6:
        case 0x7:
        case 0x8:
        case 0x9:
        case 0xA:
            return major;
        default:
            return 0xF;
        }
    }

    bool TryParseAutoBaseInt(
        std::wstring_view src,
        int& outValue) noexcept
    {
        outValue = 0;

        const std::wstring s =
            TrimText(src);

        if (s.empty()) {
            return false;
        }

        int base = 10;
        std::size_t pos = 0;

        if (s.size() >= 2 &&
            s[0] == L'0' &&
            (s[1] == L'x' || s[1] == L'X')) {
            base = 16;
            pos = 2;
        }

        if (pos >= s.size()) {
            return false;
        }

        int value = 0;
        bool any = false;

        for (; pos < s.size(); ++pos) {
            const wchar_t ch = s[pos];
            int digit = -1;

            if (ch >= L'0' && ch <= L'9') {
                digit = static_cast<int>(ch - L'0');
            }
            else if (ch >= L'a' && ch <= L'f') {
                digit = 10 + static_cast<int>(ch - L'a');
            }
            else if (ch >= L'A' && ch <= L'F') {
                digit = 10 + static_cast<int>(ch - L'A');
            }
            else {
                break;
            }

            if (digit < 0 ||
                digit >= base) {
                return false;
            }

            value = value * base + digit;
            any = true;
        }

        if (!any) {
            return false;
        }

        outValue = value;
        return true;
    }

    bool TryExtractGenreCodeFromText(
        std::wstring_view src,
        int& outMajor) noexcept
    {
        outMajor = -1;

        const std::wstring text(src);

        const std::array<std::wstring_view, 6> keys = {
            L"primary=0x",
            L"primary=",
            L"genre_primary=0x",
            L"genre_primary=",
            L"genrePrimary=",
            L"major="
        };

        for (std::wstring_view key : keys) {
            const std::size_t pos =
                text.find(key);

            if (pos == std::wstring::npos) {
                continue;
            }

            const std::size_t valueBegin =
                pos + key.size();

            std::size_t valueEnd =
                text.find_first_of(
                    L", )]\r\n\t",
                    valueBegin);

            if (valueEnd == std::wstring::npos) {
                valueEnd = text.size();
            }

            std::wstring valueText =
                text.substr(
                    valueBegin,
                    valueEnd - valueBegin);

            if (key.find(L"0x") != std::wstring::npos) {
                valueText = L"0x" + valueText;
            }

            int parsed = -1;

            if (TryParseAutoBaseInt(
                valueText,
                parsed)) {
                outMajor = NormalizeGenreCode(parsed);
                return true;
            }
        }

        int parsed = -1;

        if (TryParseAutoBaseInt(
            text,
            parsed)) {
            outMajor = NormalizeGenreCode(parsed);
            return true;
        }

        return false;
    }
}//  anonymous namespace

namespace EpgHtmlLite::Common
{
    const std::array<std::wstring_view, 12>& GetGenreMajorNames()
    {
        static const std::array<std::wstring_view, 12> names = {
            L"ニュース・報道",
            L"スポーツ",
            L"ドラマ",
            L"音楽",
            L"バラエティー",
            L"映画",
            L"アニメ・特撮",
            L"情報・ワイドショー",
            L"ドキュメンタリー",
            L"劇場・公演",
            L"趣味・教育",
            L"その他"
        };

        return names;
    }

    std::wstring NormalizeGenreMajorName(
        std::wstring_view genreText)
    {
        std::wstring genre =
            TrimText(genreText);

        if (genre.empty()) {
            return L"その他";
        }

        auto cutAt = [&](const std::wstring& sep) {
            const std::size_t pos = genre.find(sep);
            if (pos != std::wstring::npos) {
                genre =
                    TrimText(
                        std::wstring_view(
                            genre.data(),
                            pos));
            }
            };

        cutAt(L">>");
        cutAt(L"／");
        cutAt(L"/");
        cutAt(L"|");
        cutAt(L";");

        for (std::wstring_view name : GetGenreMajorNames()) {
            if (genre == name ||
                genre.find(name) != std::wstring::npos) {
                return std::wstring(name);
            }
        }

        // 日本語の揺れ・短縮表記対応。
        if (genre.find(L"ニュース") != std::wstring::npos ||
            genre.find(L"報道") != std::wstring::npos) {
            return L"ニュース・報道";
        }

        if (genre.find(L"スポーツ") != std::wstring::npos) {
            return L"スポーツ";
        }

        if (genre.find(L"情報") != std::wstring::npos ||
            genre.find(L"ワイドショー") != std::wstring::npos) {
            return L"情報・ワイドショー";
        }

        if (genre.find(L"ドラマ") != std::wstring::npos) {
            return L"ドラマ";
        }

        if (genre.find(L"音楽") != std::wstring::npos) {
            return L"音楽";
        }

        if (genre.find(L"バラエティ") != std::wstring::npos ||
            genre.find(L"バラエティー") != std::wstring::npos) {
            return L"バラエティー";
        }

        if (genre.find(L"映画") != std::wstring::npos) {
            return L"映画";
        }

        if (genre.find(L"アニメ") != std::wstring::npos ||
            genre.find(L"特撮") != std::wstring::npos) {
            return L"アニメ・特撮";
        }

        if (genre.find(L"ドキュメンタリー") != std::wstring::npos ||
            genre.find(L"教養") != std::wstring::npos) {
            return L"ドキュメンタリー";
        }

        if (genre.find(L"劇場") != std::wstring::npos ||
            genre.find(L"公演") != std::wstring::npos) {
            return L"劇場・公演";
        }

        if (genre.find(L"趣味") != std::wstring::npos ||
            genre.find(L"教育") != std::wstring::npos) {
            return L"趣味・教育";
        }

        const std::wstring key =
            ToLowerAscii(genre);

        if (key == L"news" ||
            key == L"news_report" ||
            key == L"report" ||
            key == L"news-report") {
            return L"ニュース・報道";
        }

        if (key == L"sports" ||
            key == L"sport") {
            return L"スポーツ";
        }

        if (key == L"info" ||
            key == L"wide" ||
            key == L"information" ||
            key == L"wide_show" ||
            key == L"wide-show") {
            return L"情報・ワイドショー";
        }

        if (key == L"drama") {
            return L"ドラマ";
        }

        if (key == L"music") {
            return L"音楽";
        }

        if (key == L"variety" ||
            key == L"variety_show" ||
            key == L"variety-show") {
            return L"バラエティー";
        }

        if (key == L"movie" ||
            key == L"cinema" ||
            key == L"film") {
            return L"映画";
        }

        if (key == L"anime" ||
            key == L"animation" ||
            key == L"tokusatsu") {
            return L"アニメ・特撮";
        }

        if (key == L"documentary" ||
            key == L"doc" ||
            key == L"culture") {
            return L"ドキュメンタリー";
        }

        if (key == L"stage" ||
            key == L"theater" ||
            key == L"theatre" ||
            key == L"performance") {
            return L"劇場・公演";
        }

        if (key == L"hobby" ||
            key == L"education" ||
            key == L"edu") {
            return L"趣味・教育";
        }

        if (key == L"other" ||
            key == L"all") {
            return L"その他";
        }

        int major = -1;

        if (TryExtractGenreCodeFromText(
            genre,
            major)) {
            switch (NormalizeGenreCode(major)) {
            case 0x0:
                return L"ニュース・報道";
            case 0x1:
                return L"スポーツ";
            case 0x2:
                return L"情報・ワイドショー";
            case 0x3:
                return L"ドラマ";
            case 0x4:
                return L"音楽";
            case 0x5:
                return L"バラエティー";
            case 0x6:
                return L"映画";
            case 0x7:
                return L"アニメ・特撮";
            case 0x8:
                return L"ドキュメンタリー";
            case 0x9:
                return L"劇場・公演";
            case 0xA:
                return L"趣味・教育";
            default:
                return L"その他";
            }
        }

        return L"その他";
    }

    std::wstring BuildGuidePageFileName(const GuidePageKind kind, const int space)
    {
        const std::wstring suffix = MakeSpaceSuffix(space);

        switch (kind) {
        case GuidePageKind::NowTimetable:
            return L"epg_lite_now_timetable" + suffix + L".html";
        case GuidePageKind::NowList:
            return L"epg_lite_now_list" + suffix + L".html";
        case GuidePageKind::BeforeTimetable:
            return L"epg_lite_before_timetable" + suffix + L".html";
        case GuidePageKind::BeforeList:
            return L"epg_lite_before_list" + suffix + L".html";
        case GuidePageKind::Unknown:
        default:
            return L"epg_lite_program_menu" + suffix + L".html";
        }
    }

    std::wstring BuildAutoReserveEntryFileName(
        const int space)
    {
        return
            L"/epg_out/epg_lite_keyword_search_space" +
            std::to_wstring(space) +
            L".html";
    }

    std::wstring BuildAutoReserveEntryUrl(
        const std::wstring_view programTitle,
        const std::wstring_view configuredEntryUrl,
        const int space)
    {
        const std::wstring entryUrl =
            configuredEntryUrl.empty()
            ? BuildAutoReserveEntryFileName(space)
            : std::wstring(configuredEntryUrl);

        const wchar_t separator =
            entryUrl.find(L'?') == std::wstring::npos
            ? L'?'
            : L'&';

        return
            entryUrl +
            separator +
            L"mode=new&keyword=" +
            UrlEncode_(programTitle);
    }

    std::vector<NavLink> BuildMainMenuLinks(const int space, const MenuId activeMenu)
    {
        const std::wstring suffix = MakeSpaceSuffix(space);

        std::vector<NavLink> links;
        links.reserve(11);

        auto push = [&](MenuId id, const wchar_t* label, std::wstring href, bool implemented) {
            NavLink link{};
            link.label = label;
            link.href = std::move(href);
            link.active = (id == activeMenu);
            link.implemented = implemented;
            links.push_back(std::move(link));
            };

        push(MenuId::ProgramGuide, L"番組表", L"epg_lite_program_menu" + suffix + L".html", true);
        push(MenuId::ReservationList, L"予約リスト", L"epg_lite_reservation_list" + suffix + L".html", true);
        push(MenuId::KeywordSearch, L"キーワード検索", L"epg_lite_keyword_search" + suffix + L".html", true);
        push(MenuId::AutoKeywordReservationList, L"自動検索予約リスト", L"epg_lite_auto_keyword_reservation_list" + suffix + L".html", true);
        push(MenuId::ReservationEdit, L"拡張ページ", L"epg_lite_extension" + suffix + L".html", false);
        push(MenuId::Controller, L"コントローラー", L"epg_lite_controller" + suffix + L".html", false);
        push(MenuId::NewPrograms, L"新番組", L"epg_lite_new_programs" + suffix + L".html", false);
        push(MenuId::Recommended, L"オススメ", L"epg_lite_recommended" + suffix + L".html", false);
        push(MenuId::Movie, L"ムービー", L"epg_lite_movie" + suffix + L".html", false);
        push(MenuId::Log, L"ログ", L"epg_lite_log" + suffix + L".html", true);
        push(MenuId::PastPrograms, L"過去番組", L"epg_lite_past_program_menu" + suffix + L".html", true);

        return links;
    }

    std::vector<NavLink> BuildBroadcastLinks(const GuidePageKind kind, const int activeSpace)
    {
        std::vector<NavLink> links;
        links.reserve(3);

        struct SpaceInfo {
            int space;
            const wchar_t* label;
        };

        static constexpr SpaceInfo kSpaces[] = {
            { 0, L"地上波" },
            { 1, L"BS"   },
            { 2, L"CS"   }
        };

        for (const auto& item : kSpaces) {
            NavLink link{};
            link.label = item.label;
            link.href = BuildGuidePageFileName(kind, item.space);
            link.active = (item.space == activeSpace);
            link.implemented = true;
            links.push_back(std::move(link));
        }

        return links;
    }

    std::vector<NavLink> BuildGuideScopeLinks(const int space, const GuidePageKind activeKind)
    {
        std::vector<NavLink> links;
        links.reserve(2);

        const bool isPast =
            (activeKind == GuidePageKind::BeforeTimetable || activeKind == GuidePageKind::BeforeList);

        NavLink nowLink{};
        nowLink.label = L"現在";
        nowLink.href = L"epg_lite_program_menu_space" + std::to_wstring(space) + L".html";
        nowLink.active = !isPast;
        nowLink.implemented = true;
        links.push_back(std::move(nowLink));

        NavLink pastLink{};
        pastLink.label = L"過去";
        pastLink.href = L"epg_lite_past_program_menu_space" + std::to_wstring(space) + L".html";
        pastLink.active = isPast;
        pastLink.implemented = true;
        links.push_back(std::move(pastLink));

        return links;
    }

    std::vector<NavLink> BuildGuideViewLinks(const int space, const GuidePageKind activeKind)
    {
        std::vector<NavLink> links;
        links.reserve(2);

        const bool isPast =
            (activeKind == GuidePageKind::BeforeTimetable || activeKind == GuidePageKind::BeforeList);
        const bool isList =
            (activeKind == GuidePageKind::NowList || activeKind == GuidePageKind::BeforeList);

        NavLink timetableLink{};
        timetableLink.label = L"時間割";
        timetableLink.href = isPast
            ? BuildGuidePageFileName(GuidePageKind::BeforeTimetable, space)
            : BuildGuidePageFileName(GuidePageKind::NowTimetable, space);
        timetableLink.active = !isList;
        timetableLink.implemented = true;
        links.push_back(std::move(timetableLink));

        NavLink listLink{};
        listLink.label = L"表一覧";
        listLink.href = isPast
            ? BuildGuidePageFileName(GuidePageKind::BeforeList, space)
            : BuildGuidePageFileName(GuidePageKind::NowList, space);
        listLink.active = isList;
        listLink.implemented = true;
        links.push_back(std::move(listLink));

        return links;
    }

    std::vector<NavLink> BuildTimeBlockLinks(
        const int space,
        const GuidePageKind activeKind,
        const int activeBlock)
    {
        std::vector<NavLink> links;
        links.reserve(4);

        const std::wstring base = BuildGuidePageFileName(activeKind, space);
        if (base.empty()) {
            return links;
        }

        static constexpr const wchar_t* kLabels[4] = {
            L"04-10", L"10-16", L"16-22", L"22-04"
        };

        for (int i = 0; i < 4; ++i) {
            NavLink link{};
            link.label = kLabels[i];
            link.href = AppendQuery(base, L"block=" + std::to_wstring(i));
            link.active = (i == activeBlock);
            link.implemented = true;
            links.push_back(std::move(link));
        }

        return links;
    }

    std::vector<NavLink> BuildGenreLinks(
        const int space,
        const GuidePageKind activeKind,
        const int activeBlock,
        const std::wstring_view activeGenreKey)
    {
        std::vector<NavLink> links;
        links.reserve(13);

        const std::wstring base =
            BuildGuidePageFileName(
                activeKind,
                space);

        if (base.empty()) {
            return links;
        }

        struct GenreItem
        {
            const wchar_t* key;
            const wchar_t* label;
            const wchar_t* majorName;
        };

        static constexpr GenreItem kGenres[] = {
            { L"all",         L"全て",       L"その他" },
            { L"news",        L"ニュース",   L"ニュース・報道" },
            { L"sports",      L"スポーツ",   L"スポーツ" },
            { L"drama",       L"ドラマ",     L"ドラマ" },
            { L"music",       L"音楽",       L"音楽" },
            { L"variety",     L"バラエティ", L"バラエティー" },
            { L"movie",       L"映画",       L"映画" },
            { L"anime",       L"アニメ",     L"アニメ・特撮" },
            { L"info",        L"情報",       L"情報・ワイドショー" },
            { L"documentary", L"ドキュメンタリー", L"ドキュメンタリー" },
            { L"stage",       L"劇場",       L"劇場・公演" },
            { L"hobby",       L"趣味・教育", L"趣味・教育" },
            { L"other",       L"その他",     L"その他" }
        };

        const std::wstring activeKey =
            ToLowerAscii(
                TrimText(activeGenreKey));

        const std::wstring activeMajor =
            NormalizeGenreMajorName(activeGenreKey);

        for (const auto& item : kGenres) {
            NavLink link{};
            link.label = item.label;
            link.href =
                AppendQuery(
                    base,
                    L"block=" +
                    std::to_wstring(activeBlock) +
                    L"&genre=" +
                    item.key);

            if (activeKey.empty()) {
                link.active =
                    std::wstring(item.key) == L"all";
            }
            else {
                link.active =
                    (activeKey == item.key) ||
                    (activeMajor == item.majorName &&
                        std::wstring(item.key) != L"all");
            }

            link.implemented = true;
            links.push_back(std::move(link));
        }

        return links;
    }

    std::wstring BuildCommonCss()
    {
        return LR"CSS(
:root{
    --lite-bg:#0f131a;
    --lite-panel:#181f29;
    --lite-panel2:#1d2632;
    --lite-line:#2d3848;
    --lite-text:#e8edf5;
    --lite-sub:#aab6c8;
    --lite-accent:#4da3ff;
    --lite-active-bg:#1d4d7f;
    --lite-active-line:#6bb7ff;
    --lite-placeholder-bg:#2a2d33;
    --lite-placeholder-line:#5d6572;
}
body{
    margin:0;
    background:var(--lite-bg);
    color:var(--lite-text);
    font-family:"Segoe UI","Yu Gothic UI","Meiryo",sans-serif;
}
.lite-shell{
    min-height:100vh;
    display:grid;
    grid-template-rows:auto 1fr;
}
.lite-shell-header{
    position:sticky;
    top:0;
    z-index:120;
    background:#11161d;
    border-bottom:1px solid var(--lite-line);
    padding:12px 16px 12px;
    box-shadow:0 6px 16px rgba(0,0,0,0.28);
}
.lite-title-block{
    margin-bottom:10px;
}
.lite-title-block h1{
    margin:0 0 4px;
    font-size:24px;
}
.lite-title-block .sub{
    color:var(--lite-sub);
    font-size:13px;
}
.lite-switch-band{
    display:grid;
    gap:8px;
}
.lite-switch-row{
    display:grid;
    grid-template-columns:92px 1fr;
    gap:10px;
    align-items:start;
}
.lite-switch-row.top-index-row{
    padding-bottom:6px;
    border-bottom:1px solid rgba(255,255,255,0.06);
    margin-bottom:2px;
}
.lite-switch-label{
    color:var(--lite-sub);
    font-size:12px;
    font-weight:700;
    padding-top:8px;
    user-select:none;
}
.lite-switch-row.top-index-row .lite-switch-label{
    color:#d7e9ff;
}
.lite-switch-links{
    display:flex;
    flex-wrap:wrap;
    gap:8px;
}
.lite-switch-link{
    display:inline-flex;
    align-items:center;
    justify-content:center;
    min-height:34px;
    padding:7px 12px;
    border-radius:999px;
    border:1px solid #425067;
    background:#1a2330;
    color:var(--lite-text);
    text-decoration:none;
    font-size:13px;
    font-weight:700;
    line-height:1.2;
}
.lite-switch-link:hover{
    transform:translateY(-1px);
    border-color:var(--lite-accent);
}
.lite-switch-link.active{
    background:var(--lite-active-bg);
    border-color:var(--lite-active-line);
    color:#eef7ff;
}
.lite-switch-link.placeholder{
    background:var(--lite-placeholder-bg);
    border-color:var(--lite-placeholder-line);
    color:#d6dbe4;
    border-style:dashed;
}
.guide-control-wrap{
    display:flex;
    flex-wrap:wrap;
    gap:10px 16px;
}
.guide-control-group{
    display:flex;
    align-items:center;
    flex-wrap:wrap;
    gap:8px;
    padding:2px 0;
}
.guide-control-name{
    min-width:28px;
    font-size:12px;
    font-weight:700;
    color:var(--lite-sub);
}
.guide-control-links{
    display:flex;
    flex-wrap:wrap;
    gap:8px;
}
.lite-shell-main{
    min-height:0;
    padding:12px 16px 16px;
}
.lite-content-frame{
    min-height:calc(100vh - 220px);
    background:var(--lite-panel);
    border:1px solid var(--lite-line);
    border-radius:12px;
    padding:14px 16px;
    overflow:auto;
}
.lite-dummy-panel{
    background:var(--lite-panel2);
    border:1px solid var(--lite-line);
    border-radius:12px;
    padding:14px 16px;
}
.lite-dummy-panel h2{
    margin:0 0 10px;
    font-size:18px;
}
.lite-dummy-panel p{
    margin:0;
    color:var(--lite-sub);
    line-height:1.7;
    font-size:13px;
}
@media (max-width: 900px){
    .lite-switch-row{
        grid-template-columns:1fr;
        gap:6px;
    }
    .lite-switch-label{
        padding-top:0;
    }
    .lite-content-frame{
        min-height:calc(100vh - 260px);
    }
}
)CSS";
    }

    std::wstring BuildLayoutBegin(const HeaderContext& ctx)
    {
        std::wostringstream oss;

        oss << LR"HTML(<div class="lite-shell">
<header class="lite-shell-header">
<div class="lite-title-block">
<h1>)HTML" << HtmlEscape(ctx.title) << LR"HTML(</h1>
<div class="sub">)HTML" << HtmlEscape(ctx.subtitle) << LR"HTML(</div>
</div>
<div class="lite-switch-band">
)HTML";

        if (ctx.showMainMenuRow) {
            oss << RenderLinkRow(L"インデックス", ctx.mainLinks, L"top-index-row");
        }

        if (ctx.showGuideControlRow) {
            oss << RenderGuideControlRow(
                ctx.broadcastLinks,
                ctx.guideScopeLinks,
                ctx.guideViewLinks,
                ctx.timeBlockLinks);
        }

        if (ctx.showGenreRow) {
            oss << RenderLinkRow(L"ジャンル", ctx.genreLinks);
        }

        oss << LR"HTML(</div>
</header>
<main class="lite-shell-main">
<section class="lite-content-frame">
)HTML";

        return oss.str();
    }

    std::wstring BuildLayoutEnd()
    {
        return LR"HTML(</section>
</main>
</div>
)HTML";
    }

    std::wstring BuildDummyPanelHtml(const std::wstring& title, const std::wstring& description)
    {
        std::wostringstream oss;
        oss << LR"HTML(<div class="lite-dummy-panel">
<h2>)HTML" << HtmlEscape(title) << LR"HTML(</h2>
<p>)HTML" << HtmlEscape(description) << LR"HTML(</p>
</div>
)HTML";
        return oss.str();
    }
}
