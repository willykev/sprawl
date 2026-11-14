#pragma once

#include <QStringList>
#include <cmath>

#include "canvasview.h"

// What `sprawl open --bench` and `sprawl open --probe` may be asked to do.
// They measure the canvas on the machine it runs on, and on the one they
// were made for, a frame too heavy for the GPU takes the whole desktop with
// it, so they keep to what is known to be safe there: what is asked for past
// that is cut back to it, and what makes no sense is refused. Either way, a
// line saying so is added to `said`: in English, like all the commands print.
namespace Measuring {

// The biggest board --bench makes up: about twice the one it was measured
// on, which kept up with a 120 Hz display (see docs/guide.md).
inline constexpr int mostItems = 100'000;

// What --probe-with can name. The first is what is drawn when it names
// nothing.
inline const QStringList &probeKinds() {
    static const QStringList kinds{"strokes", "notes", "far", "unseen", "writing", "drawing"};
    return kinds;
}

// The bytes --probe draws up to, from the megabytes it is given: never more
// than any frame is allowed (CanvasView::largestLoadBudget), whatever it
// draws. None when what it is given is not a number above none.
inline qint64 probeLimit(const QString &megabytes, QStringList &said) {
    bool number = false;
    const double asked = megabytes.toDouble(&number);
    if (!number || !(asked > 0)) {
        said << "--probe is the megabytes to draw up to, a number above 0, like 8";
        return 0;
    }
    const double most = CanvasView::largestLoadBudget / 1e6;
    if (asked > most) {
        said << QString("--probe draws no more than %1 MB, as more could take the desktop with it, "
                        "so this one draws up to that")
                    .arg(most);
        return CanvasView::largestLoadBudget;
    }
    return qMax<qint64>(1, qint64(asked * 1e6));
}

// What --probe-with names, or the first of probeKinds() when it names
// nothing. Nothing when it names something else.
inline QString probeKind(const QString &kind, QStringList &said) {
    if (kind.isEmpty())
        return probeKinds().first();
    if (probeKinds().contains(kind))
        return kind;
    said << "--probe-with is one of " + probeKinds().join(", ");
    return {};
}

// How many items --bench makes up a board of: never more than mostItems.
// Less than none when what it is given is not a number of items.
inline int benchItems(const QString &items, QStringList &said) {
    bool number = false;
    const double asked = items.toDouble(&number);
    if (!number || !(asked >= 0) || std::floor(asked) != asked) {
        said << "--bench is how many items to make up a board of, like 10000";
        return -1;
    }
    if (asked > mostItems) {
        said << QString("--bench makes up boards of %1 items at most, so this one has that many").arg(mostItems);
        return mostItems;
    }
    return int(asked);
}

} // namespace Measuring
