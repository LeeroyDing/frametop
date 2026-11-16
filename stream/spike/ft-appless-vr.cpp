// ft-appless-vr: Steam Frame live VR spike for Apple High Performance Screen Sharing.
// Connects directly to macOS screensharingd (H.264 4:2:0, tilesPerFrame=1, SPS DPB 16),
// decodes on Qualcomm Iris (/dev/video-dec0 -> Q08C UBWC), converts to a 3-buffer RGBA
// UBWC ring on Adreno GLES, and presents zero-copy to SteamVR overlay frametop.appless.
#include "../iris.h"
#include "../apple_hp.h"

#include <iostream>

namespace {

bool ParseVirtualArg(const std::string &spec, uint32_t *w, uint32_t *h, double *scale) {
    const size_t x = spec.find('x');
    if (x == std::string::npos) return false;
    const size_t at = spec.find('@', x + 1);
    const std::string ws = spec.substr(0, x);
    const std::string hs = (at == std::string::npos) ? spec.substr(x + 1) : spec.substr(x + 1, at - (x + 1));
    const int wi = std::atoi(ws.c_str()), hi = std::atoi(hs.c_str());
    if (wi < 640 || hi < 360) return false;
    *w = uint32_t(wi);
    *h = uint32_t(hi);
    if (at != std::string::npos) {
        const double sc = std::atof(spec.substr(at + 1).c_str());
        if (sc < 1.0 || sc > 3.0) return false;
        *scale = sc;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    ft_apple_hp::SessionOptions opt;
    const char *dev = "/dev/video-dec0";
    bool pw_stdin = false;
    bool headlocked = false;
    double seconds = 0, distance = 1.6, width = 2.4;
    int buffers = 8;

    Decoder dec;
    dec.codec = V4L2_PIX_FMT_H264;
    dec.fourcc = V4L2_PIX_FMT_QC08C;
    dec.toSteamVR = false;

    Converter conv;
    conv.matrix = EGL_ITU_REC709_EXT;
    conv.range = EGL_YUV_FULL_RANGE_EXT;  // Apple's H.264 screen stream is full-range (yuvj420p).

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : "";
        if (a == "--host" && i + 1 < argc) opt.host = v, ++i;
        else if (a == "--port" && i + 1 < argc) opt.port = uint16_t(std::atoi(v)), ++i;
        else if (a == "--bind-ip" && i + 1 < argc) opt.bind_ip = v, ++i;
        else if (a == "--user" && i + 1 < argc) opt.username = v, ++i;
        else if (a == "--password-stdin") pw_stdin = true;
        else if (a == "--virtual" && i + 1 < argc) {
            opt.virtual_display = true;
            if (!ParseVirtualArg(v, &opt.logical_w, &opt.logical_h, &opt.hidpi_scale))
                return std::fprintf(stderr, "invalid --virtual %s\n", v), 2;
            ++i;
        }
        else if (a == "--physical" || a == "--no-curtain") opt.virtual_display = false;
        else if (a == "--no-warmup") opt.warmup_tcp = false;
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(v), ++i;
        else if (a == "--distance" && i + 1 < argc) distance = std::atof(v), ++i;
        else if (a == "--width" && i + 1 < argc) width = std::atof(v), ++i;
        else if (a == "--headlocked") headlocked = true;
        else if (a == "--device" && i + 1 < argc) dev = v, ++i;
        else if (a == "--matrix" && i + 1 < argc) {
            conv.matrix = (std::string(v) == "601") ? EGL_ITU_REC601_EXT : EGL_ITU_REC709_EXT;
            ++i;
        }
        else if (a == "--range" && i + 1 < argc) {
            conv.range = (std::string(v) == "tv") ? EGL_YUV_NARROW_RANGE_EXT : EGL_YUV_FULL_RANGE_EXT;
            ++i;
        }
        else {
            std::fprintf(stderr,
                         "usage: ft-appless-vr --host HOST --user USER (--password-stdin | FT_APPLESS_PASSWORD)\n"
                         "                     [--virtual WxH[@SCALE] | --physical] [--seconds s]\n"
                         "                     [--distance m] [--width m] [--headlocked]\n");
            return 2;
        }
    }

    if (opt.username.empty()) return std::fprintf(stderr, "missing --user USER\n"), 2;
    if (pw_stdin) {
        if (!std::getline(std::cin, opt.password)) return std::fprintf(stderr, "stdin password read failed\n"), 2;
        if (!opt.password.empty() && opt.password.back() == '\r') opt.password.pop_back();
    } else if (const char *env_pw = std::getenv("FT_APPLESS_PASSWORD")) {
        opt.password = env_pw;
    } else {
        return std::fprintf(stderr, "supply password via --password-stdin or FT_APPLESS_PASSWORD\n"), 2;
    }

    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);

