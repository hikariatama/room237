#pragma once
// Background preparation of an open library: previews, metadata and perceptual hashes, on bounded worker pools.
#include <QString>

namespace core::preload {

enum class TaskPriority { High, Low };

bool enqueueThumbTask(const QString &path, const QString &thumbDir, bool totalsAccounted, TaskPriority priority);
bool enqueueMetaTask(const QString &path, bool totalsAccounted, TaskPriority priority);
void enqueuePreload(const QString &dir);
void dropPreloadForPath(const QString &prefix);
bool setActiveRoot(const QString &root);
void startPreloaderWorker();
bool isPreloading();
bool lockUntilPreloaded();
void setAllowOpen(bool allow);
bool artifactsMissing(const QString &dir);
void applyWorkerLimits();
void enableEvents();

}  // namespace core::preload
