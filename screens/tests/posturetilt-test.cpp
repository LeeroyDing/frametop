// The posture tilt (screens/posturetilt.h): how far back the head is from the anchor, leaning back and
// sitting up with their dwells and hysteresis, and the tilt round the eyes across a panel's bearing.
#include "../posturetilt.h"

#include <cmath>
#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b, double tol = 1e-4) { return std::fabs(a - b) < tol; }

struct M {
    float m[3][4];
};

// A panel at (x, y, z) facing back along the direction it's seen from (yaw left, pitch up), as "place".
static M Panel(double x, double y, double z, double yawDeg, double pitchDeg) {
    const double yw = yawDeg * M_PI / 180, pt = pitchDeg * M_PI / 180;
    const double f[3] = {-std::sin(yw) * std::cos(pt), std::sin(pt), -std::cos(yw) * std::cos(pt)};
    const double Z[3] = {-f[0], -f[1], -f[2]};
    double X[3] = {Z[2], 0, -Z[0]};
    const double n = std::hypot(X[0], X[2]);
    X[0] /= n, X[2] /= n;
    const double Y[3] = {Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0]};
    M p{};
    for (int i = 0; i < 3; ++i) p.m[i][0] = float(X[i]), p.m[i][1] = float(Y[i]), p.m[i][2] = float(Z[i]);
    p.m[0][3] = float(x), p.m[1][3] = float(y), p.m[2][3] = float(z);
    return p;
}

static double Dist(const M &p, const double e[3]) {
    return std::hypot(p.m[0][3] - e[0], p.m[1][3] - e[1], p.m[2][3] - e[2]);
}

