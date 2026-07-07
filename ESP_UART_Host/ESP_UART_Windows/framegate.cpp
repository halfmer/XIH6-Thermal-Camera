#include "framegate.h"

#include <QVector>
#include <algorithm>
#include <cmath>

namespace FrameGate {

static double meanAbsRowDiff(const quint16 *data, int width, int row)
{
    const quint16 *a = data + row * width;
    const quint16 *b = a + width;
    qint64 acc = 0;
    for (int x = 0; x < width; ++x)
        acc += std::abs(int(a[x]) - int(b[x]));
    return double(acc) / width;
}

TearReport analyze(const quint16 *data, int width, int height)
{
    TearReport r;
    if (!data || width <= 0 || height < 8 || (height % 4) != 0)
        return r;                      // not a 4-segment layout; let it pass

    const int seg = height / 4;
    const int seams[3] = { seg - 1, 2 * seg - 1, 3 * seg - 1 };

    QVector<double> plain;             // row diffs away from the 3 seams
    plain.reserve(height - 4);
    double seamDiff[3] = { 0.0, 0.0, 0.0 };

    for (int row = 0; row < height - 1; ++row) {
        const double d = meanAbsRowDiff(data, width, row);
        if (row == seams[0])      seamDiff[0] = d;
        else if (row == seams[1]) seamDiff[1] = d;
        else if (row == seams[2]) seamDiff[2] = d;
        else                      plain.append(d);
    }

    std::sort(plain.begin(), plain.end());
    r.baseline = plain.isEmpty() ? 0.0 : plain[plain.size() / 2];

    const double limit = std::max(kTearMinAbs, r.baseline * kTearRatio);
    for (int i = 0; i < 3; ++i) {
        if (seamDiff[i] > r.worstDiff) {
            r.worstDiff = seamDiff[i];
            r.worstBoundary = i;
        }
    }
    r.torn = (r.worstDiff > limit);
    if (!r.torn)
        r.worstBoundary = -1;
    return r;
}

} // namespace FrameGate
