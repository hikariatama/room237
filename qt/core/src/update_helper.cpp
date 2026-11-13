// room237-core: the update helper. It has no Qt dependency, so the app can run a copy of it from the profile while
// the install it replaces is swapped. "--version" lets an older app verify a staged install before swapping.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#ifndef ROOM237_VERSION
#define ROOM237_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// Minimal JSON reader for the helper plan: objects, arrays, strings and non-negative integers.
struct Value {
  enum Kind { Null, Number, String, Array, Object, Bool } kind = Null;
  unsigned long long number = 0;
  bool boolean = false;
  std::string text;
  std::vector<Value> items;
  std::map<std::string, Value> fields;
  const Value *get(const std::string &key) const {
    auto it = fields.find(key);
    return it == fields.end() ? nullptr : &it->second;
  }
};

class Parser {
public:
  explicit Parser(const std::string &input) : s_(input) {}
  std::optional<Value> parse() {
    auto value = parseValue();
    space();
    if (!value || i_ != s_.size()) return std::nullopt;
    return value;
  }
private:
  void space() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
  }
  static void appendUtf8(std::string &out, unsigned code) {
    if (code < 0x80) out += char(code);
    else if (code < 0x800) {
      out += char(0xc0 | (code >> 6));
      out += char(0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
      out += char(0xe0 | (code >> 12));
      out += char(0x80 | ((code >> 6) & 0x3f));
      out += char(0x80 | (code & 0x3f));
    } else {
      out += char(0xf0 | (code >> 18));
      out += char(0x80 | ((code >> 12) & 0x3f));
      out += char(0x80 | ((code >> 6) & 0x3f));
      out += char(0x80 | (code & 0x3f));
    }
  }
  std::optional<std::string> parseString() {
    if (s_[i_] != '"') return std::nullopt;
    ++i_;
    std::string out;
    while (i_ < s_.size() && s_[i_] != '"') {
      char c = s_[i_++];
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i_ >= s_.size()) return std::nullopt;
      c = s_[i_++];
      switch (c) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        if (i_ + 4 > s_.size()) return std::nullopt;
        unsigned code = std::stoul(s_.substr(i_, 4), nullptr, 16);
        i_ += 4;
        if (code >= 0xd800 && code < 0xdc00 && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
          const unsigned low = std::stoul(s_.substr(i_ + 2, 4), nullptr, 16);
          i_ += 6;
          code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        }
        appendUtf8(out, code);
        break;
      }
      default: return std::nullopt;
      }
    }
    if (i_ >= s_.size()) return std::nullopt;
    ++i_;
    return out;
  }
  std::optional<Value> parseValue() {
    space();
    if (i_ >= s_.size()) return std::nullopt;
    Value v;
    const char c = s_[i_];
    if (c == '{') {
      v.kind = Value::Object;
      ++i_;
      space();
      if (i_ < s_.size() && s_[i_] == '}') {
        ++i_;
        return v;
      }
      for (;;) {
        space();
        auto key = parseString();
        if (!key) return std::nullopt;
        space();
        if (i_ >= s_.size() || s_[i_++] != ':') return std::nullopt;
        auto item = parseValue();
        if (!item) return std::nullopt;
        v.fields[*key] = *item;
        space();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == '}') {
          ++i_;
          return v;
        }
        return std::nullopt;
      }
    }
    if (c == '[') {
      v.kind = Value::Array;
      ++i_;
      space();
      if (i_ < s_.size() && s_[i_] == ']') {
        ++i_;
        return v;
      }
      for (;;) {
        auto item = parseValue();
        if (!item) return std::nullopt;
        v.items.push_back(*item);
        space();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == ']') {
          ++i_;
          return v;
        }
        return std::nullopt;
      }
    }
    if (c == '"') {
      auto text = parseString();
      if (!text) return std::nullopt;
      v.kind = Value::String;
      v.text = *text;
      return v;
    }
    if (s_.compare(i_, 4, "true") == 0 || s_.compare(i_, 5, "false") == 0) {
      v.kind = Value::Bool;
      v.boolean = s_[i_] == 't';
      i_ += v.boolean ? 4 : 5;
      return v;
    }
    if (s_.compare(i_, 4, "null") == 0) {
      i_ += 4;
      return v;
    }
    if (c >= '0' && c <= '9') {
      v.kind = Value::Number;
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') v.number = v.number * 10 + unsigned(s_[i_++] - '0');
      return v;
    }
    return std::nullopt;
  }
  const std::string &s_;
  size_t i_ = 0;
};

std::string escape(const std::string &text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (unsigned(c) < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof buffer, "\\u%04x", unsigned(c));
      out += buffer;
    } else out += c;
  }
  return out + "\"";
}

struct Plan {
  unsigned long long waitPid = 0, healthTimeoutSecs = 0;
  fs::path install, staged, backup, healthFile, resultFile;
  std::vector<std::string> launch;
};

