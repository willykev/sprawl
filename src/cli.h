#pragma once

#include <QString>
#include <QStringList>

// The commands that need no window, for scripts and coding agents: they
// read, change, check and export boards with no display. Their output stays
// in English whatever the desktop's language is.
bool isCliCommand(const QString &word);
QString cliSummary();
int runCli(const QStringList &arguments);

// What is printed of text that came from a board or a file. Whoever wrote
// it, a terminal obeys the control characters in it, so none of them get
// there but the new line and the tab. In JSON they are written as escapes
// instead, which read back as the same text.
QString printableText(const QString &text);
QByteArray printableJson(const QByteArray &json);
