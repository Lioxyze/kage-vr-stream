// kage-bitrate-replay: replays the headset reports logged in driver.log ("client: ..."
// lines) through the previous and the current bitrate controller, per streaming session,
// and compares cuts, lowest and mean rate. Usage: kage-bitrate-replay [driver.log]
// Limits: the log keeps ~1 report/s (+ every congested one) and not the PC's sent-frame
// count, approximated by the last "video: N fps" line.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include "bitrate_controller.hpp"

using kage::driver::BitrateController;

namespace {

// The controller as it was before (driver.cpp up to 2026-11-09), for comparison.
class Legacy {
public:
    explicit Legacy(uint32_t maxKbps, uint32_t start) : max_(maxKbps), br_(std::min(start, maxKbps)) {}
    uint32_t update(const BitrateController::Report& r) {
        const uint64_t now = r.nowUs;
        const uint64_t dt = last_ ? std::clamp<uint64_t>(now - last_, 100'000, 2'000'000) : 1'000'000;
        last_ = now;
        if (!first_) first_ = now;
        const double dts = dt / 1e6;
        const uint32_t net = r.networkUsAvg;
        if (net && (best_ == 0 || net < best_)) best_ = net;
        if (best_) best_ += static_cast<uint32_t>(50 * dts);
        delay_ = (net && best_ && net > best_ + 10000) ? delay_ + dt : 0;
        const bool bigDelay = net && best_ && net > best_ + 25000;
        const bool loss = r.framesDropped > 0 || r.framesReceived + std::max(2.0, 5 * dts) < r.framesSent;
        const bool congested = loss || delay_ >= 2'000'000 || bigDelay;
        if (now - first_ < 5'000'000) return br_;
        if (congested) {
            if (now - cut_ >= 400'000) {
                const uint32_t floor = loss ? 10'000 : std::min<uint32_t>(50'000, br_);
                br_ = std::max<uint32_t>(floor, br_ * 3 / 4);
                cut_ = now;
            }
            hold_ = now + 2'000'000;
        } else if (now >= hold_) {
            br_ = std::min<uint32_t>(max_, br_ + static_cast<uint32_t>(br_ * 0.10 * dts));
        }
        return br_;
    }

private:
    uint32_t max_, br_, best_ = 0;
    uint64_t last_ = 0, first_ = 0, delay_ = 0, cut_ = 0, hold_ = 0;
};

struct Summary {
    int cuts = 0;
    uint32_t minKbps = ~0u;
    double sumKbpsSec = 0, secs = 0;
    uint32_t prev = 0;
    void add(uint32_t kbps, double dts) {
        if (prev && kbps < prev) ++cuts;
        prev = kbps;
        minKbps = std::min(minKbps, kbps);
        sumKbpsSec += kbps * dts;
        secs += dts;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : std::string(std::getenv("LOCALAPPDATA") ? std::getenv("LOCALAPPDATA") : "") + "\\KageVR\\driver.log";
    std::ifstream in(path);
    if (!in) {
        std::printf("introuvable : %s\n", path.c_str());
        return 1;
    }
    const std::regex client(R"((\d+):(\d+):(\d+)\.(\d+) \[INF\] client: (\d+) fps, (\d+) lost, decode [\d.]+ ms, transit ([\d.]+) ms.*-> (\d+) Mbit/s)");
    const std::regex video(R"(\[INF\] video: ([\d.]+) fps)");
    const std::regex session(R"(\[INF\] stream: encrypted session with ([\d.]+))");
    std::string line;
    struct Sess {
        std::string where;
        std::vector<std::pair<uint64_t, BitrateController::Report>> reports;  // + logged rate
        std::vector<uint32_t> logged;
    };
    std::vector<Sess> sessions;
    double sentFps = 60;
    uint64_t dayOffset = 0, lastT = 0;
    std::smatch m;
    while (std::getline(in, line)) {
        if (std::regex_search(line, m, session)) {
            sessions.push_back({m[1].str() + (m[1].str().rfind("127.", 0) == 0 ? " (USB)" : " (Wi-Fi)"), {}, {}});
            continue;
        }
        if (std::regex_search(line, m, video)) {
            sentFps = std::stod(m[1].str());
            continue;
        }
        if (sessions.empty() || !std::regex_search(line, m, client)) continue;
        uint64_t t = ((std::stoull(m[1]) * 60 + std::stoull(m[2])) * 60 + std::stoull(m[3])) * 1'000'000 + std::stoull(m[4]) * 1000;
        t += dayOffset;
        if (lastT && t < lastT) {  // midnight
            dayOffset += 86'400'000'000ull;
            t += 86'400'000'000ull;
        }
        auto& s = sessions.back();
        const double dts = s.reports.empty() ? 1.0 : std::clamp((t - s.reports.back().first) / 1e6, 0.1, 2.0);
        lastT = t;
        BitrateController::Report r;
        r.nowUs = t;
        r.framesReceived = static_cast<uint32_t>(std::stoul(m[5]) * dts + 0.5);
        r.framesDropped = static_cast<uint32_t>(std::stoul(m[6]));
        r.framesSent = static_cast<uint32_t>(sentFps * dts + 0.5);
        r.networkUsAvg = static_cast<uint32_t>(std::stod(m[7]) * 1000);
        s.reports.push_back({t, r});
        s.logged.push_back(static_cast<uint32_t>(std::stoul(m[8]) * 1000));
    }
    std::printf("%-26s %6s | %-27s | %-27s\n", "session", "durée", "ancien: coupes min moy", "nouveau: coupes min moy");
    Summary allOld, allNew;
    for (auto& s : sessions) {
        if (s.reports.size() < 20) continue;
        const uint32_t start = s.logged.front();
        Legacy old(200'000, start);
        BitrateController now(200'000, start);
        Summary a, b;
        uint64_t prevT = 0;
        for (auto& [t, r] : s.reports) {
            const double dts = prevT ? std::clamp((t - prevT) / 1e6, 0.1, 2.0) : 1.0;
            prevT = t;
            a.add(old.update(r), dts);
            b.add(now.update(r), dts);
        }
        for (auto* p : {&a, &b}) {
            auto& all = p == &a ? allOld : allNew;
            all.cuts += p->cuts;
            all.minKbps = std::min(all.minKbps, p->minKbps);
            all.sumKbpsSec += p->sumKbpsSec;
            all.secs += p->secs;
        }
        std::printf("%-26s %5.0fs | %6d %6u %8.1f Mb | %6d %6u %8.1f Mb\n", s.where.c_str(), a.secs, a.cuts, a.minKbps / 1000,
                    a.sumKbpsSec / a.secs / 1000, b.cuts, b.minKbps / 1000, b.sumKbpsSec / b.secs / 1000);
    }
    if (allOld.secs > 0)
        std::printf("TOTAL %25.0fs | %6d %6u %8.1f Mb | %6d %6u %8.1f Mb\n", allOld.secs, allOld.cuts, allOld.minKbps / 1000,
                    allOld.sumKbpsSec / allOld.secs / 1000, allNew.cuts, allNew.minKbps / 1000,
                    allNew.sumKbpsSec / allNew.secs / 1000);
    return 0;
}
