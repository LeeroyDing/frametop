// The wearer's hand profile: what ft-hands has learned about these hands, kept from one run to
// the next so a start doesn't learn it all again (hands/tracking.md, "Hand profile"). Numbers
// only: each side's hand size against the model's average hand (Hand::scale), the 30 quantities
// of its shape (skel::Calibration), where the palm search finds this wearer's hands (TileStat), and
// how far apart the thumb and index read when they pinch and when they don't (PinchStat).
// No images, and nothing here leaves the headset.
//
// The file ($XDG_STATE_HOME/frametop/hands/profile, ~/.local/state/...; mode 0600) is lines of
// words, so it reads as it is:
//   format 1
//   scale left|right VALUE SAMPLES
//   shape left|right, then the 30 quantities in metres
//   count left|right, then each quantity's samples
//   tiles CAMERA N, then N pairs: how often the palm search tried each of that camera's tiles, and
//   how often that found a hand
//   taps left|right and idle left|right, then 50 counts, a millimetre each: the lowest thumb to index
//   distance each pinch reached, and the distances of a hand that wasn't pinching
// A line it doesn't know is skipped, a value no hand has is dropped with its line, and a file
// that can't be read counts as none: the tracker then learns as it did before there was one.
#pragma once

#include "skeleton.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace profile {

// A value that takes the mean of its first kSettle samples, then follows them by at most `step`
// a sample (as skel::Calibration follows a bone's length), so one odd reading hardly moves it.
struct Running {
    static constexpr int kSettle = 45;
    double value = 0;
    long n = 0;
    void add(double x, double step);
    bool settled() const { return n >= kSettle; }
};

struct Side {
    Running scale;                    // Hand::scale over every clean stereo frame of this side
    double q[skel::kQuantities]{};    // the shape's quantities, m (skel::Calibration's estimates)
    int n[skel::kQuantities]{};       // and each one's samples
    int settled() const;              // how many of the quantities have kSettle samples or more
};

// One of the palm search's tiles (Tracker::Tile): how often the search's rotation tried it while no
// tracked hand was in it, and how often that found a hand. Where this wearer's hands come into
// view and rest shows up as tiles that find more than the others.
struct TileStat {
    double tries = 0, finds = 0;
};
// How much more (or less) often than its fixed weight says a tile is searched: its finds per try
// against every tile's together, between kTileBoostMin and kTileBoostMax. kTilePrior tries at the
// mean rate are counted in first, so a tile hardly tried keeps its fixed weight, and no tile is
// ever left out: the worst is searched half as often as before.
constexpr double kTilePrior = 50, kTileBoostMin = 0.5, kTileBoostMax = 4.0;
double mean_rate(const std::map<std::string, std::vector<TileStat>> &tiles);   // finds per try, all tiles
double tile_boost(const TileStat &t, double mean_rate);
// Past kTileForget tries in all, every count is halved: last month's desk counts for less than this week's.
constexpr double kTileForget = 20000;
void forget_old(std::map<std::string, std::vector<TileStat>> &tiles);

// One side's pinches, as the pinch detector measures them (Pinch::world_d: thumb tip to index
// tip), in bins of a millimetre up to kPinchBins mm (the last bin takes everything past it).
constexpr int kPinchBins = 50;
struct PinchStat {
    double taps[kPinchBins]{};   // each pinch that ended by opening: the lowest distance it read
    double idle[kPinchBins]{};   // a tracked hand with no pinch down, begun or ended within kPinchClear s
    double tap_n() const;
    double idle_n() const;
    void add(const PinchStat &o);
};
// The distance under which the share q of bins' counts lies (m, to the millimetre; -1 with none).
double quantile(const double bins[kPinchBins], double q);

// The pinch detector's two distances, moved to where this wearer's pinches are (PinchParams:
// begin_m, a pinch begins under it; dip_max_m, the dip rule only counts dips that get under it).
//  - dip_max_m comes down to 3 mm past where 97% of the wearer's taps bottom out, and no lower than
//    kDipMaxFloor: fingers that stop further apart than this wearer's taps ever do aren't a tap.
//    It never goes up.
//  - begin_m goes up to 2 mm past where 80% of the taps bottom out, when that is over the detector's
//    own, by at most kBeginRise, and only as far as 3 mm under the lowest 1% of the idle hand's
//    distances: a wearer whose resting fingers come that close (typing does it) keeps the
//    detector's. It never goes down.
// Nothing moves before kPinchMinTaps taps, nor begin_m before kPinchMinIdle idle samples.
constexpr double kPinchMinTaps = 100, kPinchMinIdle = 3000, kDipMaxFloor = 0.024, kBeginRise = 0.006;
struct PinchLearned {
    double begin_m, dip_max_m;
};
PinchLearned learned_pinch(const PinchStat &both_sides, double begin_m, double dip_max_m);

// Fills a PinchStat per side from the pinch detector's readings, one update a processed set.
class PinchObserver {
public:
    static constexpr double kPinchClear = 0.3;    // s either side of a pinch that aren't "idle"
    static constexpr double kIdleEvery = 0.1;     // s between idle samples
    // distance: Pinch::world_d (m; under 0: no hand); down: a pinch is down on the side; began, ended
    // and lost: this update's events (lost: the hand went, or a grip took it: not a tap).
    void update(int side, int64_t t_ns, double distance, bool down, bool began, bool ended, bool lost);
    PinchStat stat[2];
    // Past kTapsForget taps or kIdleForget idle samples on a side, its counts are halved.
    static constexpr double kTapsForget = 5000, kIdleForget = 200000;

private:
    double low_[2] = {1, 1};
    int64_t quiet_until_[2] = {0, 0}, last_idle_[2] = {0, 0};
    std::deque<std::pair<int64_t, double>> pending_[2];   // idle samples a pinch may yet claim as its approach
};

struct Profile {
    Side side[2];   // left, right
    std::map<std::string, std::vector<TileStat>> tiles;   // camera -> its tiles, in the tracker's order
    PinchStat pinch[2];
    PinchStat &side_pinch(int s) { return pinch[s]; }
    bool empty() const;
};

// What a hand can be: a value outside these isn't taken from a file.
constexpr double kScaleMin = 0.8, kScaleMax = 1.6;   // Tracker::hand_3d clamps its samples to these
bool plausible(int quantity, double metres);

std::string default_path();          // makes its folders (0700)
std::string text(const Profile &p);  // the file's contents
// Reads text (a file's contents) into out, which it clears first. Returns how many lines it kept.
int parse(const std::string &text, Profile &out);
// True with an empty profile when there's no file; false and err when one can't be read.
bool load(const std::string &path, Profile &out, std::string &err);
// Through a temporary file beside it, then a rename, so a reader never sees half of one.
bool save(const std::string &path, const Profile &p, std::string &err);

}  // namespace profile
