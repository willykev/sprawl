#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

// The colors a board is drawn with. A plain value, so the canvas, the
// exporter, and the tests can all be handed one without a Theme.
struct Palette {
    bool dark = true;
    QColor background{"#1a1b26"}; // the canvas
    QColor panel{"#13141c"};      // rail and status line
    QColor raised{"#24283b"};     // shape fills, status segments
    QColor foreground{"#a9b1d6"};
    QColor bright{"#c0caf5"};
    QColor muted{"#414868"};     // frame borders, separators
    QColor selection{"#292e42"}; // the dot grid
    QColor accent{"#7aa2f7"};
    QColor accentInk{"#1a1b26"};
    QColor ink{"#1a1b26"}; // text on a note
    QHash<QString, QColor> swatches;
    QHash<QString, QColor> notes;

    // A named color as the theme gives it, for borders, strokes and text.
    QColor swatch(const QString &name, const QColor &fallback) const {
        const auto found = swatches.constFind(name);
        return found == swatches.constEnd() ? fallback : *found;
    }
    // The same color as note paper: toned toward the canvas until dark text
    // reads on it.
    QColor note(const QString &name) const {
        const auto found = notes.constFind(name);
        return found == notes.constEnd() ? notes.value(QStringLiteral("yellow"), QColor("#b48e5a")) : *found;
    }

    static Palette fallback();
    bool operator==(const Palette &other) const = default;
};

