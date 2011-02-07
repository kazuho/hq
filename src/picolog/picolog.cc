#include <cstdio>
#include <cstring>
#include "picolog/picolog.h"

using namespace std;

static const char* level_labels[] = {
  " [DEBUG] ",
  " [INFO] ",
  " [WARN] ",
  " [ERROR] "
};

void picolog::_init(int level)
{
  time_t t;
  char buf[26 + sizeof(" [DEBUG] ")];
  
  time(&t);
  ctime_r(&t, buf);
  strcpy(buf + strlen(buf) - 1, level_labels[level]); // remove "\n", set label
  ss_ = new ostringstream();
  *ss_ << buf;
}

void picolog::_flush()
{
  *ss_ << endl;
  string s = ss_->str();
  write(fd_, s.c_str(), s.size());
  delete ss_;
}

int picolog::fds_[picolog::NUM_LEVELS] = {
  -1, // DEBUG - closed
  -1, // INFO  - closed
  2,  // WARN  - stderr
  2  // ERROR - stderr
};
