// The posture tilt (SCREEN_TILT_DEG; user, 2026-11-10: a video above the Mac panel to "lean back to
// watch", too high and too small wherever a fixed stack put it): leaning back tilts every panel in the
// room down round the eyes, so a big video over the work screen comes down to where a reclined head
// looks while the work screen stays in view under it, and sitting up tilts them back. Leaning is
// measured from where the layout was last arranged (ft-layout's layout-anchor.json: the eyes and the
// heading at the last arrange, reset or capture): the head that far back, level, for long enough.
// Header only, no OpenVR, so its test runs anywhere (screens/tests/posturetilt-test.cpp).
#pragma once

#include <cmath>
#include <cstdint>

namespace posturetilt {

struct Params {
    double backM = 0.12;     // SCREEN_TILT_BACK: the head this far back from the anchor's eyes (level) leans back
    int64_t leanMs = 1000;   // SCREEN_TILT_MS: ...for this long tilts down
    int64_t upMs = 500;      // back within half of backM for this long tilts up again
    double farM = 0.8;       // farther from the anchor's eyes (walked off, the room moved): neither
};

// How far back the head is from the anchor's eyes, level, along the anchor's heading (metres; forward is
// negative). headingDeg: as ft-layout's and "place"'s yaw, 0 looking down -z, positive to the left.
inline double BackOf(const double head[3], const double eye[3], double headingDeg) {
    const double h = headingDeg * M_PI / 180;
    const double fx = -std::sin(h), fz = -std::cos(h);   // where the anchor faced, level
    return -((head[0] - eye[0]) * fx + (head[2] - eye[2]) * fz);
}

// Leaning back or sitting up: a dwell each way, and half the distance back to sit up again, so a head
// near the line doesn't flicker between them.
struct Detector {
    bool leaned = false;
    int64_t since = -1;   // when the other posture began (-1: it hasn't)
    // True when `leaned` changed. back: BackOf; dist: how far the head is from the anchor's eyes.
    bool Update(double back, double dist, int64_t nowMs, const Params &k) {
        if (dist > k.farM) return since = -1, false;
        const bool other = leaned ? back <= k.backM / 2 : back >= k.backM;
        if (!other) return since = -1, false;
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
