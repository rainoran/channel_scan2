#ifndef EPG_HTML_PORTAL_SWITCH_SCRIPT_HPP
#define EPG_HTML_PORTAL_SWITCH_SCRIPT_HPP
#pragma once

#include <string>

#include "EpgHtmlPortal.hpp"

namespace EpgHtmlPortal_SwitchScript
{
    void AppendRuntimeSwitchScript(std::wstring& js);

    bool WriteRuntimeSwitchFragment(const EpgHtmlPortal::WriteOptions& opt);

    // epg_spaceX.html / epg_spaceX_before.html 用
    bool WriteProgramTableSwitchFragment(const EpgHtmlPortal::WriteOptions& opt);
}

#endif // EPG_HTML_PORTAL_SWITCH_SCRIPT_HPP