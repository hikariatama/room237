// Small QtCore-only probe: tests the actual fence implementation without starting the GUI/core.
#include "qa_fence.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  std::string line;
  while (std::getline(std::cin, line)) {
    const QJsonObject request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    const QString op = request.value("op").toString();
    bool allowed = false;
    if (op == "inside") allowed = QaFence::inside(request.value("path").toString(), request.value("fence").toString(), request.value("missing").toBool());
    if (op == "roots") allowed = QaFence::rootsAllowed(request.value("state").toObject());
    if (op == "params") allowed = QaFence::methodAllowed(request.value("method").toString()) && QaFence::parametersAllowed(request.value("params").toObject(), request.value("method").toString());
    std::cout << (allowed ? "true" : "false") << std::endl;
  }
}
