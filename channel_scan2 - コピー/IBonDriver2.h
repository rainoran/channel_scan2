#ifndef IBONDRIVER2_H
#define IBONDRIVER2_H

#pragma once

#include "IBonDriver.h"

#include <vector>

// 構造体の前方宣言（詳細は `IBonDriver2.cpp` に記述）
struct EPGData;  // 前方宣言
struct EPGDetails;
struct NetworkInfo;
struct TsDetails;
struct TunerInfo;
struct ChannelInfo;


class IBonDriver2 : public IBonDriver {
public:
   

    // IBonDriver2 のメソッド
	virtual LPCTSTR GetTunerName(void) = 0;

	virtual const BOOL IsTunerOpening(void) = 0;

	virtual LPCTSTR EnumTuningSpace(const DWORD dwSpace) = 0;
	virtual LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) = 0;

	virtual const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) = 0;

	virtual const DWORD GetCurSpace(void) = 0;
	virtual const DWORD GetCurChannel(void) = 0;


    // IBonDriver3 のメソッド
    virtual const DWORD GetTotalDeviceNum() = 0;
    virtual const DWORD GetActiveDeviceNum() = 0;
    virtual const BOOL SetLnbPower(const BOOL bEnable) = 0;

    virtual void Release() override = 0;
};

#endif // IBONDRIVER2_H
