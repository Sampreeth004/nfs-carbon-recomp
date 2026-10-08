// Android thread affinity watchdog, ported from victorgbd/NFSMW-Recompiled-Mobile
// (tools/afinidad.cpp). Pins the command processor and guest bootstrap thread
// to prime cores. Carbon's MainThread may use all cores, including prime cores;
// forcing it onto the remaining cores can delay frame production. Other threads
// use the remaining cores. NFSMW's original policy measured +17% fps in
// draw-heavy scenes on a Snapdragon 8 Elite; that is not a Carbon measurement.
//
// The SDK has per-thread masks, but nothing applies them on POSIX, and Android
// rewrites the cpuset when the app goes to the background, so a small thread
// rescans /proc/self/task every 2 s and only touches threads whose mask differs.

#include "thread_affinity.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(thread_affinity, "auto", "System",
                      "Pin threads to cores: 'auto' or rules 'Name=6-7;Other=0+1;*=0-5'. "
                      "Empty disables it");

namespace nfscarbon::afinidad {

namespace {

constexpr int kMaxCores = 64;
constexpr auto kInterval = std::chrono::seconds(2);

struct Rule {
  std::string prefix;
  cpu_set_t cores;
  std::string text;
};

std::string Trim(std::string s) {
  const auto a = s.find_first_not_of(" \t");
  const auto b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool ReadFile(const char* path, char* buf, size_t size) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  const ssize_t n = read(fd, buf, size - 1);
  close(fd);
  if (n <= 0) {
    return false;
  }
  buf[n] = '\0';
  for (ssize_t i = n - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r'); --i) {
    buf[i] = '\0';
  }
  return true;
}

bool ParseCores(const std::string& list, cpu_set_t& out) {
  CPU_ZERO(&out);
  size_t pos = 0;
  while (pos <= list.size()) {
    size_t end = list.find('+', pos);
    if (end == std::string::npos) {
      end = list.size();
    }
    const std::string piece = Trim(list.substr(pos, end - pos));
    if (!piece.empty()) {
      const size_t dash = piece.find('-');
      const int from = std::atoi(piece.substr(0, dash).c_str());
      const int to =
          dash == std::string::npos ? from : std::atoi(piece.substr(dash + 1).c_str());
      for (int c = from; c <= to && c < kMaxCores; ++c) {
        if (c >= 0) {
          CPU_SET(c, &out);
        }
      }
    }
    pos = end + 1;
  }
  return CPU_COUNT(&out) > 0;
}

std::string CoresText(const cpu_set_t& cores) {
  std::string s;
  for (int c = 0; c < kMaxCores; ++c) {
    if (CPU_ISSET(c, &cores)) {
      if (!s.empty()) {
        s += '+';
      }
      s += std::to_string(c);
    }
  }
  return s;
}

std::vector<Rule> ParseRules(const std::string& text) {
  std::vector<Rule> rules;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find(';', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string piece = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = piece.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    Rule rule;
    rule.prefix = Trim(piece.substr(0, eq));
    if (rule.prefix == "*") {
      rule.prefix.clear();
    }
    if (!ParseCores(piece.substr(eq + 1), rule.cores)) {
      REXLOG_WARN("[affinity] rule without cores, ignored: '{}'", piece);
      continue;
    }
    rule.text = (rule.prefix.empty() ? "*" : rule.prefix) + "=" + CoresText(rule.cores);
    rules.push_back(std::move(rule));
  }
  return rules;
}

std::vector<Rule> AutoRules() {
  int capacity[kMaxCores];
  int highest = 0;
  int lowest = 0;
  int count = 0;
  for (int c = 0; c < kMaxCores; ++c) {
    char path[96];
    char buf[32];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
    if (!ReadFile(path, buf, sizeof(buf))) {
      break;
    }
    capacity[c] = std::atoi(buf);
    highest = count == 0 ? capacity[c] : std::max(highest, capacity[c]);
    lowest = count == 0 ? capacity[c] : std::min(lowest, capacity[c]);
    ++count;
  }
  if (count == 0 || highest == lowest) {
    REXLOG_INFO("[affinity] auto: all cores equal, leaving affinity alone");
    return {};
  }

  cpu_set_t prime;
  cpu_set_t rest;
  CPU_ZERO(&prime);
  CPU_ZERO(&rest);
  for (int c = 0; c < count; ++c) {
    if (capacity[c] == highest) {
      CPU_SET(c, &prime);
    } else {
      CPU_SET(c, &rest);
    }
  }

  std::vector<Rule> rules;
  for (const char* name : {"GPU Commands", "Main XThread"}) {
    Rule rule;
    rule.prefix = name;
    rule.cores = prime;
    rule.text = std::string(name) + "=" + CoresText(prime);
    rules.push_back(std::move(rule));
  }
  // Carbon's frame-producing guest worker is named MainThread, separately from
  // Main XThread. Let Android schedule it on prime cores when useful without
  // forcing three busy threads to share the small prime-only mask.
  Rule main_worker;
  main_worker.prefix = "MainThread";
  CPU_ZERO(&main_worker.cores);
  for (int c = 0; c < count; ++c) CPU_SET(c, &main_worker.cores);
  main_worker.text = "MainThread=" + CoresText(main_worker.cores);
  rules.push_back(std::move(main_worker));
  Rule wildcard;
  wildcard.cores = rest;
  wildcard.text = "*=" + CoresText(rest);
  rules.push_back(std::move(wildcard));
  return rules;
}

}  // namespace

class Vigilante {
 public:
  explicit Vigilante(std::vector<Rule> rules) : rules_(std::move(rules)) {
    thread_ = std::thread([this]() { Loop(); });
  }

