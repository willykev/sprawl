#pragma once

#include <QCommandLineParser>
#include <QDir>
#include <QFile>

#include "theme.h"

// Whether `sprawl open`, with these arguments, asks anything of the window
// it opens: a board, or any option at all (a theme, a size, a tool, a zoom,
// a selection, a panel, another window, a screenshot, a benchmark), Qt's own
// among them, or boards kept somewhere else (SPRAWL_HOME). With nothing
// asked, launching Sprawl again only brings the window that is open to the
// front, as a board is one thing to look at. That window started with what
// it was given, and with the boards it was given, so whatever is asked now
// would be left undone there, or done to the wrong boards: it is done in a
// window of its own instead. Qt takes its own options (a window's size and
// title, right to left) out of the arguments before the parser sees them,
// so `qtOptions` says whether there were any. Apart from main(), so tests
// can ask it.
inline bool wantsOwnWindow(const QCommandLineParser &args, bool qtOptions = false) {
    return qtOptions || !args.positionalArguments().value(1).isEmpty() || !args.optionNames().isEmpty() ||
           !qEnvironmentVariableIsEmpty("SPRAWL_HOME");
}

// Where Omarchy keeps its themes: the person's own, then those it comes with.
inline QStringList themesFolders() {
    return {QDir::homePath() + "/.config/omarchy/themes", "/usr/share/omarchy/themes"};
}

// The folder of the theme `sprawl open --theme` asks for, or nothing when
// there is none. As with boards, a theme's name is that theme, whatever the
// folder Sprawl is started from has in it, and anything else is a path: what
// has a slash in it, or is the name of no theme. A test says where themes
// are kept, so it needs none of the person's.
inline QString findTheme(const QString &asked, const QStringList &folders = themesFolders()) {
    // Nothing named is no theme, not a folder themes are kept in.
    if (asked.isEmpty())
        return {};
    QStringList candidates;
    // A name is the name of a folder among the themes, never a way out of them.
    if (!asked.contains('/') && asked != "." && asked != "..")
        for (const QString &folder : folders)
            candidates << folder + "/" + asked;
    candidates << asked;
    for (const QString &candidate : std::as_const(candidates))
        if (QFile::exists(candidate + "/colors.toml"))
            return QDir::cleanPath(QDir::current().absoluteFilePath(candidate));
    return {};
}

// The person's own settings for the shell that `sprawl open` lays over the
// theme it shows, as the shell lays them over whichever theme is current:
// theirs over the desktop's, and over another asked for with --theme, which
// then looks as the shell would show it here; none over --default-theme,
// which shows Omarchy as it ships, the same on every machine. A test says
// where the person's are, so it needs none of theirs.
inline QString userShellFor(const QCommandLineParser &args, const QString &own = Theme::userShellFile()) {
    return args.isSet("default-theme") ? QString() : own;
}
