#pragma once
#ifndef EPG_HTML_LITE_RESERVATION_SWITCH_HPP
#define EPG_HTML_LITE_RESERVATION_SWITCH_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgHtmlLite.hpp"
#include "EpgHtmlLite_Common.hpp"

#include <filesystem>
#include <sstream>
#include <string>

namespace EpgHtmlLite::ReservationSwitch
{
    enum class HostKind
    {
        List = 0,
        Timetable = 1,
    };

    const wchar_t* ScriptFileName() noexcept;
    const wchar_t* ScriptSrcFromFramePage() noexcept;

    void AppendScriptTagForFramePage(
        std::wostringstream& oss);

    void AppendCss(
        std::wostringstream& oss,
        HostKind hostKind);

    void AppendProgramSwitch(
        std::wostringstream& oss,
        const ProgramEntry& entry);

    std::wstring BuildScript();

    bool WriteScriptFile(
        const std::filesystem::path& liteRootOutputDir,
        std::wstring* outPath = nullptr);

    bool WriteScriptForPageOutput(
        const std::filesystem::path& pageOutputPath,
        std::wstring* outPath = nullptr);
}

#endif // EPG_HTML_LITE_RESERVATION_SWITCH_HPP