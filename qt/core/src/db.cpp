#include "db.h"

#include <sqlite3.h>

namespace core::db {

namespace {
[[noreturn]] void raise(sqlite3 *db, int code) {
  fail(QString::fromUtf8(db ? sqlite3_errmsg(db) : sqlite3_errstr(code)));
}
void check(sqlite3 *db, int code) {
  if (code != SQLITE_OK) raise(db, code);
}
Value fromSqlite(sqlite3_value *value) {
  switch (sqlite3_value_type(value)) {
  case SQLITE_INTEGER: return qint64(sqlite3_value_int64(value));
  case SQLITE_FLOAT: return sqlite3_value_double(value);
  case SQLITE_TEXT: return QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_value_text(value)), sqlite3_value_bytes(value));
  case SQLITE_BLOB: return QByteArray(static_cast<const char *>(sqlite3_value_blob(value)), sqlite3_value_bytes(value));
  default: return std::monostate();
  }
}
}  // namespace

Statement::Statement(sqlite3 *db, const QString &sql) : db_(db) {
  const QByteArray text = sql.toUtf8();
  check(db_, sqlite3_prepare_v2(db_, text.constData(), int(text.size()), &stmt_, nullptr));
}
Statement::~Statement() {
  if (stmt_) sqlite3_finalize(stmt_);
}
Statement::Statement(Statement &&other) noexcept : db_(other.db_), stmt_(other.stmt_) { other.stmt_ = nullptr; }
Statement &Statement::bind(int index, const Value &value) {
  int code = SQLITE_OK;
  if (std::holds_alternative<std::monostate>(value)) code = sqlite3_bind_null(stmt_, index);
  else if (auto *i = std::get_if<qint64>(&value)) code = sqlite3_bind_int64(stmt_, index, *i);
  else if (auto *d = std::get_if<double>(&value)) code = sqlite3_bind_double(stmt_, index, *d);
  else if (auto *s = std::get_if<QString>(&value)) {
    const QByteArray utf8 = s->toUtf8();
    code = sqlite3_bind_text64(stmt_, index, utf8.constData(), sqlite3_uint64(utf8.size()), SQLITE_TRANSIENT, SQLITE_UTF8);
  } else if (auto *b = std::get_if<QByteArray>(&value)) {
    code = sqlite3_bind_blob64(stmt_, index, b->constData(), sqlite3_uint64(b->size()), SQLITE_TRANSIENT);
  }
  check(db_, code);
  return *this;
}
bool Statement::step() {
  const int code = sqlite3_step(stmt_);
  if (code == SQLITE_ROW) return true;
  if (code == SQLITE_DONE) return false;
  raise(db_, code);
}
void Statement::reset() {
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
}
int Statement::execute() {
  while (step()) {
  }
  return sqlite3_changes(db_);
}
Value Statement::value(int column) const {
  switch (sqlite3_column_type(stmt_, column)) {
  case SQLITE_INTEGER: return integer(column);
  case SQLITE_FLOAT: return real(column);
  case SQLITE_TEXT: return text(column);
  case SQLITE_BLOB: return blob(column);
  default: return std::monostate();
  }
}
bool Statement::isNull(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }
qint64 Statement::integer(int column) const { return sqlite3_column_int64(stmt_, column); }
double Statement::real(int column) const { return sqlite3_column_double(stmt_, column); }
QString Statement::text(int column) const {
  const auto *data = reinterpret_cast<const char *>(sqlite3_column_text(stmt_, column));
  return data ? QString::fromUtf8(data, sqlite3_column_bytes(stmt_, column)) : QString();
}
QByteArray Statement::blob(int column) const {
  const void *data = sqlite3_column_blob(stmt_, column);
  return data ? QByteArray(static_cast<const char *>(data), sqlite3_column_bytes(stmt_, column)) : QByteArray();
}
std::optional<QString> Statement::optionalText(int column) const {
  if (isNull(column)) return std::nullopt;
  return text(column);
}
std::optional<qint64> Statement::optionalInteger(int column) const {
  if (isNull(column)) return std::nullopt;
  return integer(column);
}

