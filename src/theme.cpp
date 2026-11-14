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
#include <optional>

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

// A table as QML reads one: an object with a property for every name.
template <typename Value> QVariantMap forQml(const QHash<QString, Value> &table) {
    QVariantMap map;
    for (auto it = table.constBegin(); it != table.constEnd(); ++it)
        map.insert(it.key(), QVariant::fromValue(it.value()));
    return map;
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

QString Theme::userShellFile() { return QDir::homePath() + "/.config/omarchy/shell.toml"; }

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

QList<std::pair<QString, QString>> Theme::readShell(const QString &path) {
    QList<std::pair<QString, QString>> values;
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return values;
    static const QRegularExpression header(R"(^\[([A-Za-z0-9_-]+)\]\s*(#.*)?$)");
    // A quoted string that is not empty, one to four numbers (a border's
    // widths), or a word; and perhaps a comment after it.
    static const QRegularExpression entry(R"re(^([A-Za-z0-9_-]+)\s*=\s*(?:)re"
                                          R"re(["']([^"']+)["']|)re"
                                          R"re((-?[0-9]+(?:\.[0-9]+)?(?:\s+-?[0-9]+(?:\.[0-9]+)?){0,3})|)re"
                                          R"re(([A-Za-z][A-Za-z0-9_-]*))re"
                                          R"re()\s*(#.*)?$)re");
    QString section;
    const QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
    for (const QString &untrimmed : lines) {
        const QString line = untrimmed.trimmed();
        const auto named = header.match(line);
        if (named.hasMatch()) {
            section = named.captured(1);
            continue;
        }
        const auto match = entry.match(line);
        if (match.hasMatch() && !section.isEmpty())
            values.append({section + "." + match.captured(1),
                           match.captured(2) + match.captured(3) + match.captured(4)});
    }
    return values;
}

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl, const QString &userShell)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory),
      m_userShell(!userShell.isEmpty() ? userShell : directory.isEmpty() ? userShellFile() : QString()) {
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
            ++m_revision;
            emit changed();
        }
    });
    connect(&m_borderProcess, &QProcess::finished, this, [this, answer](int code, QProcess::ExitStatus status) {
        const auto value = answer(m_borderProcess, code, status).value("int");
        if (value.isDouble() && value.toInt(-1) >= 0 && value.toInt() != m_borderWidth) {
            m_borderWidth = value.toInt();
            ++m_revision;
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
            ++m_revision;
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

QVariantMap Theme::swatches() const { return forQml(m_palette.swatches); }
QVariantMap Theme::notes() const { return forQml(m_palette.notes); }
QVariantMap Theme::sizes() const { return forQml(m_sizes); }

int Theme::size(const QString &token) const { return m_sizes.value(token, m_fontSize); }

QFont Theme::font() const {
    QFont font(fontFamily());
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(m_fontSize);
    return font;
}

QColor Theme::inkOn(const QColor &color, int) const {
    const QColor &bg = m_palette.background, &fg = m_palette.bright;
    const QColor &dark = luminance(bg) < luminance(fg) ? bg : fg, &light = luminance(bg) < luminance(fg) ? fg : bg;
    return luminance(color) > .179 ? dark : light;
}

void Theme::reload() {
    // Watched afresh before anything is read: a change made while this one is
    // taken in, as the shell's text size slider makes one after another, is
    // then caught and read in turn, and never lost in between.
    watch();
    askHyprland();
    const QString theme = m_directory + "/theme";
    const QHash<QString, QString> colors = readToml(theme + "/colors.toml");
    // The shell's own dictionary (Color.mergeShell): the theme's keys with the
    // person's values over them, so a size they set outlasts every theme
    // switch, and after them the person's other keys.
    QHash<QString, QString> shell;
    QStringList order;
    for (const auto &[key, value] : readShell(theme + "/shell.toml") + readShell(m_userShell)) {
        if (!shell.contains(key))
            order << key;
        shell.insert(key, value);
    }
    // [style] is the shell's old name for [controls]. Where both have a key,
    // the one later in that dictionary counts, as in Style.applyShellValues.
    QHash<QString, QString> controls;
    for (const QString &key : std::as_const(order))
        if (key.startsWith("controls.") || key.startsWith("style."))
            controls.insert(key.section('.', 1), shell.value(key));
    for (auto it = controls.constBegin(); it != controls.constEnd(); ++it)
        shell.insert("controls." + it.key(), it.value());
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
    // A number, as the shell reads one (Number): all of the value, or none.
    // Deliberately not what only JavaScript makes a number of, a blank (0)
    // or "0x10" (16), which nobody writes for an alpha or a width.
    auto number = [&shell](const QString &key, double fallback) {
        bool ok = false;
        const double value = shell.value(key).toDouble(&ok);
        return ok && std::isfinite(value) ? value : fallback;
    };
    // A size, as the shell reads one (parseInt): the whole number its value
    // starts with, if it starts with one. One too big to be a size is none,
    // where the shell's would wrap around to some other number.
    auto whole = [&shell](const QString &key) -> std::optional<int> {
        static const QRegularExpression leading("^\\s*([+-]?[0-9]+)");
        const auto match = leading.match(shell.value(key));
        bool ok = false;
        const int value = match.hasMatch() ? match.captured(1).toInt(&ok) : 0;
        return ok ? std::optional<int>(value) : std::nullopt;
    };
    auto flag = [&shell](const QString &key, bool fallback) {
        const QString value = shell.value(key).trimmed().toLower();
        return QStringList{"true", "1", "yes", "on"}.contains(value)     ? true
               : QStringList{"false", "0", "no", "off"}.contains(value) ? false
                                                                         : fallback;
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

    // The shell's type scale (Style.font): every size derives from base-size,
    // at the shell's own ratios, unless the theme or the person pins one.
    const int base = qMax(1, whole("font.base-size").value_or(12));
    auto scaled = [&](const QString &key, double ratio) {
        const int pinned = whole("font." + key).value_or(0);
        return pinned > 0 ? pinned : qMax(1, int(std::lround(base * ratio)));
    };
    const QHash<QString, int> sizes{
        {"caption", scaled("caption", .833)}, {"small", scaled("body-small", .917)},
        {"body", scaled("body", 1)},          {"subtitle", scaled("subtitle", 1.083)},
        {"title", scaled("title", 1.167)},    {"heading", scaled("heading", 1.333)},
        {"display", scaled("display", 2)},
    };
    // The bar (Style.bar): as tall as the theme or the person makes it at
    // base-size 12, and grown or shrunk with the type unless they say not to.
    const int barSize = whole("bar.size-horizontal").value_or(0);
    const double barScale = flag("bar.scale-with-font", true) ? base / 12.0 : 1;
    const int barHeight = qMax(1, int(std::lround((barSize > 0 ? barSize : 26) * barScale)));
    const int borderWidth = qMax(0, int(std::lround(number("controls.normal-border-width", 1))));

    const bool same = m_loaded && palette == m_palette && dim == m_dim && faint == m_faint && red == m_danger &&
                      good == m_good && yellow == m_warning && controlFill == m_controlFill &&
                      hoverFill == m_hoverFill &&
                      selectedFill == m_selectedFill && pressedFill == m_pressedFill &&
                      controlBorder == m_controlBorder && focusBorder == m_focusBorder &&
                      cardBackground == m_cardBackground && cardBorder == m_cardBorder &&
                      menuBackground == m_menuBackground && menuBorder == m_menuBorder && scrim == m_scrim &&
                      sizes == m_sizes && base == m_fontSize && barHeight == m_barHeight &&
                      borderWidth == m_controlBorderWidth;
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
        m_barHeight = barHeight;
        m_controlBorderWidth = borderWidth;
        m_loaded = true;
        ++m_revision;
        emit changed();
    }
}

// A theme switch replaces symlinks and files, so watch the parents too and
// re-arm before every reading. Watching the nearest existing ancestor means
// installing Omarchy later works without restarting the app.
void Theme::watch() {
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    auto nearestExisting = [](QString path) {
        while (!QFileInfo::exists(path) && path != "/")
            path = QFileInfo(path).absolutePath();
        return path;
    };
    const QString theme = m_directory + "/theme";
    const QString ancestor = nearestExisting(m_directory);
    QStringList candidates{ancestor,               QFileInfo(ancestor).absolutePath(), m_directory, theme,
                           theme + "/colors.toml", theme + "/shell.toml"};
    // The person's settings are made the first time a size is set and then
    // replaced by a new file each time, so their folder is watched as well.
    if (!m_userShell.isEmpty())
        candidates << nearestExisting(QFileInfo(m_userShell).absolutePath()) << m_userShell;
    candidates.removeDuplicates();
    for (const QString &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
}
