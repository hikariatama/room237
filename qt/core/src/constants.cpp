#include "constants.h"

#include "fs.h"

namespace core::constants {

QString lowerExtension(const QString &p) { return path::extension(p).toLower(); }
bool isImage(const QString &p) { return isImageExtension(lowerExtension(p)); }
bool isVideo(const QString &p) { return isVideoExtension(lowerExtension(p)); }
bool isMedia(const QString &p) {
  const QString ext = lowerExtension(p);
  return isImageExtension(ext) || isVideoExtension(ext) || ext == QLatin1String("r237");
}
bool isEncryptedName(const QString &p) { return lowerExtension(p) == QLatin1String("r237"); }

}  // namespace core::constants
