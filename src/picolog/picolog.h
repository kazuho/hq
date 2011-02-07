#ifndef PICOLOG_H
#define PICOLOG_H

#include <sstream>

class picolog {
public:
  enum {
    DEBUG = 0,
    INFO,
    WARN,
    ERROR,
    NUM_LEVELS,
  };
  
protected:
  int fd_;
  std::ostringstream* ss_;
public:
  explicit picolog(int level) : fd_(fds_[level]), ss_(NULL) {
    if (fd_ != -1)
      _init(level);
  }
  ~picolog() {
    if (ss_ != NULL)
      _flush();
  }
  bool is_open() const { return ss_ != NULL; }
  std::ostream& stream() { return *ss_; }
  template <typename T> picolog& operator<<(const T& v) {
    if (is_open()) stream() << v;
    return *this;
  }
private:
  /**
   * copy constructor delegates the ownership, only used by myself
   */
  picolog(const picolog& x) : fd_(x.fd_), ss_(x.ss_) {
    // do not use std::swap so that NULLification can be an optimization hint
    const_cast<picolog&>(x).ss_ = NULL;
  }
  picolog& operator=(const picolog&); // not defined
  void _init(int level);
  void _flush();
protected:
  static int fds_[NUM_LEVELS]; // closed if fds_[x] == -1
public:
  static picolog debug() { return picolog(DEBUG); }
  static picolog info() { return picolog(INFO); }
  static picolog warn() { return picolog(WARN); }
  static picolog error() { return picolog(ERROR); }
  static void set_fd(int level, int fd) {
    fds_[level] = fd;
  }
  static int get_fd(int level) {
    return fds_[level];
  }
};

#endif