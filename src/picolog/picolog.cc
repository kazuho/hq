#include <algorithm>
#include <cstdio>
#include <cstring>
#include "picolog/picolog.h"

using namespace std;

template <typename T> void ignore_result(T) {}

void picolog::_init(int level)
{
  ss_ = new ostringstream();
  *ss_ << now() << " [" << level_labels[level] << "] ";
}

void picolog::_flush()
{
  *ss_ << endl;
  string s = ss_->str();
  ignore_result(write(fd_, s.c_str(), (ssize_t)s.size()));
  delete ss_;
}

const string picolog::level_labels[] = {
  "DEBUG",
  "INFO",
  "WARN",
  "ERROR",
  "CRIT"
};

int picolog::fd_ = 2; // stderr by default
int picolog::log_level_ = picolog::WARN;

std::string picolog::now()
{
  char buf[sizeof("08/Feb/2011:10:58:19 +0900")];
  time_t t;
  tm l;
  time(&t);
  localtime_r(&t, &l);
  int tzmin = abs(timezone) / 60;
  sprintf(buf, "%02d/%s/%04d:%02d:%02d:%02d %c%02d%02d", l.tm_mday,
	  "Jan\0Feb\0Mar\0Apr\0May\0Jun\0Jul\0Aug\0Sep\0Oct\0Nov\0Dec"
	  + l.tm_mon * 4,
	  1900 + l.tm_year, l.tm_hour, l.tm_min, l.tm_sec,
	  timezone <= 0 ? '+' : '-', tzmin / 60, tzmin % 60);
  return buf;
}
