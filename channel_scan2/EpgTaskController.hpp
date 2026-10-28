#pragma once
#pragma once
#ifndef EPG_TASK_CONTROLLER_HPP
#define EPG_TASK_CONTROLLER_HPP

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <set>
#include <fstream>

#include "EpgBuild_GUI.hpp"
#include "IBonDriver2.h"

// タブへ渡す GuiActions を組み立てる役
class EpgTaskController
{
public:
    EpgTaskController() = default;

    // 危険回避：明示停止（終了時に必ず呼ぶ）
    void Reset() noexcept
    {
        RequestCancel();
        JoinIfRunning();
        running_.store(false, std::memory_order_release);
        building_.store(false, std::memory_order_release);
        builder_.ResetCancel();
    }

    void RequestCancel() noexcept
    {
        builder_.RequestCancel();
    }

    bool IsRunning() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    // BuildEpg を起動（多重起動防止）
    void StartBuildAsync(
        IBonDriver2* bon,
        FilterManagerMode mode,
        int space,
        std::function<void(std::wstring_view)> log,
        std::wstring configPath = L"config.ini")
    {
        if (!bon) {
            if (log) log(L"[EPG][ERROR] BonDriver が無効です。");
            return;
        }
        if (running_.exchange(true, std::memory_order_acq_rel)) {
            if (log) log(L"[EPG][WARN] 既にEPG構築が実行中です。");
            return;
        }

        // 以前のキャンセル状態を解除
        builder_.ResetCancel();
        building_.store(true, std::memory_order_release);

        // オプション生成
        EpgBuildGUI::Options opt;
        opt.space = space;

        // config.ini の [EpgServiceEnable_spaceX] を見て、enable=1 の「物理ch」だけ対象にする（空なら channellist から自動）
        opt.selectedPhysicalChannels = LoadEnabledPhysicalChannelsFromConfig(configPath, space);

        // ログコールバック
        EpgBuildGUI::Callbacks cb;
        cb.onLog = [log](std::wstring_view s) { if (log) log(s); };

        // 進捗（必要なら GUI 側で表示）
        cb.onOverallProgress = [log](std::size_t done, std::size_t total) {
            if (!log) return;
            std::wstring msg = L"[EPG] progress " + std::to_wstring(done) + L"/" + std::to_wstring(total);
            log(msg);
            };

        // 起動
        worker_ = std::thread([this, bon, mode, opt, cb, log]() mutable {
            try {
                const auto res = builder_.Run(bon, mode, opt, &cb);

                if (log) {
                    if (res.IsOk()) {
                        std::wstring msg = L"[EPG] done: merged=" + res.mergedEpgPath + L" html=" + res.htmlPath;
                        log(msg);
                    }
                    else if (res.IsCancelled()) {
                        log(L"[EPG] cancelled");
                    }
                    else {
                        std::wstring msg = L"[EPG][ERROR] failed status=" + std::to_wstring((unsigned)res.status);
                        log(msg);
                    }
                }
            }
            catch (...) {
                if (log) log(L"[EPG][ERROR] exception swallowed");
            }

            building_.store(false, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            });
    }

    void JoinIfRunning() noexcept
    {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    // config.ini から enable=1 の物理chを抽出（失敗/空なら {} を返す）
    static std::vector<int> LoadEnabledPhysicalChannelsFromConfig(const std::wstring& configPath, int space)
    {
        std::ifstream ifs(std::filesystem::path(configPath), std::ios::binary);
        if (!ifs) return {};

        std::string all;
        ifs.seekg(0, std::ios::end);
        const auto len = ifs.tellg();
        if (len <= 0) return {};
        all.resize((size_t)len);
        ifs.seekg(0, std::ios::beg);
        ifs.read(all.data(), len);

        const std::string sec = "[EpgServiceEnable_space" + std::to_string(space) + "]";
        const auto pos = all.find(sec);
        if (pos == std::string::npos) return {};

        // 次のセクション開始までを切り出す
        const auto bodyStart = all.find('\n', pos);
        if (bodyStart == std::string::npos) return {};
        auto bodyEnd = all.find("\n[", bodyStart);
        if (bodyEnd == std::string::npos) bodyEnd = all.size();

        std::set<int> uniqCh;

        size_t cur = bodyStart + 1;
        while (cur < bodyEnd) {
            auto eol = all.find('\n', cur);
            if (eol == std::string::npos || eol > bodyEnd) eol = bodyEnd;

            std::string line = all.substr(cur, eol - cur);
            cur = eol + 1;

            // 空行/コメント
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;

            // key=value 形式
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string val = line.substr(eq + 1);
            if (val.empty()) continue;

            // val: enable,"ServiceName",space,channel,...
            // enable
            size_t p = 0;
            auto readInt = [&](int& out) -> bool {
                while (p < val.size() && (val[p] == ' ' || val[p] == '\t')) ++p;
                bool neg = false;
                if (p < val.size() && val[p] == '-') { neg = true; ++p; }
                if (p >= val.size() || (val[p] < '0' || val[p] > '9')) return false;
                int x = 0;
                while (p < val.size() && val[p] >= '0' && val[p] <= '9') {
                    x = x * 10 + (val[p] - '0');
                    ++p;
                }
                out = neg ? -x : x;
                return true;
                };
            auto skipComma = [&]() {
                while (p < val.size() && val[p] != ',') ++p;
                if (p < val.size() && val[p] == ',') ++p;
                };
            auto skipField = [&]() {
                while (p < val.size() && (val[p] == ' ' || val[p] == '\t')) ++p;
                if (p < val.size() && val[p] == '"') {
                    ++p;
                    while (p < val.size() && val[p] != '"') ++p;
                    if (p < val.size() && val[p] == '"') ++p;
                }
                else {
                    while (p < val.size() && val[p] != ',') ++p;
                }
                if (p < val.size() && val[p] == ',') ++p;
                };

            int enable = 0;
            if (!readInt(enable)) continue;
            skipComma();      // 次（サービス名）
            skipField();      // サービス名スキップ（引用符対応）

            int sp = -1;
            int ch = -1;
            if (!readInt(sp)) continue;
            skipComma();
            if (!readInt(ch)) continue;

            if (enable == 1 && sp == space && ch >= 0) {
                uniqCh.insert(ch);
            }
        }

        std::vector<int> out;
        out.assign(uniqCh.begin(), uniqCh.end());
        return out;
    }

private:
    std::atomic_bool running_{ false };
    std::atomic_bool building_{ false };

    EpgBuildGUI::EpgBuild_GUI builder_{};
    std::thread worker_{};
};

#endif // EPG_TASK_CONTROLLER_HPP
