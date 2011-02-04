#ifndef PICOOPT_H
#define PICOOPT_H

extern "C" {
#include <getopt.h>
}
#include <cstring>
#include <string>
#include <vector>

/**
 * class for handling confugiration
 */
class picoopt {
public:
  class config_core {
  protected:
    char* name_;
    size_t index_;
  public:
    virtual ~config_core();
    virtual int setup(const char* arg, std::string& err) = 0;
    virtual int post_setup(std::string& err);
  protected:
    config_core(const char* name, int has_arg);
  };
  /**
   * base class to be inherited by configurators
   */
  template<typename T> class config_base : public config_core {
  protected:
    config_base(const char* name, int has_arg) : config_core(name, has_arg)
    {
      (void)self_; // access self_ so that it would be initialized
    }
  protected:
    static T self_;
  };
protected:
  static std::vector<option>* longopts_;
  static std::vector<config_core*>* configs_;
public:
  /**
   * parses the arguments using getopt_long and returns 0 on success otherwise
   * a non-zero value returned by one of the configurators
   */
  static int parse_args(int argc, char** argv);
protected:
  static std::vector<option>& longopts();
  static std::vector<config_core*>& configs();
  static void push_longopts(const char* name, int has_arg);
};

template<typename T> T picoopt::config_base<T>::self_;

#endif