Connection::Connection(const QString &path, Open mode) {
  int flags = SQLITE_OPEN_FULLMUTEX;
  switch (mode) {
  case Open::ReadWrite: flags |= SQLITE_OPEN_READWRITE; break;
  case Open::ReadOnly: flags |= SQLITE_OPEN_READONLY; break;
  case Open::Create: flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE; break;
  }
  const int code = sqlite3_open_v2(path.toUtf8().constData(), &db_, flags, nullptr);
  if (code != SQLITE_OK) {
    const QString message = QString::fromUtf8(db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(code));
    sqlite3_close(db_);
    db_ = nullptr;
    fail(message);
  }
  sqlite3_extended_result_codes(db_, 0);
}
Connection Connection::memory() { return Connection(QStringLiteral(":memory:")); }
Connection::~Connection() { close(); }
Connection::Connection(Connection &&other) noexcept : db_(other.db_), progress_(std::move(other.progress_)), functions_(std::move(other.functions_)) { other.db_ = nullptr; }
void Connection::close() {
  if (!db_) return;
  const int code = sqlite3_close(db_);
  if (code != SQLITE_OK) sqlite3_close_v2(db_);
  db_ = nullptr;
}
void Connection::executeBatch(const QString &sql) {
  char *message = nullptr;
  const int code = sqlite3_exec(db_, sql.toUtf8().constData(), nullptr, nullptr, &message);
  if (code != SQLITE_OK) {
    const QString text = message ? QString::fromUtf8(message) : QString::fromUtf8(sqlite3_errstr(code));
    sqlite3_free(message);
    fail(text);
  }
}
int Connection::execute(const QString &sql, std::initializer_list<Value> params) {
  Statement statement(db_, sql);
  int index = 1;
  for (const Value &value : params) statement.bind(index++, value);
  return statement.execute();
}
Statement Connection::prepare(const QString &sql) { return Statement(db_, sql); }
qint64 Connection::pragmaInteger(const QString &name) {
  Statement statement(db_, QStringLiteral("PRAGMA %1").arg(name));
  if (!statement.step()) fail(QStringLiteral("Query returned no rows"));
  return statement.integer(0);
}
bool Connection::readOnly() const { return sqlite3_db_readonly(db_, "main") == 1; }
void Connection::busyTimeout(int ms) { check(db_, sqlite3_busy_timeout(db_, ms)); }
qint64 Connection::lastInsertRowid() const { return sqlite3_last_insert_rowid(db_); }
qint64 Connection::changes() const { return sqlite3_changes64(db_); }
void Connection::progressHandler(int instructions, std::function<bool()> handler) {
  progress_ = std::move(handler);
  if (!progress_) {
    sqlite3_progress_handler(db_, 0, nullptr, nullptr);
    return;
  }
  sqlite3_progress_handler(db_, instructions, [](void *data) -> int { return (*static_cast<std::function<bool()> *>(data))() ? 1 : 0; }, &progress_);
}
void Connection::scalarFunction(const char *name, int args, std::function<Value(const std::vector<Value> &)> function) {
  functions_.push_back(std::make_unique<std::function<Value(const std::vector<Value> &)>>(std::move(function)));
  auto *callback = functions_.back().get();
  check(db_, sqlite3_create_function_v2(
                 db_, name, args, SQLITE_UTF8 | SQLITE_DETERMINISTIC, callback,
                 [](sqlite3_context *context, int argc, sqlite3_value **argv) {
                   auto &fn = *static_cast<std::function<Value(const std::vector<Value> &)> *>(sqlite3_user_data(context));
                   std::vector<Value> values;
                   values.reserve(size_t(argc));
                   for (int i = 0; i < argc; ++i) values.push_back(fromSqlite(argv[i]));
                   try {
                     const Value result = fn(values);
                     if (std::holds_alternative<std::monostate>(result)) sqlite3_result_null(context);
                     else if (auto *i = std::get_if<qint64>(&result)) sqlite3_result_int64(context, *i);
                     else if (auto *d = std::get_if<double>(&result)) sqlite3_result_double(context, *d);
                     else if (auto *s = std::get_if<QString>(&result)) {
                       const QByteArray utf8 = s->toUtf8();
                       sqlite3_result_text64(context, utf8.constData(), sqlite3_uint64(utf8.size()), SQLITE_TRANSIENT, SQLITE_UTF8);
                     } else if (auto *b = std::get_if<QByteArray>(&result)) sqlite3_result_blob64(context, b->constData(), sqlite3_uint64(b->size()), SQLITE_TRANSIENT);
                   } catch (const std::exception &error) {
                     sqlite3_result_error(context, error.what(), -1);
                   }
                 },
                 nullptr, nullptr, nullptr));
}

Transaction::Transaction(Connection &connection, Behavior behavior) : connection_(&connection) {
  const char *sql = behavior == Behavior::Immediate ? "BEGIN IMMEDIATE" : behavior == Behavior::Exclusive ? "BEGIN EXCLUSIVE" : "BEGIN DEFERRED";
  connection.executeBatch(QString::fromLatin1(sql));
}
Transaction::~Transaction() {
  if (!done_) {
    try {
      connection_->executeBatch(QStringLiteral("ROLLBACK"));
    } catch (...) {
    }
  }
}
void Transaction::commit() {
  connection_->executeBatch(QStringLiteral("COMMIT"));
  done_ = true;
}

}  // namespace core::db
