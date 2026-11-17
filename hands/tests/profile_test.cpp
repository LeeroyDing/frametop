// Unit tests for the hand profile (track/profile.h): what's written is what's read, a damaged or
// implausible file does no harm, and a calibration seeded from one starts settled and goes on
// learning. No models, no recordings, no cameras. make check (in the dev container) builds and
// runs it.
#include "../track/profile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace skel;

namespace {

int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++failures;   \
    } while (0)

// A hand `scale` times the model's average, open and flat, in its own palm frame: what
// Calibration::add takes (Skeleton::update hands it each clean stereo frame so).
void flat_hand(double scale, V3 local[21]) {
    const double q[kParams] = {};
    Rot frames[21];
    Skeleton::pose(Skeleton::canonical(scale), 1, Rot{}, V3{0, 0, 0}, q, local, frames);
}

void feed(Calibration &c, double scale, int frames) {
    V3 local[21];
    flat_hand(scale, local);
    bool ok[21], finger_ok[5] = {true, true, true, true, true};
    for (bool &b : ok) b = true;
    for (int i = 0; i < frames; ++i) c.add(local, ok, finger_ok);
}

double biggest_difference(const Shape &a, const Shape &b) {
    double worst = 0;
    for (int k : {1, 5, 9, 13, 17}) worst = std::max(worst, norm(a.palm[k] - b.palm[k]));
    for (int b_ = 0; b_ < 20; ++b_) worst = std::max(worst, std::fabs(a.len[b_] - b.len[b_]));
    return worst;
}

profile::Side side_of(const Calibration &c, double scale, long scale_n) {
    profile::Side s;
    c.quantities(s.q, s.n);
    s.scale.value = scale, s.scale.n = scale_n;
    return s;
}

}  // namespace