fs::path pathOf(const std::string &utf8) {
#ifdef _WIN32
  const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
  std::wstring wide(size_t(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), wide.data(), size);
  return fs::path(wide);
#else
  return fs::path(utf8);
#endif
}

std::optional<Plan> readPlan(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string text = buffer.str();
  const auto value = Parser(text).parse();
  if (!value || value->kind != Value::Object) return std::nullopt;
  auto str = [&](const char *key) -> std::optional<std::string> {
    const Value *v = value->get(key);
    if (!v || v->kind != Value::String) return std::nullopt;
    return v->text;
  };
  auto num = [&](const char *key) -> std::optional<unsigned long long> {
    const Value *v = value->get(key);
    if (!v || v->kind != Value::Number) return std::nullopt;
    return v->number;
  };
  Plan plan;
  const auto pid = num("waitPid"), timeout = num("healthTimeoutSecs");
  const auto install = str("install"), staged = str("staged"), backup = str("backup"), health = str("healthFile"), result = str("resultFile");
  const Value *launch = value->get("launch");
  if (!pid || !timeout || !install || !staged || !backup || !health || !result || !launch || launch->kind != Value::Array) return std::nullopt;
  for (const Value &item : launch->items) {
    if (item.kind != Value::String) return std::nullopt;
    plan.launch.push_back(item.text);
  }
  plan.waitPid = *pid;
  plan.healthTimeoutSecs = *timeout;
  plan.install = pathOf(*install);
  plan.staged = pathOf(*staged);
  plan.backup = pathOf(*backup);
  plan.healthFile = pathOf(*health);
  plan.resultFile = pathOf(*result);
  return plan;
}

std::string utf8(const fs::path &p) {
  const auto text = p.u8string();
  return std::string(text.begin(), text.end());
}

bool pidRunning(unsigned long long pid) {
#ifdef _WIN32
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
  if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
  const DWORD wait = WaitForSingleObject(process, 0);
  CloseHandle(process);
  return wait == WAIT_TIMEOUT;
#else
  if (::kill(pid_t(pid), 0) != 0 && errno != EPERM) return false;
  // A zombie has exited and released its files; only its parent has not collected it yet.
  const std::string command = "ps -o stat= -p " + std::to_string(pid);
  FILE *ps = ::popen(command.c_str(), "r");
  if (!ps) return true;
  char state[32] = {};
  const bool read = std::fgets(state, sizeof state, ps) != nullptr;
  ::pclose(ps);
  if (!read) return true;
  const char *p = state;
  while (*p == ' ') ++p;
  return *p != 'Z';
#endif
}