    vr::EVRInitError vr_err = vr::VRInitError_None;
    vr::VR_Init(&vr_err, vr::VRApplication_Overlay);
    if (vr_err != vr::VRInitError_None) {
        OPENSSL_cleanse(opt.password.data(), opt.password.size());
        return std::fprintf(stderr, "openvr: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(vr_err)), 1;
    }
    if (!dec.Open(dev, buffers) || !conv.Init()) {
        OPENSSL_cleanse(opt.password.data(), opt.password.size());
        dec.Close();
        vr::VR_Shutdown();
        return 1;
    }

    ft_apple_hp::SessionClient client;
    std::string err;
    if (!client.Connect(opt, &err)) {
        OPENSSL_cleanse(opt.password.data(), opt.password.size());
        std::fprintf(stderr, "apple-hp connect failed: %s\n", err.c_str());
        dec.Close();
        vr::VR_Shutdown();
        return 1;
    }
    OPENSSL_cleanse(opt.password.data(), opt.password.size());

    std::printf("apple-hp live: auth=%s canvas=%ux%u tiles=%u\n",
                client.auth_used().c_str(), client.canvas().width, client.canvas().height, client.canvas().tiles);

    vr::VROverlayHandle_t ov = vr::k_ulOverlayHandleInvalid;
    if (vr::VROverlay()->CreateOverlay("frametop.appless", "Apple Screen Sharing", &ov) != vr::VROverlayError_None) {
        std::fprintf(stderr, "can't create frametop.appless overlay\n");
        client.Close();
        dec.Close();
        vr::VR_Shutdown();
        return 1;
    }
    vr::VROverlay()->SetOverlayWidthInMeters(ov, float(width));
    vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_IgnoreTextureAlpha, true);

    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
    const vr::HmdMatrix34_t head = poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
    if (headlocked) {
        vr::HmdMatrix34_t m{};
        m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
        m.m[2][3] = float(-distance);
        vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(ov, vr::k_unTrackedDeviceIndex_Hmd, &m);
    } else {
        vr::HmdMatrix34_t pose = Ahead(head, distance, 0);
        vr::VROverlay()->SetOverlayTransformAbsolute(ov, vr::TrackingUniverseStanding, &pose);
    }

    const int64_t start = MonoNs();
    int64_t last_report = start, cpu0 = CpuNs();
    uint64_t fed = 0, decoded = 0;
    int interval_fed = 0, interval_decoded = 0, stalls = 0;
    double lat_sum = 0, lat_max = 0;
    std::map<uint64_t, int64_t> queued_at;
    vr::SharedTextureHandle_t shown_tex = 0;
    vr::Texture_t tex = {&shown_tex, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    bool visible = false;

    while (!g_stop) {
        const int64_t now = MonoNs();
        if (seconds > 0 && now - start > int64_t(seconds * 1e9)) break;

        std::vector<ft_apple_hp::H264RtpAssembler::AccessUnit> aus;
        if (!client.PollStep(5, &aus, &err)) {
            std::fprintf(stderr, "apple-hp stream ended: %s\n", err.c_str());
            break;
        }
        for (const auto &au : aus) {
            if (dec.Feed(au.annex_b.data(), au.annex_b.size(), fed)) {
                queued_at[fed] = MonoNs();
                ++fed, ++interval_fed;
            } else {
                ++stalls;
                client.RequestKeyframe();
            }
        }

        if (dec.Events() && !conv.Setup(dec)) break;
        dec.Reclaim();

        uint64_t frame = 0;
        bool last = false;
        int idx;
        while ((idx = dec.Decoded(&frame, &last)) >= 0) {
            const int64_t t = MonoNs();
            auto q = queued_at.find(frame);
            if (q != queued_at.end()) {
                const double ms = (t - q->second) / 1e6;
                lat_sum += ms, lat_max = std::max(lat_max, ms);
                queued_at.erase(queued_at.begin(), std::next(q));
            }
            ++decoded, ++interval_decoded;
            shown_tex = conv.Convert(idx);
            dec.Requeue(idx);
            vr::VROverlay()->SetOverlayTexture(ov, &tex);
            if (!visible) vr::VROverlay()->ShowOverlay(ov), visible = true;
        }

        if (now - last_report >= 2'000'000'000LL) {
            const double secs = (now - last_report) / 1e9, cpu = (CpuNs() - cpu0) / 1e9;
            std::printf("%3.0f s: fed %4.1f fps, decoded %4.1f fps, decode %.1f ms avg %.1f ms worst, %llu inside, "
                        "convert %.2f ms avg %.2f ms worst, lost %llu pkts, CPU %.1f%%, stalls %d\n",
                        (now - start) / 1e9, interval_fed / secs, interval_decoded / secs,
                        interval_decoded ? lat_sum / interval_decoded : 0.0, lat_max,
                        (unsigned long long)(fed - decoded),
                        interval_decoded ? conv.msSum / interval_decoded : 0.0, conv.msMax,
                        (unsigned long long)client.assembler().lost_packets(), 100 * cpu / secs, stalls);
            std::fflush(stdout);
            conv.msSum = conv.msMax = 0;
            last_report = now, cpu0 = CpuNs();
            interval_fed = interval_decoded = stalls = 0, lat_sum = lat_max = 0;
        }
    }

    vr::VROverlay()->DestroyOverlay(ov);
    conv.Reset();
    client.Close();
    dec.Close();
    vr::VR_Shutdown();
    return 0;
}
