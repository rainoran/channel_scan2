#pragma once
#ifndef EIT_EXTENDED_ITEM_DEBUG_DUMP_HPP
#define EIT_EXTENDED_ITEM_DEBUG_DUMP_HPP

#include <cstdint>
#include <cstddef>
#include <string_view>

// on/off スイッチ（必要ならプロジェクト設定や他ヘッダーから上書き）
#ifndef EIT_EXTITEM_DUMP_ENABLE
#define EIT_EXTITEM_DUMP_ENABLE 1
#endif

namespace eitdebug {

#if EIT_EXTITEM_DUMP_ENABLE

    // 拡張items の「生 EIT データ」をダンプする
    void DumpExtendedItemRaw(
        uint16_t          serviceId,
        uint16_t          eventId,
        std::wstring_view label,     // 例: L"[拡張items]"
        const uint8_t* bytes,
        std::size_t       length);

    // プログラム終了時などに明示的に閉じたい場合用（必須ではない）
    void CloseExtendedItemDump();

#else

    // 無効時はダミー実装（呼んでも何もしない）
    inline void DumpExtendedItemRaw(
        uint16_t, uint16_t,
        std::wstring_view,
        const uint8_t*, std::size_t) noexcept
    {
    }
    inline void CloseExtendedItemDump() noexcept {}

#endif // EIT_EXTITEM_DUMP_ENABLE

} // namespace eitdebug

#endif // EIT_EXTENDED_ITEM_DEBUG_DUMP_HPP
