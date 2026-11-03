// A remote display's stream settings for the way its stream goes (Route in ft-stream.cpp).
// Its entry in ~/.config/frametop-layout.json can give fps and bitrate per route:
//   "routes": {"direct": {"fps": 60, "bitrate": 15000}, "network": {"bitrate": 4000}}
// "direct" is a stream through one of the host's direct addresses (its dongle, or a second link
// such as its Ethernet), "network" one through its own address. What a route leaves out stays as
// ft-screens passed it, and bitrate 0 is the default for the size and fps (main). A value out of
// range is ignored. ft-layout's "remote set ID direct.bitrate=KBPS" writes them.
#pragma once

#include <nlohmann/json.hpp>

#include <string>

constexpr int kRouteFpsMax = 240, kRouteBitrateMax = 500000;  // kbit/s; ft-layout checks the same

// Applies the route's settings to *fps and *bitrate. True if the display has settings for it.
inline bool RouteSettings(const nlohmann::json &layout, const std::string &host, const std::string &id,
                          const std::string &via, int *fps, int *bitrate) {
    if (!layout.is_object() || !layout.contains("hosts") || !layout["hosts"].is_array()) return false;
    for (const auto &h : layout["hosts"]) {
        if (!h.is_object() || h.value("address", "") != host || !h.contains("displays") || !h["displays"].is_array())
            continue;
        for (const auto &d : h["displays"]) {
            if (!d.is_object() || d.value("client", "") != id) continue;
            if (!d.contains("routes") || !d["routes"].is_object() || !d["routes"].contains(via)) return false;
            const nlohmann::json &r = d["routes"][via];
            if (!r.is_object()) return false;
            const auto take = [&r](const char *key, long long lo, long long hi, int *out) {
                if (!r.contains(key) || !r[key].is_number_integer()) return;
                const long long v = r[key].get<long long>();
                if (v >= lo && v <= hi) *out = int(v);
            };
            take("fps", 1, kRouteFpsMax, fps);
            take("bitrate", 0, kRouteBitrateMax, bitrate);
            return true;
        }
    }
    return false;
}