bool waitExit(unsigned long long pid, std::chrono::seconds timeout) {
  const auto start = Clock::now();
  while (pidRunning(pid)) {
    if (Clock::now() - start > timeout) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return true;
}

void removePath(const fs::path &p) {
  std::error_code error;
  const auto status = fs::symlink_status(p, error);
  if (error || status.type() == fs::file_type::not_found) return;
  if (status.type() == fs::file_type::directory) fs::remove_all(p);
  else fs::remove(p);
}

// Renames fail briefly on Windows while a just-exited process releases its files.
void renameWithRetry(const fs::path &from, const fs::path &to) {
  const auto start = Clock::now();
  for (;;) {
    std::error_code error;
    fs::rename(from, to, error);
    if (!error) return;
    if (Clock::now() - start > std::chrono::seconds(15)) throw std::runtime_error(utf8(from) + " -> " + utf8(to) + ": " + error.message());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

struct Child {
#ifdef _WIN32
  HANDLE process = nullptr;
#else
  pid_t pid = -1;
#endif
  bool exited() {
#ifdef _WIN32
    return !process || WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
#else
    if (pid <= 0) return true;
    int status = 0;
    return ::waitpid(pid, &status, WNOHANG) == pid;
#endif
  }
  void kill() {
#ifdef _WIN32
    if (process) {
      TerminateProcess(process, 1);
      WaitForSingleObject(process, 5000);
    }
#else
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      int status = 0;
      ::waitpid(pid, &status, 0);
    }
#endif
  }
};

std::optional<Child> start(const std::vector<std::string> &launch) {
  if (launch.empty()) return std::nullopt;
#ifdef _WIN32
  std::wstring command;
  for (const std::string &part : launch) {
    const std::wstring wide = pathOf(part).wstring();
    if (!command.empty()) command += L' ';
    command += L'"';
    for (wchar_t c : wide) {
      if (c == L'"') command += L'\\';
      command += c;
    }
    command += L'"';
  }
  STARTUPINFOW startup {};
  startup.cb = sizeof startup;
  PROCESS_INFORMATION info {};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) return std::nullopt;
  CloseHandle(info.hThread);
  Child child;
  child.process = info.hProcess;
  return child;
#else
  std::vector<char *> argv;
  for (const std::string &part : launch) argv.push_back(const_cast<char *>(part.c_str()));
  argv.push_back(nullptr);
  Child child;
  if (::posix_spawnp(&child.pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return std::nullopt;
  return child;
#endif
}

// macOS opens the bundle through LaunchServices with `open`, which exits at once: health is the only signal.
bool waitHealthy(Child &child, const fs::path &health, std::chrono::seconds timeout, bool detached) {
  const auto started = Clock::now();
  while (Clock::now() - started < timeout) {
    if (fs::is_regular_file(health)) return true;
    if (!detached && child.exited()) return fs::is_regular_file(health);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

fs::path sibling(const fs::path &install, const std::string &suffix) {
  const std::string name = utf8(install.filename());
  if (name.empty()) throw std::runtime_error("Install path has no name");
  return install.parent_path() / pathOf("." + name + "." + suffix);
}

std::string apply(const Plan &plan) {
  if (!waitExit(plan.waitPid, std::chrono::seconds(60))) {
    removePath(plan.staged);
    throw std::runtime_error("The app did not exit");
  }
  removePath(plan.backup);
  std::error_code ignored;
  fs::remove(plan.healthFile, ignored);
  renameWithRetry(plan.install, plan.backup);
  try {
    renameWithRetry(plan.staged, plan.install);
  } catch (...) {
    renameWithRetry(plan.backup, plan.install);
    throw;
  }
  const bool detached = !plan.launch.empty() && plan.launch.front() == "/usr/bin/open";
  bool healthy = false;
  if (auto child = start(plan.launch)) {
    healthy = waitHealthy(*child, plan.healthFile, std::chrono::seconds(plan.healthTimeoutSecs), detached);
    if (!healthy && !detached) child->kill();
  }
  if (healthy) {
    // The old copy is no longer needed; a failure here only leaves a hidden sibling for cleanup.
    try {
      removePath(plan.backup);
    } catch (...) {
    }
    return "updated";
  }
  const fs::path failed = sibling(plan.install, "update-failed");
  removePath(failed);
  renameWithRetry(plan.install, failed);
  renameWithRetry(plan.backup, plan.install);
  try {
    removePath(failed);
  } catch (...) {
  }
  start(plan.launch);
  return "rolled-back";
}

#ifdef ROOM237_QA
// QA runs only: the plan must be the fixture's own, in the fence.
bool qaAllowed(const fs::path &planPath, const Plan &plan) {
  const char *port = std::getenv("ROOM237_QA_PORT");
  if (!port) return true;
  const char *fixtureText = std::getenv("ROOM237_QA_UPDATE_FIXTURE");
  if (!fixtureText) return false;
  const std::string text(fixtureText);
  const auto fixture = Parser(text).parse();
  if (!fixture || fixture->kind != Value::Object) return false;
  auto field = [&](const char *key) -> std::string {
    const Value *v = fixture->get(key);
    return v && v->kind == Value::String ? v->text : std::string();
  };
  const fs::path profile = pathOf(field("profile")), install = pathOf(field("install"));
  if (field("profile").empty() || planPath != profile / "update-plan.json") return false;
  const std::vector<std::string> launch{field("executable"), "--root", field("root"), "--profile", field("profile"), "--legacy-profile", field("legacy")};
  return plan.install == install && plan.staged == pathOf(field("staging")) && plan.backup == sibling(install, "update-old") && plan.launch == launch && plan.healthFile == profile / "update-health" &&
         plan.resultFile == profile / "update-result.json" && plan.waitPid != 0 && plan.healthTimeoutSecs >= 1 && plan.healthTimeoutSecs <= 300;
}
#endif

int runHelper(const fs::path &planPath) {
  const auto plan = readPlan(planPath);
  if (!plan) return 2;
#ifdef ROOM237_QA
  if (!qaAllowed(planPath, *plan)) return 2;
#endif
  std::string record;
  int code = 0;
  try {
    record = "{\"status\":" + escape(apply(*plan)) + "}";
  } catch (const std::exception &error) {
    record = "{\"error\":" + escape(error.what()) + ",\"status\":\"failed\"}";
    code = 1;
  }
  std::ofstream(plan->resultFile, std::ios::binary | std::ios::trunc) << record;
  if (code != 0) start(plan->launch);
  return code;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  std::vector<fs::path> args(argv + 1, argv + argc);
  if (!args.empty() && args[0] == L"--version") {
#else
int main(int argc, char **argv) {
  std::vector<fs::path> args(argv + 1, argv + argc);
  if (!args.empty() && args[0] == "--version") {
#endif
    std::puts(ROOM237_VERSION);
    return 0;
  }
  if (args.size() == 2 && args[0] == fs::path("--apply-update")) return runHelper(args[1]);
  std::fputs("Usage: room237-core --version | --apply-update PLAN\n", stderr);
  return 2;
}
