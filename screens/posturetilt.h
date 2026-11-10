// The posture tilt (SCREEN_TILT_DEG; user, 2026-11-10: a video above the Mac panel to "lean back to
// watch", too high and too small wherever a fixed stack put it): leaning back tilts every panel in the
// room down round the eyes, so a big video over the work screen comes down to where a reclined head
// looks while the work screen stays in view under it, and sitting up tilts them back. Leaning is
// measured from where the layout was last arranged (ft-layout's layout-anchor.json: the eyes and the
// heading at the last arrange, reset or capture): the head that far back, level, for long enough.
// Header only, no OpenVR, so its test runs anywhere (screens/tests/posturetilt-test.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace posturetilt {

struct Params {
    double backM = 0.07;     // SCREEN_TILT_BACK: the head this much farther back than where it sits upright leans back
    int64_t leanMs = 800;    // SCREEN_TILT_MS: ...for this long tilts down
    int64_t upMs = 400;      // back within half of backM for this long tilts up again
    double farM = 0.8;       // farther from the anchor's eyes (walked off, the room moved): neither
    double settleS = 20;     // while upright, where the head sits follows it this slowly (time constant, s)
};

// How far back the head is from the anchor's eyes, level, along the anchor's heading (metres; forward is
// negative). headingDeg: as ft-layout's and "place"'s yaw, 0 looking down -z, positive to the left.
inline double BackOf(const double head[3], const double eye[3], double headingDeg) {
    const double h = headingDeg * M_PI / 180;
    const double fx = -std::sin(h), fz = -std::cos(h);   // where the anchor faced, level
    return -((head[0] - eye[0]) * fx + (head[2] - eye[2]) * fz);
}

// Leaning back or sitting up, measured from where the head sits upright, learned while it does (user,
// 2026-11-10: the anchor is wherever a reset caught the head, 5 cm forward of how he sits, so leaning
// back from his own posture barely reached 12 cm from it): a dwell each way, and half the distance
// back to sit up again, so a head near the line doesn't flicker between them.
struct Detector {
    bool leaned = false;
    int64_t since = -1;     // when the other posture began (-1: it hasn't)
    bool based = false;     // `upright` is known
    double upright = 0;     // where the head sits upright (as BackOf), followed slowly while it does
    int64_t lastMs = -1;
    void Rebase() { based = false; }   // a new anchor: learn where the head sits again
    // True when `leaned` changed. back: BackOf; dist: how far the head is from the anchor's eyes.
    bool Update(double back, double dist, int64_t nowMs, const Params &k) {
        const double dt = lastMs < 0 ? 0 : (nowMs - lastMs) / 1000.0;
        lastMs = nowMs;
        if (dist > k.farM) return since = -1, false;
        if (!based) upright = back, based = true;
        const double rel = back - upright;
        const bool other = leaned ? rel <= k.backM / 2 : rel >= k.backM;
        if (!other) {
            if (!leaned && k.settleS > 0) upright += (back - upright) * std::min(1.0, dt / k.settleS);
            return since = -1, false;
        }
        if (since < 0) since = nowMs;
        if (nowMs - since < (leaned ? k.upMs : k.leanMs)) return false;
        leaned = !leaned, since = -1;
        return true;
    }
};

// Where a pose's centre is from `eye`, as a heading (degrees, as BackOf's).
template <class M>
double BearingOf(const M &p, const double eye[3]) {
    return std::atan2(-(p.m[0][3] - eye[0]), -(p.m[2][3] - eye[2])) * 180 / M_PI;
}

// A pose turned `deg` round the level axis through `eye` across `headingDeg` (that heading's right):
// positive raises what lies that way, negative lowers it. Each panel turns across its own bearing
// (BearingOf), so one beside you comes down upright rather than rolling where it is, and panels one
// above another, sharing a bearing, move as one. M: any type with a float m[3][4], as posemath's.
template <class M>
M Tilted(const M &p, double deg, const double eye[3], double headingDeg) {
    const double h = headingDeg * M_PI / 180, a = deg * M_PI / 180;
    const double u[3] = {std::cos(h), 0, -std::sin(h)};   // that heading's right
    const double c = std::cos(a), s = std::sin(a), t = 1 - c;
    const double R[3][3] = {{c + t * u[0] * u[0], t * u[0] * u[1] - s * u[2], t * u[0] * u[2] + s * u[1]},
                            {t * u[1] * u[0] + s * u[2], c + t * u[1] * u[1], t * u[1] * u[2] - s * u[0]},
                            {t * u[2] * u[0] - s * u[1], t * u[2] * u[1] + s * u[0], c + t * u[2] * u[2]}};
    M out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += R[i][k] * p.m[k][j];
            out.m[i][j] = float(v);
        }
        double v = eye[i];
        for (int k = 0; k < 3; ++k) v += R[i][k] * (p.m[k][3] - eye[k]);
        out.m[i][3] = float(v);
    }
    return out;
}

}  // namespace posturetilt
