#pragma once
#include <QJsonObject>
class QWidget;
namespace QaNativePanel {
// Called only during a QA-owned invocation of the real directory picker, on the GUI thread.
QJsonObject command(QWidget *owner, const QString &op, const QString &token,
                    const QString &expected, QString *error);
void retire();
}
