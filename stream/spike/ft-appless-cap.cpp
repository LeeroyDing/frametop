// ft-appless-cap: portable macOS/Linux spike for Apple High Performance Screen Sharing.
// Connects to macOS screensharingd (TCP 5900 + SRTP/UDP 5900-5901), negotiates single-tile
// H.264 4:2:0 (with patched SPS max_num_ref_frames=16), logs live stream telemetry, and
// optionally writes a clean Annex-B .h264 capture for ffprobe/ffmpeg or ft-dectest --h264.
//
// Passwords are read ONLY from --password-stdin or FT_APPLESS_PASSWORD, never argv.
#include "../apple_hp.h"

#include <csignal>
#include <iostream>

namespace {

volatile sig_atomic_t g_stop = 0;
void OnSignal(int) { g_stop = 1; }

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

void PrintUsage() {
    std::fprintf(stderr,
                 "usage: ft-appless-cap --host HOST [--port 5900] [--bind-ip IP] [--udp-port P]\n"
                 "                      [--probe-only | --user USER (--password-stdin | FT_APPLESS_PASSWORD)]\n"
                 "                      [--auth auto|srp|nonsrp] [--virtual WxH[@SCALE] | --physical]\n"
                 "                      [--strip-cursor] [--no-warmup] [--seconds 5] [--out CLIP.h264]\n");
}

}  // namespace

