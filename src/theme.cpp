// Theme watching follows Omacut, reading the whole palette follows Hype's
// AppTheme, and the roles and state tokens follow the Omarchy shell's Color
// and Style (all MIT, David Heinemeier Hansson).
#include "theme.h"

#include "item.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>
#include <functional>

namespace {

QColor withAlpha(QColor color, double alpha) {
    color.setAlphaF(qBound(0.0, alpha, 1.0) * color.alphaF());
    return color;
}

// A color as theme files write one: "#1a1b26", or Hyprland's "rgb(1a1b26)"
// and "rgba(1a1b26ee)".
QColor parseColor(const QString &text) {
    static const QRegularExpression hyprland("^rgba?\\(([0-9a-fA-F]{6})([0-9a-fA-F]{2})?\\)$");
    const auto match = hyprland.match(text);
    if (match.hasMatch()) {
        QColor color("#" + match.captured(1));
        if (!match.captured(2).isEmpty())
            color.setAlpha(match.captured(2).toInt(nullptr, 16));
        return color;
    }
    return text.startsWith('#') ? QColor(text) : QColor();
}

// The paper luminance notes are toned to. Dark themes get calm, mid-dark
// paper that dark ink reads on; light themes get pastels.
const double darkPaper = 0.285;
const double lightPaper = 0.62;

void fillNotes(Palette &palette) {
    palette.notes.clear();
    for (auto it = palette.swatches.constBegin(); it != palette.swatches.constEnd(); ++it)
        palette.notes.insert(it.key(), Theme::notePaper(it.value(), palette.background, palette.dark));
}

} // namespace

Palette Palette::fallback() {
    // Without Omarchy the app still has a look: Tokyo Night, its default.
    Palette palette;
    palette.swatches = {
        {"red", QColor("#f7768e")},    {"orange", QColor("#eb927b")}, {"yellow", QColor("#e0af68")},
        {"green", QColor("#9ece6a")},  {"cyan", QColor("#449dab")},   {"blue", QColor("#7aa2f7")},
        {"magenta", QColor("#ad8ee6")},
    };
    fillNotes(palette);
    return palette;
}

QStringList Theme::swatchOrder() { return Item::colorNames(); }

QColor Theme::mix(const QColor &base, const QColor &other, double amount) {
    return QColor::fromRgbF(base.redF() + (other.redF() - base.redF()) * amount,
                            base.greenF() + (other.greenF() - base.greenF()) * amount,
                            base.blueF() + (other.blueF() - base.blueF()) * amount);
}

// WCAG relative luminance.
double Theme::luminance(const QColor &color) {
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
}

QColor Theme::contrastInk(const QColor &color) {
    return luminance(color) > .179 ? QColor("#111111") : QColor("#ffffff");
}

QColor Theme::notePaper(const QColor &swatch, const QColor &canvas, bool dark) {
    const double target = dark ? darkPaper : lightPaper;
    const double own = luminance(swatch);
    // Already calm enough (dark themes) or pale enough (light themes).
    if (dark ? own <= target : own >= target)
        return swatch;
    // A canvas that cannot pull the swatch to the target leaves it alone.
    if (dark ? luminance(canvas) >= target : luminance(canvas) <= target)
        return swatch;
    double low = 0, high = 1; // how much of the swatch is kept
    for (int i = 0; i < 24; ++i) {
        const double keep = (low + high) / 2;
        const double value = luminance(mix(canvas, swatch, keep));
        if (dark ? value > target : value < target)
            high = keep;
        else
            low = keep;
    }
    return mix(canvas, swatch, (low + high) / 2);
}

