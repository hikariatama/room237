#include "accessibility.h"
#include "gallery.h"
#include "i18n.h"
#include "sidebar.h"
#include <QAccessibleActionInterface>
namespace {
class RowAccessible : public QAccessibleInterface, public QAccessibleActionInterface {
public:
  RowAccessible(VirtualListAccessible *list, int index) : list_(list), index_(index) {}
  bool isValid() const override { return list_ && index_ < list_->source().count(); }
  QObject *object() const override { return nullptr; }
  QWindow *window() const override { return list_ ? list_->window() : nullptr; }
  QAccessibleInterface *parent() const override { return list_; }
  QAccessibleInterface *child(int) const override { return nullptr; }
  QAccessibleInterface *childAt(int, int) const override { return nullptr; }
  int childCount() const override { return 0; }
  int indexOfChild(const QAccessibleInterface *) const override { return -1; }
  QString text(QAccessible::Text kind) const override {
    if (!isValid()) return {};
    if (kind == QAccessible::Name) return list_->source().name(index_);
    if (kind == QAccessible::Description && list_->source().description) return list_->source().description(index_);
    return {};
  }
  void setText(QAccessible::Text, const QString &) override {}
  QRect rect() const override { return isValid() ? list_->source().rect(index_) : QRect(); }
  QAccessible::Role role() const override { return QAccessible::ListItem; }
  QAccessible::State state() const override {
    QAccessible::State state;
    state.selectable = true;
    state.focusable = true;
    if (isValid() && list_->source().selected(index_)) state.selected = true;
    if (isValid() && rect().isEmpty()) state.invisible = true;
    return state;
  }
  void *interface_cast(QAccessible::InterfaceType type) override { return type == QAccessible::ActionInterface ? static_cast<QAccessibleActionInterface *>(this) : nullptr; }
  QStringList actionNames() const override { return {pressAction()}; }
  void doAction(const QString &name) override {
    if (name == pressAction() && isValid()) list_->source().activate(index_);
  }
  QStringList keyBindingsForAction(const QString &) const override { return {}; }
private:
  VirtualListAccessible *list_;
  int index_;
};
}
VirtualListAccessible::VirtualListAccessible(QWidget *widget, const QString &name, VirtualListSource source) : QAccessibleWidget(widget, QAccessible::List), name_(name), source_(std::move(source)) {}
VirtualListAccessible::~VirtualListAccessible() {
  for (QAccessible::Id id : std::as_const(rows_)) QAccessible::deleteAccessibleInterface(id);
}
int VirtualListAccessible::childCount() const { return source_.count(); }
QAccessibleInterface *VirtualListAccessible::child(int index) const {
  if (index < 0 || index >= source_.count()) return nullptr;
  auto it = rows_.constFind(index);
  if (it != rows_.cend()) return QAccessible::accessibleInterface(*it);
  auto *row = new RowAccessible(const_cast<VirtualListAccessible *>(this), index);
  rows_.insert(index, QAccessible::registerAccessibleInterface(row));
  return row;
}
int VirtualListAccessible::indexOfChild(const QAccessibleInterface *child) const {
  for (auto it = rows_.cbegin(); it != rows_.cend(); ++it)
    if (QAccessible::accessibleInterface(it.value()) == child) return it.key();
  return -1;
}
QAccessibleInterface *VirtualListAccessible::childAt(int x, int y) const {
  const int count = source_.count();
  for (int i = 0; i < count; i++)
    if (source_.rect(i).contains(x, y)) return child(i);
  return nullptr;
}
QString VirtualListAccessible::text(QAccessible::Text kind) const {
  if (kind == QAccessible::Name) return name_;
  return QAccessibleWidget::text(kind);
}
void *VirtualListAccessible::interface_cast(QAccessible::InterfaceType type) {
  return type == QAccessible::SelectionInterface ? static_cast<QAccessibleSelectionInterface *>(this) : QAccessibleWidget::interface_cast(type);
}
int VirtualListAccessible::selectedItemCount() const {
  int selected = 0;
  for (int i = 0, n = source_.count(); i < n; i++) selected += source_.selected(i);
  return selected;
}
QList<QAccessibleInterface *> VirtualListAccessible::selectedItems() const {
  QList<QAccessibleInterface *> items;
  for (int i = 0, n = source_.count(); i < n; i++)
    if (source_.selected(i)) items << child(i);
  return items;
}
bool VirtualListAccessible::isSelected(QAccessibleInterface *row) const {
  const int index = indexOfChild(row);
  return index >= 0 && source_.selected(index);
}
bool VirtualListAccessible::select(QAccessibleInterface *row) {
  const int index = indexOfChild(row);
  if (index < 0) return false;
  source_.activate(index);
  return true;
}
static QAccessibleInterface *factory(const QString &, QObject *object) {
  if (auto *tree = qobject_cast<AlbumTree *>(object)) return new VirtualListAccessible(tree, t("album.expandHint"), tree->accessibleSource());
  if (auto *gallery = qobject_cast<GalleryView *>(object)) return new VirtualListAccessible(gallery, gallery->accessibleName(), gallery->accessibleSource());
  return nullptr;
}
void installAccessibilityFactory() { QAccessible::installFactory(factory); }
