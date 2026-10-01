#include "Theme.h"

#include <QApplication>
#include <QPalette>

namespace kairo::ui {

Theme Theme::detect() {
    Theme t;
    t.dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    if (t.dark) {
        t.window = QColor("#0e1115"); t.surface = QColor("#161a20"); t.surface2 = QColor("#1b2027");
        t.border = QColor("#2a3039"); t.borderStrong = QColor("#3a424d"); t.text = QColor("#e4e8ed");
        t.muted = QColor("#96a0ad"); t.accent = QColor("#6ea2ff"); t.accentSoft = QColor("#1a2638");
        t.ok = QColor("#4cc38a"); t.okSoft = QColor("#13271e"); t.warn = QColor("#e2a33c"); t.warnSoft = QColor("#2a2114");
        t.error = QColor("#f0766c"); t.errorSoft = QColor("#2c1716"); t.neutral = QColor("#a3acb8"); t.neutralSoft = QColor("#20252c");
    } else {
        t.window = QColor("#f4f5f7"); t.surface = QColor("#ffffff"); t.surface2 = QColor("#f8f9fb");
        t.border = QColor("#dcdfe4"); t.borderStrong = QColor("#c4c9d1"); t.text = QColor("#14181d");
        t.muted = QColor("#5b6471"); t.accent = QColor("#1f5fbf"); t.accentSoft = QColor("#e8effa");
        t.ok = QColor("#17784a"); t.okSoft = QColor("#e6f4ec"); t.warn = QColor("#9a5b00"); t.warnSoft = QColor("#fbf1e1");
        t.error = QColor("#b42318"); t.errorSoft = QColor("#fbeaea"); t.neutral = QColor("#4b5563"); t.neutralSoft = QColor("#eef0f3");
    }
    return t;
}

QColor Theme::tone(const QString& tone) const {
    if (tone == QLatin1String("ok")) return ok;
    if (tone == QLatin1String("warn")) return warn;
    if (tone == QLatin1String("error")) return error;
    if (tone == QLatin1String("accent")) return accent;
    if (tone == QLatin1String("neutral")) return neutral;
    return muted;
}

QColor Theme::toneSoft(const QString& tone) const {
    if (tone == QLatin1String("ok")) return okSoft;
    if (tone == QLatin1String("warn")) return warnSoft;
    if (tone == QLatin1String("error")) return errorSoft;
    if (tone == QLatin1String("accent")) return accentSoft;
    return neutralSoft;
}

QString Theme::styleSheet() const {
    const auto c = [](const QColor& colour) { return colour.name(); };
    return QStringLiteral(R"(
QMainWindow, QWidget#reportPage, QWidget#sidebarPage { background: %1; }
QWidget { color: %6; }
QFrame#sidebar { background: %2; border-right: 1px solid %4; }
QFrame#header { background: %2; border-bottom: 1px solid %4; }
QLabel#wordmark { font-size: 20px; font-weight: 800; letter-spacing: 4px; }
QLabel#product { color: %8; font-size: 15px; font-weight: 600; }
QLabel#tagline { color: %7; }
QFrame[section="true"] { background: %2; border: 1px solid %4; border-radius: 6px; }
QFrame[section="true"][highlighted="true"] { border: 2px solid %8; }
QLabel[role="sectionTitle"] { color: %7; font-size: 11px; font-weight: 700; letter-spacing: 1px; }
QLabel[role="question"] { color: %7; font-size: 12px; }
QLabel[role="muted"] { color: %7; }
QLabel[role="mono"] { font-family: "Menlo", "Consolas", "DejaVu Sans Mono", monospace; }
QLabel[role="figureLabel"] { color: %7; font-size: 10px; font-weight: 600; letter-spacing: 1px; }
QLabel[role="figureValue"] { font-size: 16px; font-weight: 650; }
QLabel[role="notRun"] { color: %7; font-style: italic; }
QFrame[box="true"] { background: %3; border: 1px solid %5; border-radius: 6px; }
QFrame[box="true"][dashed="true"] { border-style: dashed; }
QGroupBox { font-weight: 700; color: %7; border: 0; margin-top: 18px; }
QGroupBox::title { subcontrol-origin: margin; left: 0; padding: 0; }
QPushButton#solveButton { background: %8; color: %9; font-weight: 700; padding: 8px; border-radius: 6px; border: 0; }
QPushButton#solveButton:disabled { background: %5; color: %7; }
QListWidget { background: %3; border: 1px solid %4; border-radius: 6px; }
)")
        .arg(c(window), c(surface), c(surface2), c(border), c(borderStrong), c(text), c(muted), c(accent),
             dark ? QStringLiteral("#0b1220") : QStringLiteral("#ffffff"));
}

namespace {
Theme& instance() {
    static Theme current = Theme::detect();
    return current;
}
}  // namespace

const Theme& theme() { return instance(); }

bool reloadTheme() {
    const Theme next = Theme::detect();
    if (next.dark == instance().dark) return false;
    instance() = next;
    return true;
}

}  // namespace kairo::ui
