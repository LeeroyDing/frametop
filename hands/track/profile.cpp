#include "profile.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace profile {

namespace {
const char *const kSideName[2] = {"left", "right"};
constexpr long kMaxSamples = 1'000'000'000;   // a count is only a weight: it needn't grow past this

int side_of(const std::string &word) { return word == "left" ? 0 : word == "right" ? 1 : -1; }
}  // namespace

void Running::add(double x, double step) {
    if (!std::isfinite(x)) return;
    if (n < kSettle) value += (x - value) / double(n + 1);
    else value += std::clamp(x - value, -step, step);
    n = std::min(n + 1, kMaxSamples);
}

int Side::settled() const {
    int count = 0;
    for (int k = 0; k < skel::kQuantities; ++k) count += n[k] >= skel::Calibration::kSettle;
    return count;
}

bool Profile::empty() const {
    for (const Side &s : side)
        if (s.scale.n > 0 || s.settled() > 0) return false;
    return true;
}

// The first 15 quantities are five palm points' coordinates (the knuckles and the thumb's base,
// from the wrist): within a hand's length of it. The other 15 are the fingers' bones.
bool plausible(int quantity, double metres) {
    if (!std::isfinite(metres)) return false;
    return quantity < 15 ? std::fabs(metres) < 0.15 : metres > 0.005 && metres < 0.08;
}

std::string default_path() {
    const char *state = std::getenv("XDG_STATE_HOME"), *home = std::getenv("HOME");
    std::string dir = state && *state ? state : std::string(home ? home : "") + "/.local/state";
    mkdir(dir.c_str(), 0700);
    for (const char *part : {"/frametop", "/hands"}) mkdir((dir += part).c_str(), 0700);
    return dir + "/profile";
}

std::string text(const Profile &p) {
    std::string out = "# Frametop hand profile (hands/track/profile.h): what ft-hands has learned about your hands.\n"
                      "# Numbers only. `ft-handsctl profile` shows it, `ft-handsctl profile forget` deletes it.\n"
                      "format 1\n";
    char buf[64];
    for (int s = 0; s < 2; ++s) {
        const Side &side = p.side[s];
        if (side.scale.n > 0) {
            std::snprintf(buf, sizeof buf, "scale %s %.4f %ld\n", kSideName[s], side.scale.value, side.scale.n);
            out += buf;
        }
        if (side.settled() == 0) continue;
        out += std::string("shape ") + kSideName[s];
        for (int k = 0; k < skel::kQuantities; ++k) {
            std::snprintf(buf, sizeof buf, " %.6f", side.q[k]);
            out += buf;
        }
        out += std::string("\ncount ") + kSideName[s];
        for (int k = 0; k < skel::kQuantities; ++k) out += " " + std::to_string(side.n[k]);
        out += "\n";
    }
    return out;
}

int parse(const std::string &text, Profile &out) {
    out = Profile{};
    int kept = 0, format = 0;
    bool have_shape[2] = {false, false}, have_count[2] = {false, false};
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        std::istringstream words(line);
        std::string key, which;
        if (!(words >> key) || key[0] == '#') continue;
        if (key == "format") {
            if (!(words >> format) || format != 1) return 0;   // a newer file: not ours to guess at
            continue;
        }
        if (format != 1 || !(words >> which)) continue;
        const int s = side_of(which);
        if (s < 0) continue;
        if (key == "scale") {
            double v;
            long n;
            if (!(words >> v >> n) || !std::isfinite(v) || v < kScaleMin || v > kScaleMax || n <= 0) continue;
            out.side[s].scale.value = v, out.side[s].scale.n = std::min(n, kMaxSamples);
            ++kept;
        } else if (key == "shape") {
            double q[skel::kQuantities];
            bool ok = true;
            for (int k = 0; k < skel::kQuantities && ok; ++k) ok = bool(words >> q[k]) && plausible(k, q[k]);
            if (!ok) continue;
            std::copy(q, q + skel::kQuantities, out.side[s].q);
            have_shape[s] = true;
            ++kept;
        } else if (key == "count") {
            long n[skel::kQuantities];
            bool ok = true;
            for (int k = 0; k < skel::kQuantities && ok; ++k) ok = bool(words >> n[k]) && n[k] >= 0;
            if (!ok) continue;
            for (int k = 0; k < skel::kQuantities; ++k) out.side[s].n[k] = int(std::min(n[k], kMaxSamples));
            have_count[s] = true;
            ++kept;
        }
    }
    // a shape and its counts only mean something together
    for (int s = 0; s < 2; ++s)
        if (have_shape[s] != have_count[s]) {
            std::fill(out.side[s].q, out.side[s].q + skel::kQuantities, 0.0);
            std::fill(out.side[s].n, out.side[s].n + skel::kQuantities, 0);
            --kept;
        }
    return kept;
}

bool load(const std::string &path, Profile &out, std::string &err) {
    out = Profile{};
    std::ifstream f(path);
    if (!f) {
        if (errno == ENOENT) return true;
        err = path + ": " + std::strerror(errno);
        return false;
    }
    std::stringstream all;
    all << f.rdbuf();
    if (parse(all.str(), out) == 0 && !all.str().empty()) {
        err = path + ": nothing in it this ft-hands can use";
        return false;
    }
    return true;
}

bool save(const std::string &path, const Profile &p, std::string &err) {
    const std::string tmp = path + ".tmp", body = text(p);
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        err = tmp + ": " + std::strerror(errno);
        return false;
    }
    size_t done = 0;
    while (done < body.size()) {
        const ssize_t w = write(fd, body.data() + done, body.size() - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += size_t(w);
    }
    const bool ok = done == body.size() && fsync(fd) == 0;
    if (close(fd) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        err = path + ": " + std::strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace profile
