#include <cstdio>
#include "picoopt/picoopt.h"

using namespace std;

picoopt::config_core::~config_core()
{
  delete name_;
}

picoopt::config_core::config_core(const char* name, int has_arg,
				  const char* desc)
  : name_(NULL), desc_(desc), index_(longopts().size())
{
  name_ = new char[strlen(name) + 1];
  strcpy(name_, name);
  picoopt::push_longopts(name_, has_arg);
  configs().push_back(this);
}

int picoopt::config_core::post_setup(string& err)
{
  return 0;
}

int picoopt::parse_args(int argc, char** argv)
{
  if (longopts().empty() || longopts().back().name != NULL) {
    push_longopts(NULL, 0);
  }
  int index, ch, ret;
  while ((ch = getopt_long(argc, argv, "", &*longopts().begin(), &index))
	 != -1) {
    switch (ch) {
    default:
      {
	string err;
	if ((ret = configs()[index]->setup(optarg, err)) != 0) {
	fprintf(stderr, "option --%s: %s\n", longopts()[index].name,
		err.c_str());
	return ret;
	}
      }
      break;
    case '?': // unknown option
      return 1;
    case ':': // missing argument
      fprintf(stderr, "option --%s: reqires an argument\n",
	      longopts()[index].name);
      return 1;
    }
  }
  for (index = 0; index < (int)longopts().size(); ++index) {
    if (longopts()[index].name != NULL) {
      string err;
      if ((ret = configs()[index]->post_setup(err)) != 0) {
	fprintf(stderr, "option --%s: %s\n", longopts()[index].name,
		err.c_str());
	return ret;
      }
    }
  }
  return 0;
}

void picoopt::print_help(FILE* fp, const char* desc)
{
  fprintf(fp, "%s\n\n", desc);
  fprintf(fp, "Options:\n");
  const std::vector<config_core*>& c = configs();
  for (vector<config_core*>::const_iterator i = c.begin(); i != c.end(); ++i) {
    fprintf(fp, "  --%s%s\n", (*i)->name(), (*i)->desc().c_str());
  }
}

vector<option>& picoopt::longopts()
{
  static vector<option> s;
  return s;
}

vector<picoopt::config_core*>& picoopt::configs()
{
  static vector<config_core*> s;
  return s;
}

void picoopt::push_longopts(const char* name, int has_arg)
{
  longopts().push_back(option());
  longopts().back().name = name;
  longopts().back().has_arg = has_arg;
  longopts().back().flag = NULL;
  longopts().back().val = 0;
}
