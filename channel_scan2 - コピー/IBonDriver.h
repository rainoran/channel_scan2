#ifndef IBONDRIVER_H
#define IBONDRIVER_H

#pragma once

// エラーコード定義
//#define S_OK                0   // 成功
//#define E_FAIL              1   // 一般的な失敗
//#define E_INVALID_PARAM     2   // 無効なパラメータ
//#define E_TUNER_NOT_FOUND   3   // チューナーが見つからない

class IBonDriver {
public:


    // チューナー関連
    virtual const BOOL OpenTuner(void) = 0; // チューナーオープン
    virtual void CloseTuner(void) = 0; // チューナー閉じる
    virtual const BOOL SetChannel(const BYTE bCh) = 0;// チャンネル設定
    virtual const float GetSignalLevel(void) = 0; // 信号レベル取得

    // TSストリーム関連
    virtual const DWORD WaitTsStream(const DWORD dwTimeOut = 0) = 0; // TSストリームの待機
    virtual const DWORD GetReadyCount(void) = 0; // 準備完了カウント取得
    virtual const BOOL GetTsStream(BYTE* pDst, DWORD* pdwSize, DWORD* pdwRemain) = 0; // TSストリーム取得
    virtual const BOOL GetTsStream(BYTE** ppDst, DWORD* pdwSize, DWORD* pdwRemain) = 0; // TSストリーム取得
    virtual void PurgeTsStream(void) = 0; // TSストリーム破棄

    virtual void Release(void) = 0; // 解放


};
// インスタンス生成メソッド
extern "C" __declspec(dllimport) IBonDriver* CreateBonDriver();

#endif // IBONDRIVER_H