int main(int argc, char **argv) {
    ft_apple_hp::SessionOptions opt;
    bool probe_only = false;
    bool pw_stdin = false;
    double seconds = 5.0;
    std::string out_path;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : "";
        if (a == "--help" || a == "-h") {
            PrintUsage();
            return 0;
        } else if (a == "--host" && i + 1 < argc) {
            opt.host = v, ++i;
        } else if (a == "--port" && i + 1 < argc) {
            opt.port = uint16_t(std::atoi(v)), ++i;
        } else if (a == "--bind-ip" && i + 1 < argc) {
            opt.bind_ip = v, ++i;
        } else if (a == "--udp-port" && i + 1 < argc) {
            opt.udp_ctrl_port = uint16_t(std::atoi(v));
            opt.udp_video_port = uint16_t(opt.udp_ctrl_port + 1);
            ++i;
        } else if (a == "--probe-only") {
            probe_only = true;
        } else if (a == "--user" && i + 1 < argc) {
            opt.username = v, ++i;
        } else if (a == "--password-stdin") {
            pw_stdin = true;
        } else if (a == "--auth" && i + 1 < argc) {
            const std::string m = v;
            ++i;
            if (m == "srp") opt.auth_mode = ft_apple_hp::AuthMode::kSrp;
            else if (m == "nonsrp") opt.auth_mode = ft_apple_hp::AuthMode::kNonSrp;
            else if (m == "auto") opt.auth_mode = ft_apple_hp::AuthMode::kAuto;
            else return std::fprintf(stderr, "unknown --auth %s\n", m.c_str()), 2;
        } else if (a == "--virtual" && i + 1 < argc) {
            opt.virtual_display = true;
            if (!ParseVirtualArg(v, &opt.logical_w, &opt.logical_h, &opt.hidpi_scale)) {
                return std::fprintf(stderr, "invalid --virtual %s (expected WxH or WxH@SCALE)\n", v), 2;
            }
            ++i;
        } else if (a == "--physical" || a == "--no-curtain") {
            opt.virtual_display = false;
        } else if (a == "--strip-cursor") {
            opt.bake_cursor = false;
        } else if (a == "--no-warmup") {
            opt.warmup_tcp = false;
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atof(v), ++i;
        } else if (a == "--out" && i + 1 < argc) {
            out_path = v, ++i;
        } else {
            PrintUsage();
            return 2;
        }
    }

    if (probe_only) {
        ft_apple_hp::ProbeInfo info{};
        std::string err;
        if (!ft_apple_hp::ProbeServer(opt.host, opt.port, opt.bind_ip, &info, &err)) {
            std::fprintf(stderr, "probe failed (%s:%u): %s\n", opt.host.c_str(), opt.port, err.c_str());
            return 1;
        }
        std::string banner_trim = info.banner;
        while (!banner_trim.empty() && (banner_trim.back() == '\n' || banner_trim.back() == '\r'))
            banner_trim.pop_back();
        std::printf("probe ok: host=%s:%u banner=\"%s\" security_types=[", opt.host.c_str(), opt.port,
                    banner_trim.c_str());
        for (size_t i = 0; i < info.security_types.size(); ++i) {
            std::printf("%s%u", i ? "," : "", unsigned(info.security_types[i]));
        }
        std::printf("] rsa_pubkey_bits=%d\n", info.rsa_key_bits);
        return 0;
    }

    if (opt.username.empty()) {
        std::fprintf(stderr, "missing --user USER (or pass --probe-only)\n");
        return 2;
    }
    if (pw_stdin) {
        if (!std::getline(std::cin, opt.password)) {
            std::fprintf(stderr, "failed to read password from stdin\n");
            return 2;
        }
        if (!opt.password.empty() && opt.password.back() == '\r') opt.password.pop_back();
    } else if (const char *env_pw = std::getenv("FT_APPLESS_PASSWORD")) {
        opt.password = env_pw;
    } else {
        std::fprintf(stderr, "supply password via --password-stdin or FT_APPLESS_PASSWORD\n");
        return 2;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    FILE *out_fp = nullptr;
    if (!out_path.empty()) {
        out_fp = std::fopen(out_path.c_str(), "wb");
        if (!out_fp) {
            std::perror(out_path.c_str());
            return 1;
        }
    }

    ft_apple_hp::SessionClient client;
    std::string err;
    if (!client.Connect(opt, &err)) {
        OPENSSL_cleanse(opt.password.data(), opt.password.size());
        if (out_fp) std::fclose(out_fp);
        std::fprintf(stderr, "connect failed: %s\n", err.c_str());
        return 1;
    }
    OPENSSL_cleanse(opt.password.data(), opt.password.size());

    std::printf("connected: auth=%s server_init=%ux%u layout=%ux%u(backing %ux%u) 0x1c_canvas=%ux%u tiles=%u\n",
                client.auth_used().c_str(), client.server_init_w(), client.server_init_h(),
                client.layout().scaled_w, client.layout().scaled_h,
                client.layout().backing_w, client.layout().backing_h,
                client.canvas().width, client.canvas().height, client.canvas().tiles);
    std::fflush(stdout);

    const int64_t t_start = ft_apple_hp::NowNs();
    int64_t t_report = t_start;
    uint64_t total_aus = 0, total_keys = 0, total_au_bytes = 0;
    uint64_t win_aus = 0, win_keys = 0, win_bytes = 0;
    bool logged_sps = false;

    while (!g_stop) {
        const int64_t now = ft_apple_hp::NowNs();
        if (seconds > 0 && (now - t_start) >= int64_t(seconds * 1e9)) break;

        std::vector<ft_apple_hp::H264RtpAssembler::AccessUnit> aus;
        if (!client.PollStep(20, &aus, &err)) {
            std::fprintf(stderr, "session ended: %s\n", err.c_str());
            break;
        }

        if (!logged_sps && client.assembler().has_config()) {
            const auto &s = client.assembler().raw_sps_info();
            std::printf("avcC config: profile=%u level=%u->60 max_ref_frames=%u->16 sps_dims=%ux%u\n",
                        s.profile_idc, s.level_idc, s.max_num_ref_frames, s.width, s.height);
            std::fflush(stdout);
            logged_sps = true;
        }

        for (const auto &au : aus) {
            ++total_aus, ++win_aus;
            if (au.is_keyframe) ++total_keys, ++win_keys;
            total_au_bytes += au.annex_b.size();
            win_bytes += au.annex_b.size();
            if (out_fp) std::fwrite(au.annex_b.data(), 1, au.annex_b.size(), out_fp);
        }

        if (now - t_report >= 1'000'000'000LL) {
            const double dt = (now - t_report) / 1e9;
            std::printf("%4.1f s: %5.1f fps (%llu key), %6.0f kbit/s, rtp_pkts=%llu lost=%llu\n",
                        (now - t_start) / 1e9, win_aus / dt, (unsigned long long)win_keys,
                        (win_bytes * 8.0 / 1000.0) / dt,
                        (unsigned long long)client.video_rtp_packets(),
                        (unsigned long long)client.assembler().lost_packets());
            std::fflush(stdout);
            t_report = now;
            win_aus = win_keys = win_bytes = 0;
        }
    }

    if (out_fp) std::fclose(out_fp);
    client.Close();
    std::printf("summary: %llu access units (%llu keyframes), %llu bytes, %llu lost packets\n",
                (unsigned long long)total_aus, (unsigned long long)total_keys,
                (unsigned long long)total_au_bytes,
                (unsigned long long)client.assembler().lost_packets());
    return total_aus > 0 ? 0 : 1;
}
