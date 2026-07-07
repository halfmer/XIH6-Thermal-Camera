#ifndef FRAMEGATE_H
#define FRAMEGATE_H

#include <QtGlobal>

// Content-level gate for assembled thermal frames.
//
// The Lepton 3.x VoSPI stream delivers a 160x120 frame as 4 horizontal
// segments of 30 rows. The STM32 publisher keeps a persistent segment shelf
// (README_7 baseline: the 4 segments may come from different capture rounds),
// so a checksum-valid frame can still be stitched from different moments in
// time. When the scene moves, the stitch shows up as hard horizontal seams at
// the segment boundaries (rows 29|30, 59|60, 89|90).
//
// analyze() compares the mean absolute row-to-row difference at each segment
// boundary against the frame's own median row difference. A seam that jumps
// far above that baseline marks the frame as torn; the caller then keeps the
// previous image on screen (the user prefers latency over torn frames).

namespace FrameGate {

struct TearReport {
    bool   torn = false;
    int    worstBoundary = -1;   // 0/1/2 = seam at rows 29|30, 59|60, 89|90; -1 = none
    double worstDiff = 0.0;      // mean |row[b]-row[b+1]| at the worst seam (raw counts)
    double baseline  = 0.0;      // median row-to-row diff away from the seams
};

// A seam is torn only when it exceeds BOTH baseline * kTearRatio and
// kTearMinAbs (raw counts; TLinear centikelvin: 80 = 0.8 K). The absolute
// floor keeps sensor noise on a flat scene from tripping the ratio test; the
// ratio keeps scenes with strong natural vertical gradients from tripping the
// absolute test.
inline constexpr double kTearRatio  = 8.0;
inline constexpr double kTearMinAbs = 80.0;

TearReport analyze(const quint16 *data, int width, int height);

inline bool looksTorn(const quint16 *data, int width, int height)
{
    return analyze(data, width, height).torn;
}

} // namespace FrameGate

#endif // FRAMEGATE_H