int main() {
    using namespace posturetilt;
    const double eye[3] = {0, 1.6, 0};

    // back: along the anchor's heading, level; sideways and up don't count
    {
        const double back[3] = {0, 1.5, 0.2}, fwd[3] = {0, 1.6, -0.1}, side[3] = {0.3, 1.6, 0};
        check("back: 20 cm behind the eyes is 0.2", near(BackOf(back, eye, 0), 0.2));
        check("back: 10 cm ahead is -0.1", near(BackOf(fwd, eye, 0), -0.1));
        check("back: to the side is 0", near(BackOf(side, eye, 0), 0));
        const double west[3] = {0.15, 1.6, 0};   // facing -x (heading 90, left), +x is back
        check("back: along a turned heading", near(BackOf(west, eye, 90), 0.15));
    }

    // leaning back: backM farther back than where the head sits upright, for leanMs; sitting up: within half
    // of it for upMs. The head sits 5 cm behind the anchor here, as it did in the live test.
    {
        Params k;
        Detector d;
        check("upright: learned at the first look", !d.Update(0.05, 0.1, 0, k) && near(d.upright, 0.05));
        check("lean: not at once", !d.Update(0.13, 0.15, 100, k) && !d.leaned);
        check("lean: not before leanMs", !d.Update(0.13, 0.15, 800, k) && !d.leaned);
        check("lean: at leanMs", d.Update(0.13, 0.15, 900, k) && d.leaned);
        check("lean: between half and full stays leaned", !d.Update(0.10, 0.12, 5000, k) && d.leaned);
        check("sit up: within half, not at once", !d.Update(0.06, 0.08, 6000, k) && d.leaned);
        check("sit up: a lean in between starts over", !d.Update(0.12, 0.13, 6200, k) && !d.Update(0.06, 0.08, 6300, k) && d.leaned);
        check("sit up: at upMs", d.Update(0.06, 0.08, 6700, k) && !d.leaned);
        check("upright: not moved by the lean", near(d.upright, 0.05));
        Detector e;
        e.Update(0.05, 0.1, 0, k);
        e.Update(0.13, 0.15, 100, k);
        check("lean: a dip back under it starts over", !e.Update(0.07, 0.1, 600, k) && !e.Update(0.13, 0.15, 700, k) &&
                                                       !e.Update(0.13, 0.15, 1400, k) && e.Update(0.13, 0.15, 1500, k));
        // where the head sits upright follows it slowly, so a lean counts from how you sit now
        Detector s;
        s.Update(0.0, 0.1, 0, k);
        for (int t = 1; t <= 600; ++t) s.Update(0.05, 0.1, t * 100, k);   // a minute 5 cm farther back
        check("upright: follows where the head sits", near(s.upright, 0.05, 0.005) && !s.leaned);
        check("lean: from there, 5 cm more is not leaning back",
              !s.Update(0.10, 0.12, 60100, k) && !s.Update(0.10, 0.12, 61500, k) && !s.leaned);
        check("lean: 8 cm more is", !s.Update(0.13, 0.15, 62000, k) && s.Update(0.13, 0.15, 62800, k) && s.leaned);
        Detector f;
        f.Update(0.05, 0.1, 0, k);
        check("far from the anchor: nothing", !f.Update(0.3, 1.0, 100, k) && !f.Update(0.3, 1.0, 2000, k) && !f.leaned);
        Detector r;
        r.Update(0.05, 0.1, 0, k);
        r.Rebase();
        check("a new anchor: learned again", !r.Update(0.20, 0.25, 100, k) && near(r.upright, 0.20) && !r.leaned);
    }

    // the tilt: round the eyes, down across the panel's bearing, keeping its distance and facing you
    {
        const M ahead = Panel(0, 1.6, -2, 0, 0);
        check("bearing: straight ahead is 0", near(BearingOf(ahead, eye), 0));
        const M t = Tilted(ahead, -15, eye, BearingOf(ahead, eye));
        check("tilt: 15 deg down", near(t.m[1][3], 1.6 - 2 * std::sin(15 * M_PI / 180), 1e-5) &&
                                   near(t.m[2][3], -2 * std::cos(15 * M_PI / 180), 1e-5) && near(t.m[0][3], 0, 1e-6));
        check("tilt: as far from the eyes", near(Dist(t, eye), 2, 1e-5));
        const double to[3] = {eye[0] - t.m[0][3], eye[1] - t.m[1][3], eye[2] - t.m[2][3]};
        const double n = std::hypot(to[0], to[1], to[2]);
        check("tilt: still facing the eyes",
              near(t.m[0][2], to[0] / n, 1e-5) && near(t.m[1][2], to[1] / n, 1e-5) && near(t.m[2][2], to[2] / n, 1e-5));
        const M back = Tilted(t, 15, eye, BearingOf(t, eye));
        bool same = true;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) same = same && near(back.m[i][j], ahead.m[i][j], 1e-5);
        check("tilt: up again puts it back", same);

        // one above another on the same bearing moves as one: the video stays over the Mac
        const M mac = Panel(0, 1.6 - 1.9 * std::sin(15 * M_PI / 180), -1.9 * std::cos(15 * M_PI / 180), 0, -15);
        const M video = Panel(0, 1.6 + 2.4 * std::sin(28.8 * M_PI / 180), -2.4 * std::cos(28.8 * M_PI / 180), 0, 28.8);
        const M mt = Tilted(mac, -15, eye, BearingOf(mac, eye)), vt = Tilted(video, -15, eye, BearingOf(video, eye));
        const double up = std::atan2(vt.m[1][3] - 1.6, -vt.m[2][3]) * 180 / M_PI,
                     down = std::atan2(mt.m[1][3] - 1.6, -mt.m[2][3]) * 180 / M_PI;
        check("stack: the video comes 15 deg down, to 13.8", near(up, 13.8, 1e-3));
        check("stack: the Mac goes to -30", near(down, -30, 1e-3));

        // a panel beside you comes down upright instead of rolling where it is
        const M left = Panel(-2, 1.6, 0, 90, 0);
        check("bearing: on your left is 90", near(BearingOf(left, eye), 90));
        const M lt = Tilted(left, -15, eye, BearingOf(left, eye));
        check("beside: 15 deg down, as far", near(lt.m[1][3], 1.6 - 2 * std::sin(15 * M_PI / 180), 1e-5) &&
                                            near(Dist(lt, eye), 2, 1e-5) && near(lt.m[2][3], 0, 1e-6));
        check("beside: still upright (its right is level)", near(lt.m[1][0], 0, 1e-6));

        // the eyes don't move
        M at = ahead;
        at.m[0][3] = float(eye[0]), at.m[1][3] = float(eye[1]), at.m[2][3] = float(eye[2]);
        const M e = Tilted(at, -15, eye, 0);
        check("tilt: a point at the eyes stays", near(e.m[0][3], 0) && near(e.m[1][3], 1.6) && near(e.m[2][3], 0));
    }

    std::printf("%d failed\n", failed);
    return failed ? 1 : 0;
}
