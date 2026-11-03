// A display's settings per route (route_settings.h), offline: no headset, host, or layout file.
#include "../route_settings.h"

#include <cstdio>

namespace {

int g_fail = 0, g_checks = 0;
void Check(bool ok, int line, const char *what) {
    ++g_checks;
    if (!ok) ++g_fail, std::printf("route_settings_test.cpp:%d: FAILED: %s\n", line, what);
}
#define CHECK(cond) Check((cond), __LINE__, #cond)

const nlohmann::json kLayout = nlohmann::json::parse(R"({
  "hosts": [
    {"address": "mac.local", "direct": ["192.168.0.2"], "displays": [
      {"id": "mac-desktop", "client": "frametop-mac-desktop", "fps": 60, "bitrate": 4000,
       "routes": {"direct": {"bitrate": 15000}, "network": {"fps": 30, "bitrate": 4000}}},
      {"id": "mac-plain", "client": "frametop-mac-plain", "fps": 60, "bitrate": 4000},
      {"id": "mac-odd", "client": "frametop-mac-odd",
       "routes": {"direct": {"fps": 0, "bitrate": -5}, "network": {"fps": 241, "bitrate": 500001},
                  "dongle": {"bitrate": 9000}}},
      {"id": "mac-wrong", "client": "frametop-mac-wrong",
       "routes": {"direct": {"fps": "60", "bitrate": 1.5e4}, "network": [1, 2]}},
      {"id": "mac-default", "client": "frametop-mac-default", "routes": {"direct": {"bitrate": 0}}}
    ]},
    {"address": "pc.local", "displays": [
      {"id": "pc-desktop", "client": "frametop-mac-desktop", "routes": {"direct": {"bitrate": 50000}}}
    ]}
  ]
})");

struct Got {
    bool routed;
    int fps, bitrate;
};

Got Resolve(const nlohmann::json &layout, const char *host, const char *id, const char *via) {
    Got g{false, 60, 4000};
    g.routed = RouteSettings(layout, host, id, via, &g.fps, &g.bitrate);
    return g;
}

}  // namespace

int main() {
    // Each route takes its own settings; what it leaves out stays as passed.
    Got g = Resolve(kLayout, "mac.local", "frametop-mac-desktop", "direct");
    CHECK(g.routed), CHECK(g.fps == 60), CHECK(g.bitrate == 15000);
    g = Resolve(kLayout, "mac.local", "frametop-mac-desktop", "network");
    CHECK(g.routed), CHECK(g.fps == 30), CHECK(g.bitrate == 4000);

    // No settings for the route: unchanged, and not routed.
    g = Resolve(kLayout, "mac.local", "frametop-mac-plain", "direct");
    CHECK(!g.routed), CHECK(g.fps == 60), CHECK(g.bitrate == 4000);

    // The same client id under another host is that host's display, not this one.
    g = Resolve(kLayout, "pc.local", "frametop-mac-desktop", "network");
    CHECK(!g.routed), CHECK(g.bitrate == 4000);
    g = Resolve(kLayout, "pc.local", "frametop-mac-desktop", "direct");
    CHECK(g.routed), CHECK(g.bitrate == 50000);

    // An unknown host or client changes nothing.
    g = Resolve(kLayout, "other.local", "frametop-mac-desktop", "direct");
    CHECK(!g.routed), CHECK(g.fps == 60), CHECK(g.bitrate == 4000);
    g = Resolve(kLayout, "mac.local", "frametop-nobody", "direct");
    CHECK(!g.routed), CHECK(g.bitrate == 4000);

    // Out-of-range values are ignored, at either end, and only "direct" and "network" are routes.
    g = Resolve(kLayout, "mac.local", "frametop-mac-odd", "direct");
    CHECK(g.routed), CHECK(g.fps == 60), CHECK(g.bitrate == 4000);
    g = Resolve(kLayout, "mac.local", "frametop-mac-odd", "network");
    CHECK(g.routed), CHECK(g.fps == 60), CHECK(g.bitrate == 4000);

    // Values that aren't whole numbers, and a route that isn't an object, are ignored.
    g = Resolve(kLayout, "mac.local", "frametop-mac-wrong", "direct");
    CHECK(g.routed), CHECK(g.fps == 60), CHECK(g.bitrate == 4000);
    g = Resolve(kLayout, "mac.local", "frametop-mac-wrong", "network");
    CHECK(!g.routed), CHECK(g.bitrate == 4000);

    // 0 kbit/s is kept: ft-stream then works out the default for the size and fps.
    g = Resolve(kLayout, "mac.local", "frametop-mac-default", "direct");
    CHECK(g.routed), CHECK(g.bitrate == 0);

    // A missing or broken layout changes nothing.
    g = Resolve(nlohmann::json(), "mac.local", "frametop-mac-desktop", "direct");
    CHECK(!g.routed), CHECK(g.bitrate == 4000);
    g = Resolve(nlohmann::json::parse(R"({"hosts": {}})"), "mac.local", "frametop-mac-desktop", "direct");
    CHECK(!g.routed), CHECK(g.bitrate == 4000);
    g = Resolve(nlohmann::json::parse(R"({"hosts": [{"address": "mac.local", "displays": [
        {"client": "frametop-mac-desktop", "routes": "fast"}]}]})"), "mac.local", "frametop-mac-desktop", "direct");
    CHECK(!g.routed), CHECK(g.bitrate == 4000);

    std::printf("route_settings_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
