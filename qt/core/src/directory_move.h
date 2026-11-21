#pragma once
#include "fs.h"

namespace core::directory_move {
using Hook = std::function<void(const QString &)>;
// The caller holds the library's exclusive tree lease and has stopped producers/recovered media journals.
void move(const QString &root, const Directory &library, const QString &sourceRelative, const QString &targetRelative,
          const Directory &admitted, const Hook &checkpoint = [](const QString &) {});
// Takes the exclusive tree lease itself. A retained claim rolls back to its original relative location; an already
// published directory stays in place. Ambiguous entries are never overwritten or removed.
void recover(const QString &root, const Hook &checkpoint = [](const QString &) {});
}  // namespace core::directory_move
