#pragma once

// Colours and the application style sheet. Tones carry meaning only:
// ok (checked / completed), warn (limits, infeasible, not established),
// error (failed / refused), muted (not run / not available), accent.
// Light and dark variants follow the platform palette.

#include <QColor>
#include <QString>

namespace kairo::ui {

struct Theme {
    bool dark = false;
    QColor window, surface, surface2, border, borderStrong, text, muted, accent, accentSoft;
    QColor ok, okSoft, warn, warnSoft, error, errorSoft, neutral, neutralSoft;

    static Theme detect();
    QString styleSheet() const;
    QColor tone(const QString& tone) const;      // foreground for a tone
    QColor toneSoft(const QString& tone) const;  // background for a tone
};

const Theme& theme();
// Re-reads the platform palette (light/dark switch while running). Returns
// true when the theme changed and the views must be rebuilt.
bool reloadTheme();

}  // namespace kairo::ui
