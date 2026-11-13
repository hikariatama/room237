#pragma once
// Tree leases: ".room237-tree.lock" in the chosen root, outside metadata/cache cleanup. Readers take every existing
// ancestor marker shared; renaming or moving an album takes its root's marker exclusively.
#include "fs.h"

#include <memory>
#include <set>

namespace core::album_tree {

class TreeLease {
public:
  TreeLease() = default;
  ~TreeLease();
  TreeLease(TreeLease &&) = default;
  TreeLease &operator=(TreeLease &&) = default;
  void ensureCurrent(const QStringList &albums) const;
private:
  friend std::unique_ptr<TreeLease> acquire(const QStringList &, const QString *, bool);
  std::vector<std::pair<QString, std::shared_ptr<FileLock>>> readers_;
  std::optional<std::pair<QString, std::unique_ptr<FileLock>>> writer_;
  std::set<QString> markers_;
};

std::unique_ptr<TreeLease> read(const QStringList &albums);
std::unique_ptr<TreeLease> tryRead(const QStringList &albums);  // nullptr when busy.
std::unique_ptr<TreeLease> write(const QString &root);
void initialize(const QString &root);

}  // namespace core::album_tree
