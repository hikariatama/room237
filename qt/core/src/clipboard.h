#pragma once
#include <QStringList>

namespace core::clipboard {

// Puts the files on the system clipboard as file references, as the platform's file manager copies them.
void setClipboardFiles(const QStringList &paths);

}  // namespace core::clipboard
