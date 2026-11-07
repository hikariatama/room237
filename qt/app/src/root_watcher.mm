#include "root_watcher.h"
#include "qatrace.h"
#include <CoreServices/CoreServices.h>
#include <QMetaObject>
#include <dispatch/dispatch.h>
#include <cstring>
// Per-file events: the core keeps writing .room237-* entries directly into album folders.
bool RootWatcher::isDerivedPath(const char *path) {
  if (std::strstr(path, "/.room237")) return true;
  const char *name = std::strrchr(path, '/');
  return name && name[1] == '.';
}
void RootWatcher::callback(const void *, void *self, size_t count, void *paths, const unsigned *flags, const unsigned long long *ids) {
  auto *watcher = static_cast<RootWatcher *>(self);
  auto **names = static_cast<const char **>(paths);
  QString last;
  for (size_t i = 0; i < count; i++) {
    QString directory;
    if (flags[i] & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped)) {
      directory = QString::fromUtf8(watcher->rootBytes_);  // Events were lost: treat it as a change of the whole tree.
    } else {
      if (isDerivedPath(names[i])) continue;
      const char *name = std::strrchr(names[i], '/');
      if (!name) continue;
      directory = QString::fromUtf8(names[i], name - names[i]);
    }
    if (directory == last) continue;
    last = directory;
    qaTrace("watch-change", QStringLiteral("flags=%1 id=%2 now=%3 from %4").arg(flags[i], 0, 16).arg(ids[i]).arg(FSEventsGetCurrentEventId()).arg(QString::fromUtf8(names[i])));
    QMetaObject::invokeMethod(watcher, [watcher, directory] { emit watcher->changed(directory); }, Qt::QueuedConnection);
  }
}
void RootWatcher::startNative(const QString &root) {
  const QByteArray path = root.toUtf8();
  rootBytes_ = path;
  CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, path.constData(), kCFStringEncodingUTF8);
  CFArrayRef list = CFArrayCreate(kCFAllocatorDefault, reinterpret_cast<const void **>(&name), 1, &kCFTypeArrayCallBacks);
  FSEventStreamContext context{0, this, nullptr, nullptr, nullptr};
  auto stream = FSEventStreamCreate(kCFAllocatorDefault, reinterpret_cast<FSEventStreamCallback>(&RootWatcher::callback), &context, list, kFSEventStreamEventIdSinceNow, 0.2, kFSEventStreamCreateFlagFileEvents);
  CFRelease(list);
  CFRelease(name);
  auto queue = dispatch_queue_create("room237.root-watcher", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
  queue_ = queue;
  FSEventStreamSetDispatchQueue(stream, queue);
  FSEventStreamStart(stream);
  stream_ = stream;
}
void RootWatcher::stopNative() {
  if (!stream_) return;
  auto stream = static_cast<FSEventStreamRef>(stream_);
  FSEventStreamStop(stream);
  FSEventStreamInvalidate(stream);
  FSEventStreamRelease(stream);
  stream_ = nullptr;
  // A callback already running on the queue still uses this object.
  auto queue = static_cast<dispatch_queue_t>(queue_);
  dispatch_sync(queue, ^{});
  dispatch_release(queue);
  queue_ = nullptr;
}
