#include "channellist.hpp"
#include "DebugManager.h"

#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <algorithm>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{
    // ------------------------------
    // UTF-8(BOM) 出力ユーティリティ
    // ------------------------------
    std::string WideToUtf8(std::wstring_view w)
    {
        if (w.empty()) return {};

#ifdef _WIN32
        // まずは厳密変換（不正サロゲート等で失敗する可能性）
        int needed = ::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            w.data(),
            static_cast<int>(w.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (needed <= 0) {
            // フォールバック（置換を許可）
            needed = ::WideCharToMultiByte(
                CP_UTF8,
                0,
                w.data(),
                static_cast<int>(w.size()),
                nullptr,
                0,
                nullptr,
                nullptr);
            if (needed <= 0) return {};
        }

        std::string out(static_cast<std::size_t>(needed), '\0');
        const int wrote = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            w.data(),
            static_cast<int>(w.size()),
            out.data(),
            needed,
            nullptr,
            nullptr);

        if (wrote <= 0) return {};
        if (wrote != needed) out.resize(static_cast<std::size_t>(wrote));
        return out;
#else
        // 非Windows環境は想定外だが、空で返す（必要なら後で実装）
        (void)w;
        return {};
#endif
    }

    class Utf8BomWriter final
    {
    public:
        Utf8BomWriter() = default;
        Utf8BomWriter(const std::filesystem::path& path, bool append) { Open(path, append); }

        bool Open(const std::filesystem::path& path, bool append)
        {
            Close();

            // ★ BOM を入れるかどうかは「開く前」に確定する（app中に誤挿入しない）
            bool needBom = false;

            if (!append) {
                needBom = true; // truncで新規作成するので必ずBOM
            }
            else {
                // append時：新規作成 or 空ファイルの時だけBOM
                std::error_code ec;
                const bool exists = std::filesystem::exists(path, ec);
                if (!exists || ec) {
                    // exists判定が失敗した場合、BOMを“追記”して壊すのが最悪なので
                    // 安全側として「BOM無し」に寄せる（新規なら後続処理でヘッダが書かれるだけ）
                    needBom = !exists && !ec; // existsがfalseでecなしなら新規扱い
                }
                else {
                    const auto size = std::filesystem::file_size(path, ec);
                    if (!ec && size == 0) {
                        needBom = true;
                    }
                }
            }

            std::ios::openmode mode = std::ios::binary | std::ios::out;
            mode |= append ? std::ios::app : std::ios::trunc;

            m_ofs.open(path, mode);
            if (!m_ofs.is_open()) return false;

            // ★ append中にBOMを入れるのは「新規/空」だけ
            if (needBom) {
                WriteBom();
            }

            return true;
        }


        bool IsOpen() const noexcept { return m_ofs.is_open(); }

        void Write(std::wstring_view s)
        {
            if (!m_ofs.is_open()) return;
            const std::string u8 = WideToUtf8(s);
            if (!u8.empty()) m_ofs.write(u8.data(), static_cast<std::streamsize>(u8.size()));
        }

        void WriteLine(std::wstring_view s)
        {
            Write(s);
            // Windows 互換（メモ帳等）: CRLF
            m_ofs.write("\r\n", 2);
        }

        void Close()
        {
            if (m_ofs.is_open()) {
                m_ofs.flush();
                m_ofs.close();
            }
        }

        ~Utf8BomWriter() { Close(); }

    private:
        void WriteBom()
        {
            static constexpr unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
            m_ofs.write(reinterpret_cast<const char*>(kBom), 3);
        }

        std::ofstream m_ofs;
    };

    // ------------------------------
    // 文字列サニタイズ（ゼロ幅文字除去 + 制御文字抑制）
    // - U+FEFF がサービス名の先頭に混ざると「空欄」に見えるため除去
    // - CSV を壊す CR/LF/TAB/NUL は空白に置換
    // ------------------------------
    inline bool RemoveZeroWidthMarks(std::wstring& s)
    {
        const auto old = s.size();
        s.erase(std::remove_if(s.begin(), s.end(),
            [](wchar_t ch) {
                return (ch == 0xFEFF) || (ch == 0x200B) || (ch == 0x2060);
            }), s.end());
        return s.size() != old;
    }

    inline bool SanitizeCsvField(std::wstring& s)
    {
        bool changed = false;

        changed |= RemoveZeroWidthMarks(s);

        for (auto& ch : s) {
            if (ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == L'\0') {
                ch = L' ';
                changed = true;
                continue;
            }
            // 追加で “他の制御文字” も潰したい場合はここで:
            if (ch < 0x20) {
                ch = L' ';
                changed = true;
            }
        }

        return changed;
    }

    // NIT の networkName(vector<uint16_t>) → std::wstring に変換
    inline std::wstring DecodeNetworkName(const NITNetworkInfo& net)
    {
        std::wstring w;
        w.reserve(net.networkName.size());
        for (auto c : net.networkName) {
            w.push_back(static_cast<wchar_t>(c));
        }
        return w;
    }

    // ★ リモコンID順ソート用
    bool LessByRemoteId(const ChannelListEntry& a, const ChannelListEntry& b) noexcept
    {
        const int ra = a.remoteControlKeyId;
        const int rb = b.remoteControlKeyId;

        // 「リモコンIDが有効 (>0) な局」を先に並べる
        const bool aHas = (ra > 0);
        const bool bHas = (rb > 0);
        if (aHas != bHas) {
            return aHas > bHas; // true が先
        }

        // 両方とも有効な場合 → RemoteID 昇順
        if (aHas && bHas) {
            if (ra != rb) return ra < rb;
        }

        // RemoteID が同じ or 両方不明 → 物理チャンネル順で安定させる
        if (a.space != b.space)     return a.space < b.space;
        if (a.channel != b.channel) return a.channel < b.channel;

        // 同一 TS 内では serviceId 順（サブchの順番）
        return a.serviceId < b.serviceId;
    }

    // ★ 物理チャンネル順ソート用
    bool LessByPhysical(const ChannelListEntry& a, const ChannelListEntry& b) noexcept
    {
        if (a.space != b.space)     return a.space < b.space;
        if (a.channel != b.channel) return a.channel < b.channel;
        return a.serviceId < b.serviceId;
    }

    // SDT/NITの情報を利用してチャンネルリストを作成
    void AddChannelListEntryToBuffer(
        std::vector<ChannelListEntry>& allEntries,
        const std::vector<ServiceInfo>& services,
        const std::vector<NITNetworkInfo>& networks,
        int space,
        int channel)
    {
        auto partial = BuildChannelListEntries(services, networks, space, channel);
        allEntries.insert(allEntries.end(),
            std::make_move_iterator(partial.begin()),
            std::make_move_iterator(partial.end()));
    }

} // namespace

