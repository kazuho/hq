#ifndef PICOOPT_H
#define PICOOPT_H

/**
 * HOWTO: derive your configurator from config_base<your_configurator>
 * and the configurator will be automatically registered.
 * 
 * EXAMPLE:
 * 
 * class my_config : public picoopt::config_base<my_config> {
 * public:
 *   my_config()
 *     : picoopt::config_base<my_config>(
 *         "my-config",         // name of my option (passed to getopt_long)
 *         required_argument    // passed to getopt_long
 *     ) {}
 *   virtual int setup(const char* optval, std::string& err) {
 *     ...
 *     return 0; // success
 *   }
 * };
 *
 */

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
    std::string desc_;
    size_t index_;
  public:
    virtual ~config_core();
    virtual int setup(const std::string* arg, std::string& err) = 0;
    virtual int post_setup(std::string& err);
    const char* name() const { return name_; }
    const std::string& desc() const { return desc_; }
  protected:
    config_core(const char* name, int has_arg, const char* desc);
  };
  /**
   * base class to be inherited by configurators
   */
  template<typename T> class config_base : public config_core {
  protected:
    config_base(const char* name, int has_arg, const char* desc)
      : config_core(name, has_arg, desc)
    {
      (void)self_; // access self_ so that it would be initialized
    }
  protected:
    static T self_;
  };
public:
  /**
   * parses the arguments using getopt_long and returns 0 on success otherwise
   * a non-zero value returned by one of the configurators
   */
  static int parse_args(int argc, char** argv);
  static void print_help(FILE* fp, const char* desc);
protected:
  static std::vector<option>& longopts();
  static std::vector<config_core*>& configs();
  static void push_longopts(const char* name, int has_arg);
};

template<typename T> T picoopt::config_base<T>::self_;

#endif
