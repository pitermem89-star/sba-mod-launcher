// Crash guard: survive a crash or exception inside mod code. Header-only.
#pragma once

#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <mutex>

namespace bearite {
namespace guard_detail {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE};
constexpr int kCount = 5;
constexpr int kSlots = 16;

struct Slot {
  std::atomic<long> tid{0};
  sigjmp_buf* jump = nullptr;
};

inline Slot* slots() {
  static Slot s[kSlots];
  return s;
}
inline std::mutex& mutex() {
  static std::mutex m;
  return m;
}
inline int& depth() {
  static int d = 0;
  return d;
}
inline struct sigaction* saved() {
  static struct sigaction s[kCount];
  return s;
}
inline long current_tid() { return syscall(SYS_gettid); }

inline void handler(int sig, siginfo_t* info, void* ctx) {
  long me = current_tid();
  Slot* s = slots();
  for (int i = 0; i < kSlots; ++i)
    if (s[i].tid.load() == me && s[i].jump) siglongjmp(*s[i].jump, sig);
  // Not inside a guarded call: pass the signal to whoever had it before us.
  for (int i = 0; i < kCount; ++i) {
    if (kSignals[i] != sig) continue;
    const struct sigaction& old = saved()[i];
    if ((old.sa_flags & SA_SIGINFO) && old.sa_sigaction) {
      old.sa_sigaction(sig, info, ctx);
      return;
    }
    if (!(old.sa_flags & SA_SIGINFO) && old.sa_handler && old.sa_handler != SIG_DFL &&
        old.sa_handler != SIG_IGN) {
      old.sa_handler(sig);
      return;
    }
    break;
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

// Installs our handlers while at least one Scope is alive.
class Scope {
 public:
  Scope() {
    std::lock_guard<std::mutex> lock(mutex());
    if (depth()++ == 0) {
      struct sigaction sa;
      memset(&sa, 0, sizeof(sa));
      sa.sa_sigaction = handler;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = SA_SIGINFO | SA_NODEFER;
      for (int i = 0; i < kCount; ++i) sigaction(kSignals[i], &sa, &saved()[i]);
    }
  }
  ~Scope() {
    std::lock_guard<std::mutex> lock(mutex());
    if (--depth() == 0)
      for (int i = 0; i < kCount; ++i) sigaction(kSignals[i], &saved()[i], nullptr);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

inline int invoke(void (*fn)(void*), void* arg) {
  try {
    fn(arg);
    return 0;
  } catch (...) {
    return -1;
  }
}

}  // namespace guard_detail

// Runs fn(arg). Returns 0 = ok, -1 = C++ exception, >0 = signal number that was caught.
// Destructors of objects inside fn are skipped when a signal is caught.
inline int guarded_call(void (*fn)(void*), void* arg) {
  using namespace guard_detail;
  Scope scope;
  Slot* slot = nullptr;
  long me = current_tid();
  for (int i = 0; i < kSlots && !slot; ++i) {
    long expected = 0;
    if (slots()[i].tid.compare_exchange_strong(expected, me)) slot = &slots()[i];
  }
  if (!slot) return invoke(fn, arg);  // too many parallel guards: run unprotected

  sigjmp_buf jump;
  slot->jump = &jump;
  int sig = sigsetjmp(jump, 1);
  if (sig == 0) sig = invoke(fn, arg);
  slot->jump = nullptr;
  slot->tid.store(0);
  return sig;
}

}  // namespace bearite