  ~Vigilante() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void Loop() {
    pthread_setname_np(pthread_self(), "NFSC affinity");
    while (true) {
      Pass();
      std::unique_lock<std::mutex> lock(mutex_);
      if (cv_.wait_for(lock, kInterval, [this]() { return stop_; })) {
        return;
      }
    }
  }

  const Rule* RuleFor(const char* name) const {
    for (const Rule& rule : rules_) {
      if (rule.prefix.empty() ||
          std::strncmp(name, rule.prefix.c_str(), rule.prefix.size()) == 0) {
        return &rule;
      }
    }
    return nullptr;
  }

  void Pass() {
    DIR* dir = opendir("/proc/self/task");
    if (!dir) {
      return;
    }
    while (const dirent* e = readdir(dir)) {
      if (e->d_name[0] < '0' || e->d_name[0] > '9') {
        continue;
      }
      const pid_t tid = static_cast<pid_t>(std::atoi(e->d_name));
      char path[64];
      char name[32];
      std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
      if (!ReadFile(path, name, sizeof(name))) {
        continue;
      }
      const Rule* rule = RuleFor(name);
      if (!rule) {
        continue;
      }

      cpu_set_t current;
      if (sched_getaffinity(tid, sizeof(current), &current) == 0 &&
          CPU_EQUAL(&current, &rule->cores)) {
        continue;
      }
      if (sched_setaffinity(tid, sizeof(rule->cores), &rule->cores) != 0) {
        if (errno != EINVAL && failed_.insert(tid).second) {
          REXLOG_WARN("[affinity] could not pin '{}' (tid {}): {}", name, tid,
                      std::strerror(errno));
        }
        continue;
      }
      if (announced_.insert(std::to_string(tid) + '|' + name).second) {
        REXLOG_INFO("[affinity] {} (tid {}) -> {}", name, tid, CoresText(rule->cores));
      }
    }
    closedir(dir);
  }

  const std::vector<Rule> rules_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::set<std::string> announced_;
  std::set<pid_t> failed_;
};

void BorrarVigilante::operator()(Vigilante* v) const {
  delete v;
}

VigilantePtr Arrancar() {
  const std::string text = Trim(REXCVAR_GET(thread_affinity));
  if (text.empty()) {
    return nullptr;
  }
  std::vector<Rule> rules = text == "auto" ? AutoRules() : ParseRules(text);
  if (rules.empty()) {
    return nullptr;
  }
  std::string summary;
  for (const Rule& rule : rules) {
    if (!summary.empty()) {
      summary += "; ";
    }
    summary += rule.text;
  }
  REXLOG_INFO("[affinity] '{}' -> {}", text, summary);
  return VigilantePtr(new Vigilante(std::move(rules)));
}

}  // namespace nfscarbon::afinidad
