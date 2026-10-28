#pragma once
#ifndef IBONDRIVER3_H
#define IBONDRIVER3_H


#include "EPGDetails.h"
#include "TunerInfo.h"
#include "EPGData.h"
#include "IBonDriver2.h"
#include <vector>  

class IBonDriver3 : public IBonDriver2 {
public:
    


    // EPG情報関連
    virtual const BOOL GetEPGData(DWORD dwSpace, DWORD dwChannel, EPGData& epgData) = 0;  // EPGデータを取得
    virtual const BOOL GetEPGDetails(DWORD dwSpace, DWORD dwChannel, EPGDetails& epgDetails) = 0;  // EPG詳細情報を取得
    virtual const BOOL SearchEPG(const LPCTSTR pszSearchTerm, std::vector<EPGDetails>& searchResults) = 0;  // EPG検索
    virtual const BOOL UpdateEPG(void) = 0;  // EPGデータを更新

    // 複数チューナー管理関連
    virtual const DWORD GetTunerCount(void) = 0;  // 使用可能なチューナーの数を取得
    virtual const BOOL GetTunerInfo(const DWORD dwTunerId, TunerInfo& tunerInfo) = 0;  // チューナー情報を取得
    virtual const BOOL SetTunerForChannel(const DWORD dwTunerId, const DWORD dwSpace, const DWORD dwChannel) = 0;  // チューナーにチャンネルを設定
    virtual const BOOL ReleaseTuner(const DWORD dwTunerId) = 0;  // チューナーを解放

    // 状態確認（保守管理）
    virtual const BOOL GetEPGStatus(int& status) = 0;  // EPGデータの取得状況を確認
    virtual const BOOL GetTunerStatus(const DWORD dwTunerId, int& status) = 0;  // チューナーの状態を確認

    // IBonDriver2 の Release 関数を継承
    virtual void Release(void) = 0;
};

#endif // IBONDRIVER3_H
