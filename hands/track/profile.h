// The wearer's hand profile: what ft-hands has learned about these hands, kept from one run to
// the next so a start doesn't learn it all again (hands/tracking.md, "Hand profile"). Numbers
// only: each side's hand size against the model's average hand (Hand::scale) and the 30
// quantities of its shape (skel::Calibration). No images, and nothing here leaves the headset.
//
// The file ($XDG_STATE_HOME/frametop/hands/profile, ~/.local/state/...; mode 0600) is lines of
// words, so it reads as it is:
//   format 1
//   scale left|right VALUE SAMPLES
//   shape left|right, then the 30 quantities in metres
//   count left|right, then each quantity's samples
// A line it doesn't know is skipped, a value no hand has is dropped with its line, and a file
// that can't be read counts as none: the tracker then learns as it did before there was one.
#pragma once

#include "skeleton.h"

#include <string>

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

struct Profile {
    Side side[2];   // left, right
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