// How Omarchy looks right now, followed live: the whole palette of the
// current theme (colors.toml), the control states and type size the Omarchy
// shell draws its own panels with (shell.toml), and Hyprland's window
// rounding, border width, and whether it animates.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor panel READ panel NOTIFY changed)
    Q_PROPERTY(QColor raised READ raised NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor bright READ bright NOTIFY changed)
    Q_PROPERTY(QColor dim READ dim NOTIFY changed)
    Q_PROPERTY(QColor faint READ faint NOTIFY changed)
    Q_PROPERTY(QColor muted READ muted NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor accentInk READ accentInk NOTIFY changed)
    Q_PROPERTY(QColor ink READ ink NOTIFY changed)
    Q_PROPERTY(QColor danger READ danger NOTIFY changed)
    Q_PROPERTY(QColor good READ good NOTIFY changed)
    Q_PROPERTY(QColor warning READ warning NOTIFY changed)
    // The states every control in the Omarchy shell's own kit shares.
    Q_PROPERTY(QColor controlFill READ controlFill NOTIFY changed)
    Q_PROPERTY(QColor hoverFill READ hoverFill NOTIFY changed)
    Q_PROPERTY(QColor selectedFill READ selectedFill NOTIFY changed)
    Q_PROPERTY(QColor pressedFill READ pressedFill NOTIFY changed)
    Q_PROPERTY(QColor controlBorder READ controlBorder NOTIFY changed)
    Q_PROPERTY(QColor focusBorder READ focusBorder NOTIFY changed)
    // Popups (toolbars) and menus (dialogs), as the shell draws its own.
    Q_PROPERTY(QColor cardBackground READ cardBackground NOTIFY changed)
    Q_PROPERTY(QColor cardBorder READ cardBorder NOTIFY changed)
    Q_PROPERTY(QColor menuBackground READ menuBackground NOTIFY changed)
    Q_PROPERTY(QColor menuBorder READ menuBorder NOTIFY changed)
    Q_PROPERTY(QColor scrim READ scrim NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(int borderWidth READ borderWidth NOTIFY changed)
    Q_PROPERTY(int controlBorderWidth READ controlBorderWidth NOTIFY changed)
    Q_PROPERTY(bool reduceMotion READ reduceMotion NOTIFY changed)
    Q_PROPERTY(QString fontFamily READ fontFamily CONSTANT)
    Q_PROPERTY(int fontSize READ fontSize NOTIFY changed)
    Q_PROPERTY(QStringList swatchNames READ swatchNames CONSTANT)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    const Palette &palette() const { return m_palette; }

    QColor background() const { return m_palette.background; }
    QColor panel() const { return m_palette.panel; }
    QColor raised() const { return m_palette.raised; }
    QColor foreground() const { return m_palette.foreground; }
    QColor bright() const { return m_palette.bright; }
    QColor dim() const { return m_dim; }
    QColor faint() const { return m_faint; }
    QColor muted() const { return m_palette.muted; }
    QColor accent() const { return m_palette.accent; }
    QColor accentInk() const { return m_palette.accentInk; }
    QColor ink() const { return m_palette.ink; }
    QColor danger() const { return m_danger; }
    QColor good() const { return m_good; }
    QColor warning() const { return m_warning; }
    QColor controlFill() const { return m_controlFill; }
    QColor hoverFill() const { return m_hoverFill; }
    QColor selectedFill() const { return m_selectedFill; }
    QColor pressedFill() const { return m_pressedFill; }
    QColor controlBorder() const { return m_controlBorder; }
    QColor focusBorder() const { return m_focusBorder; }
    QColor cardBackground() const { return m_cardBackground; }
    QColor cardBorder() const { return m_cardBorder; }
    QColor menuBackground() const { return m_menuBackground; }
    QColor menuBorder() const { return m_menuBorder; }
    QColor scrim() const { return m_scrim; }
    bool dark() const { return m_palette.dark; }
    int radius() const { return m_radius; }
    int borderWidth() const { return m_borderWidth; }
    int controlBorderWidth() const { return m_controlBorderWidth; }
    bool reduceMotion() const { return m_reduceMotion; }
    // The fontconfig alias `omarchy font set` points at the Omarchy font.
    QString fontFamily() const { return QStringLiteral("monospace"); }
    int fontSize() const { return m_fontSize; }
    QStringList swatchNames() const { return swatchOrder(); }

    Q_INVOKABLE QColor swatch(const QString &name) const { return m_palette.swatch(name, m_palette.foreground); }
    Q_INVOKABLE QColor note(const QString &name) const { return m_palette.note(name); }
    // The theme's own dark or light, whichever reads on the color: for text
    // on a badge.
    Q_INVOKABLE QColor inkOn(const QColor &color) const;
    // A size from the shell's type scale: caption, small, body, subtitle,
    // title, heading, display.
    Q_INVOKABLE int size(const QString &token) const;

    // The colors a person picks from, in the order the toolbar shows them.
    static QStringList swatchOrder();
    // A TOML file as "section.key" -> value; keys before any section have no
    // prefix. Enough of TOML for theme files: strings, numbers, comments.
    static QHash<QString, QString> readToml(const QString &path);
    static QColor mix(const QColor &base, const QColor &other, double amount);
    static double luminance(const QColor &color);
    // Black or white, whichever reads on the color.
    static QColor contrastInk(const QColor &color);
    // The swatch toned toward the canvas until `ink` reads on it.
    static QColor notePaper(const QColor &swatch, const QColor &canvas, bool dark);

signals:
    void changed();

private:
    void reload();
    void askHyprland();

    QString m_directory;
    Palette m_palette = Palette::fallback();
    QColor m_dim, m_faint, m_danger, m_good, m_warning;
    QColor m_controlFill, m_hoverFill, m_selectedFill, m_pressedFill, m_controlBorder, m_focusBorder;
    QColor m_cardBackground, m_cardBorder, m_menuBackground, m_menuBorder, m_scrim;
    QHash<QString, int> m_sizes;
    int m_fontSize = 12;
    int m_controlBorderWidth = 1;
    int m_radius = 0;
    int m_borderWidth = 2;
    bool m_reduceMotion = false;
    bool m_loaded = false;

    QFileSystemWatcher m_watcher;
    QTimer m_debounce, m_poll;
    QString m_hyprctl;
    QProcess m_roundingProcess, m_borderProcess, m_animationsProcess;
};