QHash<QString, QString> Theme::readToml(const QString &path) {
    QHash<QString, QString> values;
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return values;
    static const QRegularExpression section("^\\s*\\[([A-Za-z0-9_.-]+)\\]");
    // A quoted value may hold a #; a bare one is a color or ends at a space
    // or a comment.
    static const QRegularExpression entry(
        R"re(^\s*([A-Za-z0-9_-]+)\s*=\s*(?:"([^"]*)"|'([^']*)'|(#[0-9A-Fa-f]{3,8}|[^#\s]+)))re");
    QString prefix;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine());
        const auto header = section.match(line);
        if (header.hasMatch()) {
            prefix = header.captured(1) + ".";
            continue;
        }
        const auto match = entry.match(line);
        if (!match.hasMatch())
            continue;
        const QString value = !match.captured(2).isNull()   ? match.captured(2)
                              : !match.captured(3).isNull() ? match.captured(3)
                                                            : match.captured(4);
        values.insert(prefix + match.captured(1), value);
    }
    return values;
}

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl)
    : QObject(parent), m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners, border and motion follow Hyprland's effective settings, which
    // themes and personal overrides can both set, so ask hyprctl rather than
    // reading its config. Without Hyprland the corners stay sharp, like
    // Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
                : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                    ? QStandardPaths::findExecutable("hyprctl")
                    : QString();
    auto answer = [](QProcess &process, int code, QProcess::ExitStatus status) {
        const QJsonObject reply = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        return code == 0 && status == QProcess::NormalExit ? reply : QJsonObject();
    };
    connect(&m_roundingProcess, &QProcess::finished, this, [this, answer](int code, QProcess::ExitStatus status) {
        const auto value = answer(m_roundingProcess, code, status).value("int");
        if (value.isDouble() && value.toInt(-1) >= 0 && value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    connect(&m_borderProcess, &QProcess::finished, this, [this, answer](int code, QProcess::ExitStatus status) {
        const auto value = answer(m_borderProcess, code, status).value("int");
        if (value.isDouble() && value.toInt(-1) >= 0 && value.toInt() != m_borderWidth) {
            m_borderWidth = value.toInt();
            emit changed();
        }
    });
    connect(&m_animationsProcess, &QProcess::finished, this, [this, answer](int code, QProcess::ExitStatus status) {
        const QJsonObject reply = answer(m_animationsProcess, code, status);
        if (!reply.contains("bool") && !reply.contains("int"))
            return;
        // `omarchy toggle animations` turns them off for machines that draw
        // on the CPU; a window that still animated would be the odd one out.
        const bool animates = reply.contains("bool") ? reply.value("bool").toBool() : reply.value("int").toInt() != 0;
        if (m_reduceMotion == animates) {
            m_reduceMotion = !animates;
            emit changed();
        }
    });
    m_poll.setInterval(2000);
    connect(&m_poll, &QTimer::timeout, this, &Theme::askHyprland);
    if (!m_hyprctl.isEmpty())
        m_poll.start();
    reload();
}

Theme::~Theme() {
    for (QProcess *process : {&m_roundingProcess, &m_borderProcess, &m_animationsProcess})
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
}

void Theme::askHyprland() {
    if (m_hyprctl.isEmpty())
        return;
    if (m_roundingProcess.state() == QProcess::NotRunning)
        m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    if (m_borderProcess.state() == QProcess::NotRunning)
        m_borderProcess.start(m_hyprctl, {"-j", "getoption", "general:border_size"});
    if (m_animationsProcess.state() == QProcess::NotRunning)
        m_animationsProcess.start(m_hyprctl, {"-j", "getoption", "animations:enabled"});
}

int Theme::size(const QString &token) const { return m_sizes.value(token, m_fontSize); }

QColor Theme::inkOn(const QColor &color) const {
    const QColor &bg = m_palette.background, &fg = m_palette.bright;
    const QColor &dark = luminance(bg) < luminance(fg) ? bg : fg, &light = luminance(bg) < luminance(fg) ? fg : bg;
    return luminance(color) > .179 ? dark : light;
}

void Theme::reload() {
    askHyprland();
    const QString theme = m_directory + "/theme";
    const QHash<QString, QString> colors = readToml(theme + "/colors.toml");
    const QHash<QString, QString> shell = readToml(theme + "/shell.toml");
    const Palette defaults = Palette::fallback();

    auto named = [&colors](const QString &key, const QColor &fallback) {
        const QColor color = parseColor(colors.value(key));
        return color.isValid() ? color : fallback;
    };
    Palette palette;
    palette.background = named("background", defaults.background);
    palette.foreground = named("foreground", defaults.foreground);
    const QColor bg = palette.background, fg = palette.foreground;
    const QString mode = colors.value("mode");
    palette.dark = mode == "light" ? false : mode == "dark" ? true : luminance(bg) < .179;
    const bool dark = palette.dark;
    palette.accent = named("accent", defaults.accent);
    palette.accentInk = luminance(palette.accent) > .179 ? (dark ? bg : fg) : (dark ? fg : bg);
    palette.panel = named("dark_background", mix(bg, dark ? QColor("#000000") : fg, dark ? .25 : .05));
    palette.raised = named("lighter_background", mix(bg, fg, .08));
    palette.bright = named("bright_foreground", fg);
    palette.muted = named("muted", mix(bg, fg, .3));
    palette.selection = named("selection", mix(bg, fg, .12));
    // Notes are written on in the darker of the theme's two base colors.
    palette.ink = luminance(bg) < luminance(fg) ? bg : fg;
    const QColor red = named("red", "#f7768e"), yellow = named("yellow", "#e0af68");
    palette.swatches = {
        {"red", red},
        {"orange", named("orange", mix(red, yellow, .5))},
        {"yellow", yellow},
        {"green", named("green", "#9ece6a")},
        {"cyan", named("cyan", "#449dab")},
        {"blue", named("blue", "#7aa2f7")},
        {"magenta", named("magenta", "#ad8ee6")},
    };
    fillNotes(palette);

    // A token in shell.toml is a color, a role, a pointer to another key
    // ("hyprland.active-border"), or a gradient, of which the first stop serves.
    std::function<QColor(const QString &, const QColor &, int)> resolve = [&](const QString &token, const QColor &fallback,
                                                                             int depth) -> QColor {
        static const QRegularExpression space("\\s+");
        QString first;
        for (const QString &part : token.trimmed().split(space, Qt::SkipEmptyParts))
            if (!part.endsWith("deg")) {
                first = part;
                break;
            }
        const QString role = first.toLower();
        if (role.isEmpty())
            return fallback;
        if (role == "foreground" || role == "text")
            return fg;
        if (role == "background")
            return bg;
        if (role == "accent")
            return palette.accent;
        if (role == "urgent")
            return red;
        if (role == "muted")
            return palette.muted;
        if (role == "transparent")
            return QColor(0, 0, 0, 0);
        if (shell.contains(role) && depth < 4)
            return resolve(shell.value(role), fallback, depth + 1);
        const QColor color = parseColor(first);
        return color.isValid() ? color : fallback;
    };
    auto token = [&](const QString &key, const QColor &fallback) {
        return shell.contains(key) ? resolve(shell.value(key), fallback, 0) : fallback;
    };
    auto number = [&shell](const QString &key, double fallback) {
        bool ok = false;
        const double value = shell.value(key).toDouble(&ok);
        return ok ? value : fallback;
    };

    const QColor normal = token("controls.normal-color", fg);
    const QColor hover = token("controls.hover-cursor-color", fg);
    const QColor focus = token("controls.focus-color", hover);
    const QColor selected = token("controls.selected-color", fg);
    const double hoverBorder = number("controls.hover-cursor-border-alpha", .25);

    // Derived rather than taken from dark_foreground, which some themes make
    // too pale to read as text.
    const QColor dim = mix(bg, fg, .62), faint = mix(bg, fg, .4);
    const QColor controlFill = withAlpha(normal, number("controls.normal-fill-alpha", .04));
    const QColor hoverFill = withAlpha(hover, number("controls.hover-cursor-fill-alpha", .08));
    const QColor selectedFill = withAlpha(selected, number("controls.selected-fill-alpha", .18));
    const QColor pressedFill =
        withAlpha(token("controls.pressed-color", hover), number("controls.pressed-fill-alpha", .22));
    const QColor controlBorder =
        withAlpha(token("controls.normal-border", normal), number("controls.normal-border-alpha", .4));
    const QColor focusBorder =
        withAlpha(token("controls.focus-border", focus), number("controls.focus-border-alpha", hoverBorder));
    const QColor cardBackground = withAlpha(token("popups.background", bg), number("popups.background-alpha", 1));
    const QColor cardBorder = withAlpha(token("popups.border", palette.accent), number("popups.border-alpha", 1));
    const QColor menuBackground = withAlpha(token("menu.background", bg), number("menu.background-alpha", 1));
    const QColor menuBorder = withAlpha(token("menu.border", fg), number("menu.border-alpha", 1));
    const QColor scrim = withAlpha(token("menu.scrim", bg), number("menu.scrim-alpha", .5));
    const QColor good = named("green", "#9ece6a");

    // The shell's type scale: every size derives from base-size unless the
    // theme pins one.
    const int base = qMax(1, int(number("font.base-size", 12)));
    auto scaled = [&](const QString &key, double ratio) {
        return qMax(1, int(std::lround(number("font." + key, base * ratio))));
    };
    const QHash<QString, int> sizes{
        {"caption", scaled("caption", 10.0 / 12)},   {"small", scaled("body-small", 11.0 / 12)},
        {"body", scaled("body", 1)},                 {"subtitle", scaled("subtitle", 13.0 / 12)},
        {"title", scaled("title", 14.0 / 12)},       {"heading", scaled("heading", 16.0 / 12)},
        {"display", scaled("display", 2)},
    };
    const int borderWidth = qMax(0, int(std::lround(number("controls.normal-border-width", 1))));

    const bool same = m_loaded && palette == m_palette && dim == m_dim && faint == m_faint && red == m_danger &&
                      good == m_good && yellow == m_warning && controlFill == m_controlFill &&
                      hoverFill == m_hoverFill &&
                      selectedFill == m_selectedFill && pressedFill == m_pressedFill &&
                      controlBorder == m_controlBorder && focusBorder == m_focusBorder &&
                      cardBackground == m_cardBackground && cardBorder == m_cardBorder &&
                      menuBackground == m_menuBackground && menuBorder == m_menuBorder && scrim == m_scrim &&
                      sizes == m_sizes && base == m_fontSize && borderWidth == m_controlBorderWidth;
    if (!same) {
        m_palette = palette;
        m_dim = dim;
        m_faint = faint;
        m_danger = red;
        m_good = good;
        m_warning = yellow;
        m_controlFill = controlFill;
        m_hoverFill = hoverFill;
        m_selectedFill = selectedFill;
        m_pressedFill = pressedFill;
        m_controlBorder = controlBorder;
        m_focusBorder = focusBorder;
        m_cardBackground = cardBackground;
        m_cardBorder = cardBorder;
        m_menuBackground = menuBackground;
        m_menuBorder = menuBorder;
        m_scrim = scrim;
        m_sizes = sizes;
        m_fontSize = base;
        m_controlBorderWidth = borderWidth;
        m_loaded = true;
        emit changed();
    }

    // A theme switch replaces symlinks and files, so watch the parents too
    // and re-arm after every change. Watching the nearest existing ancestor
    // means installing Omarchy later works without restarting the app.
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor,               QFileInfo(ancestor).absolutePath(), m_directory, theme,
                           theme + "/colors.toml", theme + "/shell.toml"};
    candidates.removeDuplicates();
    for (const QString &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
}