// チャンネルリストをバッファに追加
void ChannelListBuffer::AddChannelListEntry(
    const std::vector<ServiceInfo>& services,
    const std::vector<NITNetworkInfo>& networks,
    int space,
    int channel)
{
    AddChannelListEntryToBuffer(allEntries, services, networks, space, channel);
}

// 一括保存（ソート付き）
bool ChannelListBuffer::SortAndSaveChannelList(const std::wstring& filePath)
{
#if CHANNELLIST_SORT_USE_REMOTE_ID
    std::sort(allEntries.begin(), allEntries.end(), LessByRemoteId);
#else
    std::sort(allEntries.begin(), allEntries.end(), LessByPhysical);
#endif
    return AppendChannelListFile(filePath, allEntries, true);
}

// 保存（ソートなし）
bool ChannelListBuffer::SaveChannelListToFile(const std::wstring& filePath)
{
    return AppendChannelListFile(filePath, allEntries, true);
}

// space ごとに channellist_spaceX.ch を返す
inline std::wstring GetChannelListFileName(int space)
{
    return L"channellist_space" + std::to_wstring(space) + L".ch";
}

// 引数なし版（デフォルトで地上波 space=0 を返す）
std::wstring GetChannelListFileName()
{
    return GetChannelListFileName(0);
}

void InitializeChannelListFile(const std::wstring& filename)
{
    namespace fs = std::filesystem;

    try {
        fs::path p(filename);
        if (fs::exists(p)) {
            fs::remove(p);
            DebugLog(L"[INFO] InitializeChannelListFile: remove old file: %ls",
                filename.c_str());
        }
        else {
            DebugLog(L"[INFO] InitializeChannelListFile: no existing file: %ls",
                filename.c_str());
        }
    }
    catch (const std::exception& e) {
        DebugLog(L"[WARN] InitializeChannelListFile: exception: %hs", e.what());
    }
}

