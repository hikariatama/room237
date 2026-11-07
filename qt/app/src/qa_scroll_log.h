#pragma once
class MainWindow;
class QString;
// QA builds only: writes every wheel event (receiver, phase, deltas), mouse press and drag and every scroll position change of the album
// list and the gallery to a file, one line each. Enabled by ROOM237_SCROLL_LOG=<path>.
void installScrollLog(MainWindow *window, const QString &path);
