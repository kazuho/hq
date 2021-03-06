/**
 * very simple logger
 *
 * Usage:
 *   picolog::debug() << "this is debug level";
 *   picolog::info() << "this is info level";
 *   picolog::warn() << "this is warn level";
 *   picolog::error() << "this is error level";
 *   picolog::crit() << "this is crit level";
 * 
 * To change the destination, call picolog::set_fd(level, fd).  By default, >=warn goes to stderr and <=info are disgarded.
 */

#ifndef PICOLOG_H
#define PICOLOG_H

#include <string>
#include <sstream>

class picolog {
public:
  enum {
    DEBUG = 0,
    INFO,
    WARN,
    ERROR,
    NUM_LEVELS
  };
  
  template <typename Arg, typename Result> struct mem_fun_t {
    Result (*f_)(Arg);
    Arg arg_;
    mem_fun_t(Result (*f)(Arg), Arg arg) : f_(f), arg_(arg) {}
    friend std::ostream& operator<<(std::ostream& os, const mem_fun_t<Arg, Result>& op) {
      return os << op.f_(op.arg_);
    }
  };
  template <typename Arg, typename Result> static mem_fun_t<Arg, Result> mem_fun(Result (*f)(Arg), Arg a) {
    return mem_fun_t<Arg, Result>(f, a);
  }
  
protected:
  std::ostringstream* ss_;
public:
  explicit picolog(int level) : ss_(NULL) {
    if (log_level_ <= level)
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
  picolog(const picolog& x) : ss_(x.ss_) {
    // do not use std::swap so that NULLification can be an optimization hint
    const_cast<picolog&>(x).ss_ = NULL;
  }
  picolog& operator=(const picolog&); // not defined
  void _init(int level);
  void _flush();
public:
  static const std::string level_labels[];
protected:
  static int fd_;
  static int log_level_;
public:
  static picolog debug() { return picolog(DEBUG); }
  static picolog info() { return picolog(INFO); }
  static picolog warn() { return picolog(WARN); }
  static picolog error() { return picolog(ERROR); }
  static void set_fd(int fd) {
    fd_ = fd;
  }
  static int get_fd() {
    return fd_;
  }
  static void set_log_level(int level) {
    log_level_ = level;
  }
  static int get_log_level() {
    return log_level_;
  }
  static std::string now();
};

#endif
