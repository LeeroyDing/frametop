// Unit tests for the hand profile (track/profile.h): what's written is what's read, a damaged or
// implausible file does no harm, and a calibration seeded from one starts settled and goes on
// learning. No models, no recordings, no cameras. make check (in the dev container) builds and
// runs it.
#include "../track/profile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
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

    // --- the search's tiles: more where hands turn up, never none
    {
        std::map<std::string, std::vector<profile::TileStat>> tiles;
        CHECK(profile::mean_rate(tiles) == 0 && profile::tile_boost({}, 0) == 1);   // nothing found yet: as before
        tiles["slam_left"] = {{400, 80}, {400, 4}, {0, 0}, {10, 1}};
        tiles["upper_left"] = {{200, 16}};
        const double mean = profile::mean_rate(tiles);
        CHECK(std::fabs(mean - 101.0 / 1010.0) < 1e-12);
        const double hot = profile::tile_boost(tiles["slam_left"][0], mean), cold = profile::tile_boost(tiles["slam_left"][1], mean);
        CHECK(hot > 1.8 && hot < 2.0);                                       // 20% against 10%, pulled a little to the mean
        CHECK(cold == profile::kTileBoostMin);                               // 1%: half as often, no less
        CHECK(profile::tile_boost(tiles["slam_left"][2], mean) == 1.0);      // never tried: its fixed weight
        CHECK(std::fabs(profile::tile_boost(tiles["slam_left"][3], mean) - 1.0) < 0.05);   // hardly tried: nearly
        CHECK(profile::tile_boost({50, 50}, mean) == profile::kTileBoostMax);              // every try: 4 times, no more
        profile::forget_old(tiles);                                          // 1010 tries: nothing forgotten yet
        CHECK(tiles["slam_left"][0].tries == 400);
        tiles["upper_left"][0].tries = 30000;
        profile::forget_old(tiles);
        CHECK(tiles["slam_left"][0].tries == 200 && tiles["slam_left"][0].finds == 40 && tiles["upper_left"][0].tries == 15000);
        // written and read with the rest
        profile::Profile with = p, read;
        with.tiles = tiles;
        const std::string t = profile::text(with);
        CHECK(profile::parse(t, read) == 8 && profile::text(read) == t);
        CHECK(read.tiles["slam_left"].size() == 4 && read.tiles["slam_left"][0].finds == 40 && read.tiles.count("upper_left"));
        profile::Profile only;
        only.tiles["slam_right"] = {{3, 1}};
        CHECK(!only.empty());
        // a line that doesn't add up is left out: fewer numbers than it says, more finds than tries, no tiles
        CHECK(profile::parse("format 1\ntiles slam_left 3 10 1 10 2\ntiles slam_right 1 5 9\ntiles upper_left 0\n", read) == 0);
        CHECK(read.empty());
    }

    // --- the wearer's pinches: counted, and the detector's two distances moved to them
    {
        using profile::PinchStat;
        double bins[profile::kPinchBins] = {};
        CHECK(profile::quantile(bins, 0.5) == -1);
        bins[10] = 50, bins[20] = 30, bins[30] = 20;   // 10-11 mm, 20-21 mm, 30-31 mm
        CHECK(profile::quantile(bins, 0.5) == 0.011 && profile::quantile(bins, 0.8) == 0.021 && profile::quantile(bins, 0.97) == 0.031);
        const double begin = 0.020, dip = 0.032;
        auto same = [&](const PinchStat &s) {
            const profile::PinchLearned l = profile::learned_pinch(s, begin, dip);
            return l.begin_m == begin && l.dip_max_m == dip;
        };
        auto fill = [](double *b, int from_mm, int to_mm, double each) {
            for (int mm = from_mm; mm < to_mm; ++mm) b[mm] += each;
        };
        PinchStat firm;                          // taps that close well: 8 to 16 mm
        fill(firm.taps, 8, 16, 5);               // 40 taps: too few to go by
        CHECK(same(firm));
        fill(firm.taps, 8, 16, 20);              // 200
        fill(firm.idle, 40, 50, 500);            // an idle hand that stays open
        profile::PinchLearned l = profile::learned_pinch(firm, begin, dip);
        CHECK(l.begin_m == begin);               // nothing to gain: it stays
        CHECK(l.dip_max_m == profile::kDipMaxFloor);   // dips past 24 mm aren't this wearer's taps
        PinchStat shallow;                       // a wearer whose quick taps often read 20 to 26 mm
        fill(shallow.taps, 10, 20, 12);          // 120 reach under 20
        fill(shallow.taps, 20, 26, 20);          // 120 don't (the dip rule caught them)
        CHECK(profile::learned_pinch(shallow, begin, dip).begin_m == begin);   // no idle samples yet: no move up
        fill(shallow.idle, 40, 50, 500);
        l = profile::learned_pinch(shallow, begin, dip);
        CHECK(std::fabs(l.begin_m - 0.026) < 1e-12);     // 80% under 25 mm, plus 2: capped at 6 mm over
        CHECK(std::fabs(l.dip_max_m - 0.029) < 1e-12);   // 97% under 26 mm, plus 3
        PinchStat typist = shallow;              // the same taps, but resting fingers come within 2 cm
        fill(typist.idle, 15, 20, 30);           // 150 of 5150: 3%
        l = profile::learned_pinch(typist, begin, dip);
        CHECK(l.begin_m == begin && std::fabs(l.dip_max_m - 0.029) < 1e-12);
        PinchStat close_idle = shallow;          // idle fingers down to 26 mm: begin may reach 23, not 26
        fill(close_idle.idle, 26, 28, 100);
        l = profile::learned_pinch(close_idle, begin, dip);
        CHECK(std::fabs(l.begin_m - 0.024) < 1e-12);     // 1% of the idle under 27 mm, less 3
        // never below the detector's own begin, never above its dip limit, and room for the dip rule
        for (const PinchStat &s : {firm, shallow, typist, close_idle}) {
            const profile::PinchLearned any = profile::learned_pinch(s, begin, dip);
            CHECK(any.begin_m >= begin && any.begin_m <= begin + profile::kBeginRise && any.dip_max_m <= dip &&
                  any.dip_max_m >= any.begin_m + 0.002 - 1e-12);
        }

        // the observer, at 30 frames a second: an open hand, a tap, an open hand, a pinch lost, a hand gone
        profile::PinchObserver seen;
        int64_t t = 0;
        const int64_t frame = 33'333'333;
        auto run = [&](int frames, double d, bool down, bool began = false, bool ended = false, bool lost = false) {
            for (int i = 0; i < frames; ++i, t += frame) seen.update(1, t, d, down, began && i == 0, ended && i == 0, lost && i == 0);
        };
        run(60, 0.060, false);                    // 2 s open at 60 mm
        run(4, 0.030, false);                     // closing
        run(1, 0.018, true, true);                // begins
        run(5, 0.012, true);                      // down, to 12 mm
        run(3, 0.040, false, false, true);        // ends, the fingers still opening
        run(60, 0.060, false);                    // 2 s open again
        CHECK(seen.stat[1].tap_n() == 1 && seen.stat[1].taps[12] == 1);
        CHECK(seen.stat[1].idle[30] == 0 && seen.stat[1].idle[40] == 0);   // the closing and the opening aren't idle
        const double idle_before_lost = seen.stat[1].idle_n();
        // 60 mm is past the last bin, which takes it: 2 s at 10 a second, less 0.3 s before the pinch;
        // then 2 s less the 0.3 s after it, and less the 0.3 s still waiting at the end
        CHECK(idle_before_lost >= 28 && idle_before_lost <= 34 && seen.stat[1].idle[49] == idle_before_lost);
        if (!(idle_before_lost >= 28 && idle_before_lost <= 34)) std::printf("idle samples: %.0f\n", idle_before_lost);
        run(1, 0.015, true, true);
        run(3, 0.015, true);
        run(1, -1, false, false, false, true);    // the hand went: no tap
        CHECK(seen.stat[1].tap_n() == 1);
        run(30, -1, false);                       // no hand: nothing counted
        CHECK(seen.stat[1].idle_n() <= idle_before_lost + 3 && seen.stat[0].idle_n() == 0);
        seen.update(2, t, 0.05, false, false, false, false);   // not a side: ignored

        // in the file with the rest
        profile::Profile with = p, read;
        with.pinch[0] = shallow, with.pinch[1] = seen.stat[1];
        const std::string text = profile::text(with);
        CHECK(profile::parse(text, read) == 6 + 4 && profile::text(read) == text);
        CHECK(read.pinch[0].tap_n() == shallow.tap_n() && read.pinch[1].taps[12] == 1);
        CHECK(profile::parse("format 1\ntaps left 1 2 3\nidle right " + std::string(100, 'x') + "\n", read) == 0 && read.empty());
        std::string negative = "format 1\ntaps left";
        for (int b = 0; b < profile::kPinchBins; ++b) negative += b == 7 ? " -1" : " 0";
        CHECK(profile::parse(negative + "\n", read) == 0);
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
