#pragma once
// Assistive access for the drawn lists. They expose plain list items: Qt 6.11's cocoa bridge crashes reading
// AXSelectedChildren of its table-based tree, and these interfaces never go through that code.
#include <QAccessible>
#include <QAccessibleSelectionInterface>
#include <QAccessibleWidget>
#include <QHash>
#include <functional>
struct VirtualListSource {
  std::function<int()> count;
  std::function<QString(int)> name;
  std::function<QString(int)> description;
  std::function<QRect(int)> rect;  // Global coordinates.
  std::function<bool(int)> selected;
  std::function<void(int)> activate;
};
class VirtualListAccessible : public QAccessibleWidget, public QAccessibleSelectionInterface {
public:
  VirtualListAccessible(QWidget *widget, const QString &name, VirtualListSource source);
  ~VirtualListAccessible() override;
  int childCount() const override;
  QAccessibleInterface *child(int index) const override;
  int indexOfChild(const QAccessibleInterface *child) const override;
  QAccessibleInterface *childAt(int x, int y) const override;
  QString text(QAccessible::Text kind) const override;
  void *interface_cast(QAccessible::InterfaceType type) override;
  int selectedItemCount() const override;
  QList<QAccessibleInterface *> selectedItems() const override;
  bool isSelected(QAccessibleInterface *child) const override;
  bool select(QAccessibleInterface *child) override;
  bool unselect(QAccessibleInterface *) override { return false; }
  bool selectAll() override { return false; }
  bool clear() override { return false; }
  const VirtualListSource &source() const { return source_; }
private:
  QString name_;
  VirtualListSource source_;
  mutable QHash<int, QAccessible::Id> rows_;
};
void installAccessibilityFactory();