int main() {
    // --- Running: the mean of the first 45, then bounded steps
    {
        profile::Running r;
        for (int i = 0; i < 45; ++i) r.add(i % 2 ? 1.1 : 1.3, 0.0005);
        CHECK(r.settled() && std::fabs(r.value - 1.2) < 0.01);
        const double before = r.value;
        r.add(1.6, 0.0005);   // one wild reading
        CHECK(std::fabs(r.value - before - 0.0005) < 1e-12);
        for (int i = 0; i < 1000; ++i) r.add(1.0, 0.0005);   // a lasting change gets there
        CHECK(std::fabs(r.value - 1.0) < 1e-9);
        r.add(NAN, 0.0005);
        CHECK(std::isfinite(r.value) && r.n == 45 + 1 + 1000);
        profile::Running few;
        for (int i = 0; i < 10; ++i) few.add(1.25, 0.0005);
        CHECK(!few.settled() && std::fabs(few.value - 1.25) < 1e-12);
    }

    // --- a calibration learned in one run
    Calibration learned[2];
    feed(learned[0], 1.18, 60);
    feed(learned[1], 1.22, 60);
    CHECK(learned[0].settled() == 1.0 && learned[1].settled() == 1.0);
    const Shape truth_l = Skeleton::canonical(1.18), truth_r = Skeleton::canonical(1.22);
    CHECK(biggest_difference(learned[0].shape(Skeleton::canonical(1.0)), truth_l) < 1e-6);

    // --- what's written is what's read
    profile::Profile p;
    p.side[0] = side_of(learned[0], 1.1812, 5321);
    p.side[1] = side_of(learned[1], 1.2204, 6120);
    CHECK(!p.empty() && p.side[0].settled() == kQuantities);
    const std::string body = profile::text(p);
    profile::Profile back;
    CHECK(profile::parse(body, back) == 6);   // a scale, a shape and its counts, per side
    CHECK(profile::text(back) == body);
    CHECK(std::fabs(back.side[1].scale.value - 1.2204) < 1e-9 && back.side[1].scale.n == 6120);
    for (int k = 0; k < kQuantities; ++k) {
        CHECK(std::fabs(back.side[0].q[k] - p.side[0].q[k]) < 1e-6);   // to the micrometre
        CHECK(back.side[0].n[k] == p.side[0].n[k]);
    }

    // --- the next run: seeded, it starts settled, on the same hand, and goes on learning
    {
        Calibration next;
        CHECK(next.settled() == 0.0);
        next.seed(back.side[1].q, back.side[1].n);
        CHECK(next.settled() == 1.0);
        // the prior (the model's average hand) no longer shows: this is the wearer's from frame 1
        CHECK(biggest_difference(next.shape(Skeleton::canonical(1.0)), truth_r) < 1e-5);
        feed(next, 1.30, 100);   // a bigger hand: each quantity at most 0.02 mm a sample toward it
        const double moved = biggest_difference(next.shape(Skeleton::canonical(1.0)), truth_r);
        CHECK(moved > 1e-4 && moved < std::sqrt(3.0) * 100 * 2e-5 + 1e-5);   // a palm point is 3 quantities
        int n[kQuantities];
        double q[kQuantities];
        next.quantities(q, n);
        CHECK(n[20] == back.side[1].n[20] + 100);   // the count carries on from the profile's
    }
    // a profile with only some quantities settled seeds those, and the rest are learned as before
    {
        profile::Side partial = back.side[0];
        for (int k = 15; k < kQuantities; ++k) partial.n[k] = 10;
        Calibration next;
        next.seed(partial.q, partial.n);
        CHECK(next.settled() == 0.0 && next.samples(0) >= Calibration::kSettle && next.samples(20) == 0);
        feed(next, 1.18, 45);
        CHECK(next.settled() == 1.0);
    }

    // --- damage does no harm
    {
        profile::Profile q;
        CHECK(profile::parse("", q) == 0 && q.empty());
        CHECK(profile::parse("format 2\n" + body.substr(body.find("scale")), q) == 0 && q.empty());   // a newer file
        CHECK(profile::parse("scale left 1.2 100\n", q) == 0 && q.empty());                           // no format line
        // unknown lines and unknown sides are skipped; the rest is kept
        CHECK(profile::parse("format 1\ntiles slam_left 3 1 2 3\nscale middle 1.2 9\nscale left 1.25 90\n", q) == 1);
        CHECK(std::fabs(q.side[0].scale.value - 1.25) < 1e-9 && q.side[1].scale.n == 0);
        // a scale no hand has, or with no samples behind it
        CHECK(profile::parse("format 1\nscale left 2.5 100\nscale right 1.2 0\nscale right nan 5\n", q) == 0 && q.empty());
        // a shape cut short, one with a bone a metre long, and one without its counts
        const size_t at = body.find("shape left"), end = body.find('\n', at);
        const std::string shape_line = body.substr(at, end - at);
        CHECK(profile::parse("format 1\n" + shape_line.substr(0, shape_line.size() / 2) + "\n", q) == 0 && q.empty());
        std::string long_bone = shape_line;
        long_bone.replace(long_bone.rfind(' '), std::string::npos, " 1.000000");
        CHECK(profile::parse("format 1\n" + long_bone + "\n", q) == 0 && q.empty());
        CHECK(profile::parse("format 1\n" + shape_line + "\n", q) == 0 && q.side[0].settled() == 0);
        CHECK(!profile::plausible(0, 0.2) && profile::plausible(0, -0.03) && !profile::plausible(20, 0.001) &&
              !profile::plausible(20, NAN) && profile::plausible(20, 0.03));
    }

    // --- the file: none is fine, written whole and private, read back
    {
        char dir[] = "/tmp/ft-profile-test-XXXXXX";
        CHECK(mkdtemp(dir) != nullptr);
        const std::string path = std::string(dir) + "/profile";
        profile::Profile q;
        std::string err;
        CHECK(profile::load(path, q, err) && q.empty() && err.empty());   // no file yet: not an error
        CHECK(profile::save(path, p, err) && err.empty());
        struct stat st;
        CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        CHECK(access((path + ".tmp").c_str(), F_OK) != 0);
        CHECK(profile::load(path, q, err) && profile::text(q) == body);
        FILE *f = std::fopen(path.c_str(), "w");
        std::fputs("this is not a profile\n", f);
        std::fclose(f);
        CHECK(!profile::load(path, q, err) && q.empty() && !err.empty());
        CHECK(!profile::save(std::string(dir) + "/missing/profile", p, err));
        unlink(path.c_str());
        rmdir(dir);
    }

    if (failures) return std::printf("%d failure(s)\n", failures), 1;
    std::printf("profile: all checks passed\n");
    return 0;
}