std::vector<ChannelListEntry>
BuildChannelListEntries(
    const std::vector<ServiceInfo>& services,
    const std::vector<NITNetworkInfo>& networks,
    int space,
    int channel)
{
    std::vector<ChannelListEntry> result;
    result.reserve(services.size());

    // --- NIT: NetworkID → NetworkName のマップを作る ---
    std::unordered_map<uint16_t, std::wstring> networkNameMap;
    for (const auto& net : networks) {
        networkNameMap[net.networkId] = DecodeNetworkName(net);
    }

    // --- SDT 側のサービス1つずつに対して、NIT 情報をひもづけ ---
    for (const auto& s : services) {
        ChannelListEntry e{};

        e.space = space;
        e.channel = channel;

        // SDT 由来
        e.serviceId = s.service_id;
        e.serviceType = s.service_type;
        e.transportStreamId = s.transport_stream_id;
        e.originalNetworkId = s.original_network_id;
        e.freeCAMode = s.free_CA_mode;
        e.serviceName = s.service_name;
        e.providerName = s.provider_name;

        // --- NIT 由来 (networkId / networkName) ---
        uint16_t nid = s.original_network_id;
        std::wstring nname;

        auto it = networkNameMap.find(nid);
        if (it != networkNameMap.end()) {
            nname = it->second;
        }
        else if (!networkNameMap.empty()) {
            nid = networkNameMap.begin()->first;
            nname = networkNameMap.begin()->second;
        }

        e.networkId = nid;
        e.networkName = std::move(nname);

        // --- NIT(TS情報記述子)からリモコンIDを取得 ---
        int remoteKey = -1;
        for (const auto& net : networks) {
            for (const auto& ts : net.transportStreams) {
                if (ts.transportStreamId == s.transport_stream_id &&
                    ts.originalNetworkId == s.original_network_id)
                {
                    if (ts.remoteControlKeyId >= 0) {
                        remoteKey = ts.remoteControlKeyId;
                    }
                    break;
                }
            }
            if (remoteKey >= 0) break;
        }
        e.remoteControlKeyId = remoteKey;

        result.push_back(std::move(e));
    }

#if CHANNELLIST_SORT_USE_REMOTE_ID
    std::sort(result.begin(), result.end(), LessByRemoteId);
#else
    std::sort(result.begin(), result.end(), LessByPhysical);
#endif

    return result;
}

bool AppendChannelListFile(
    const std::wstring& filePath,
    const std::vector<ChannelListEntry>& entries,
    bool writeHeaderIfEmpty)
{
    namespace fs = std::filesystem;

    const fs::path p(filePath);

    bool needHeader = false;
    if (writeHeaderIfEmpty) {
        std::error_code ec;
        const bool exists = fs::exists(p, ec);
        std::uintmax_t size = 0;
        if (exists) {
            size = fs::file_size(p, ec);
        }
        if (!exists || size == 0) {
            needHeader = true;
        }
    }

    // ★変更点：wofstream → UTF-8(BOM) + ofstream(binary)
    Utf8BomWriter ofs(p, /*append=*/true);
    if (!ofs.IsOpen()) {
        return false;
    }

    // ヘッダ行（ファイルが新規のときだけ）
    if (needHeader) {
        ofs.WriteLine(L"; channel_scan2 channel list");
        ofs.WriteLine(L"; ServiceName,Space,Channel,ServiceType,ServiceID,"
            L"NetworkID,TSID,ONID,FreeCA,Provider,NetworkName,RemoteControlKeyID");
    }

    // 本体（行生成ロジックは従来通り。ただし書き込み前にサニタイズ）
    for (const auto& e : entries) {

        // ★ここでデータを壊さずに最低限のサニタイズ（U+FEFF等を除去）
        std::wstring serviceName = e.serviceName;
        std::wstring providerName = e.providerName;
        std::wstring networkName = e.networkName;

        (void)SanitizeCsvField(serviceName);
        (void)SanitizeCsvField(providerName);
        (void)SanitizeCsvField(networkName);

        std::wstring line;
        line.reserve(256);

        line += serviceName; line += L",";
        line += std::to_wstring(e.space); line += L",";
        line += std::to_wstring(e.channel); line += L",";
        line += std::to_wstring(static_cast<unsigned>(e.serviceType)); line += L",";
        line += std::to_wstring(e.serviceId); line += L",";
        line += std::to_wstring(e.networkId); line += L",";
        line += std::to_wstring(e.transportStreamId); line += L",";
        line += std::to_wstring(e.originalNetworkId); line += L",";
        line += (e.freeCAMode ? L"1" : L"0"); line += L",";
        line += providerName; line += L",";
        line += networkName; line += L",";
        line += std::to_wstring(e.remoteControlKeyId >= 0 ? e.remoteControlKeyId : 0);

        ofs.WriteLine(line);
    }

    return true;
}
