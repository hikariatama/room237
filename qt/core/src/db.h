#pragma once
// Thin RAII layer over the SQLite C API. Every failure throws core::Error with SQLite's message.
#include "fs.h"

#include <QByteArray>
#include <QString>
#include <functional>
#include <optional>
#include <variant>

struct sqlite3;
struct sqlite3_stmt;

namespace core::db {

using Value = std::variant<std::monostate, qint64, double, QString, QByteArray>;

class Statement {
public:
  Statement(sqlite3 *db, const QString &sql);
  ~Statement();
  Statement(Statement &&other) noexcept;
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  Statement &bind(int index, const Value &value);
  Statement &bind(int index, const QString &value) { return bind(index, Value(value)); }
  Statement &bind(int index, const char *value) { return bind(index, Value(QString::fromUtf8(value))); }
  Statement &bind(int index, qint64 value) { return bind(index, Value(value)); }
  Statement &bind(int index, int value) { return bind(index, Value(qint64(value))); }
  Statement &bind(int index, const QByteArray &value) { return bind(index, Value(value)); }
  Statement &bindNull(int index) { return bind(index, Value()); }
  bool step();  // true while a row is available.
  void reset();
  int execute();  // Steps to completion; returns the number of changed rows.
  bool isNull(int column) const;
  Value value(int column) const;
  qint64 integer(int column) const;
  double real(int column) const;
  QString text(int column) const;
  QByteArray blob(int column) const;
  std::optional<QString> optionalText(int column) const;
  std::optional<qint64> optionalInteger(int column) const;
private:
  sqlite3 *db_ = nullptr;
  sqlite3_stmt *stmt_ = nullptr;
};

enum class Open { ReadWrite, ReadOnly, Create };

class Connection {
public:
  Connection(const QString &path, Open mode = Open::Create);
  static Connection memory();
  ~Connection();
  Connection(Connection &&other) noexcept;
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;
  void close();
  void executeBatch(const QString &sql);
  int execute(const QString &sql, std::initializer_list<Value> params = {});
  Statement prepare(const QString &sql);
  qint64 pragmaInteger(const QString &name);
  bool readOnly() const;
  void busyTimeout(int ms);
  qint64 lastInsertRowid() const;
  qint64 changes() const;
  // Calls `handler` every `instructions` VM steps; returning true interrupts the running statement.
  void progressHandler(int instructions, std::function<bool()> handler);
  void scalarFunction(const char *name, int args, std::function<Value(const std::vector<Value> &)> function);
  sqlite3 *handle() const { return db_; }
private:
  Connection() = default;
  sqlite3 *db_ = nullptr;
  std::function<bool()> progress_;
  std::vector<std::unique_ptr<std::function<Value(const std::vector<Value> &)>>> functions_;
};

enum class Behavior { Deferred, Immediate, Exclusive };
// Rolls back unless committed.
class Transaction {
public:
  explicit Transaction(Connection &connection, Behavior behavior = Behavior::Deferred);
  ~Transaction();
  void commit();
  Connection &connection() { return *connection_; }
private:
  Connection *connection_;
  bool done_ = false;
};

}  // namespace core::db
