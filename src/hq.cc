extern "C" {
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif
}
#include <algorithm>

#include "config.h"

extern "C" {
#include "picohttpparser/picohttpparser.h"
}
#include "picolog/picolog.h"
#include "hq.hh"

#define MAX_FDS 1024
#define READ_MAX 131072
#define MAX_HEADERS 256
#define TIMEOUT_SECS 60

using namespace std;

hq_req_reader::hq_req_reader()
  : buf_(), method_(), path_(), headers_(), content_(NULL)
{
  reset();
}

hq_req_reader::~hq_req_reader()
{
  delete content_;
}

void hq_req_reader::reset()
{
  buf_.clear();
  method_.clear();
  path_.clear();
  minor_version_ = 0;
  headers_.clear();
  delete content_;
  content_ = NULL;
  content_length_ = 0;
  state_ = READ_REQUEST;
}

bool
hq_req_reader::read_request(int fd)
{
  switch (state_) {
  case READ_REQUEST:
    return _read_request(fd);
  case READ_CONTENT:
    return _read_content(fd);
  default:
    assert(0); // unreachable
    return false;
  }
}

bool
hq_req_reader::_read_request(int fd)
{
  int r;

 RETRY:
  if ((r = read(fd, buf_.prepare(READ_MAX), READ_MAX)) == 0) {
    // closed by peer
    picolog::info() << picolog::mem_fun(hq_util::gethostof, fd)
		    << " closed by peer while reading the request";
    return false;
  } else if (r == -1) { // error
    if (errno == EINTR) {
      goto RETRY;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    } else {
      picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		       << hq_util::strerror(errno) << ", closing the socket";
      return false;
    }
  }
  
  // read some bytes
  buf_.adjust_size(r);
  {
    const char* method, * path;
    int minor_version;
    phr_header hdrs[MAX_HEADERS];
    size_t method_len, path_len, num_hdrs = MAX_HEADERS;
    r = phr_parse_request(buf_.buffer(), buf_.size(), &method, &method_len,
			  &path, &path_len, &minor_version, hdrs, &num_hdrs, 0);
    if (r == -1) { // error
      picolog::info() << picolog::mem_fun(hq_util::gethostof, fd)
		      << " received a broken HTTP request";
      return false;
    } else if (r == -2) { // partial
      return true;
    }
    // got request
    method_ = string(method, method + method_len);
    path_ = string(path, path + path_len);
    minor_version_ = minor_version;
    for (size_t i = 0; i < num_hdrs; i++) {
      if (hdrs[i].name == NULL) {
	// continuing line
	assert(i != 0);
	headers_.back().second.insert(headers_.back().second.end(),
				      hdrs[i].value,
				      hdrs[i].value + hdrs[i].value_len);
      } else {
	headers_.push_back(make_pair(string(hdrs[i].name,
					    hdrs[i].name + hdrs[i].name_len),
				     string(hdrs[i].value,
					    hdrs[i].value + hdrs[i].value_len)));
      }
    }
  }
  buf_.advance(r);
  // TODO chunked support
  hq_headers::const_iterator clen_iter
    = hq_util::find_header(headers_, "content-length");
  if (clen_iter == headers_.end()) {
    state_ = READ_COMPLETE;
    return true;
  }
  // have content-length
  if (sscanf("%llu", clen_iter->second.c_str(), &content_length_) != 1) {
    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		     << " got an invalid content-length header, closing";
    return false;
  }
  content_ = new hq_buffer();
  if (content_length_ <= buf_.size()) {
    memcpy(content_->prepare(content_length_), buf_.buffer(),
	   content_length_);
    buf_.advance(content_length_);
    state_ = READ_COMPLETE;
  } else {
    memcpy(content_->prepare(buf_.size()), buf_.buffer(), buf_.size());
    buf_.advance(buf_.size());
    state_ = READ_CONTENT;
  }
  return true;
}

bool hq_req_reader::_read_content(int fd)
{
  int maxlen = min(content_length_ - content_->size(), (size_t)INT_MAX), r;
  
 RETRY:
  if ((r = read(fd, content_->prepare(maxlen), maxlen)) == 0) {
    // closed by peer
    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		     << " closed by peer while reading the request content";
    return false;
  } else if (r == -1) { // error
    if (errno == EINTR) {
      goto RETRY;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    } else {
      picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		       << hq_util::strerror(errno) << ", closing the socket";
      return false;
    }
  }
  // read some data
  buf_.adjust_size(r);
  if (r == maxlen) {
    state_ = READ_COMPLETE;
  }
  return true;
}

list<hq_handler*> hq_handler::handlers_;

void hq_handler::dispatch_request(const hq_req_reader& req, hq_res_sender* res_sender)
{
  for (list<hq_handler*>::iterator i = handlers_.begin();
       i != handlers_.end();
       ++i) {
    hq_handler* handler = *i;
    if (handler->dispatch(req, res_sender)) {
      return;
    }
  }
  send_error(req, res_sender, 500, "no handler");
}

void hq_handler::send_error(const hq_req_reader& req, hq_res_sender* res_sender, int status, const string& msg)
{
  hq_headers hdrs;
  hdrs.push_back(hq_headers::value_type("Content-Type",
					"text/plain; charset=us-ascii"));
  if (! res_sender->open_response(status, msg, hdrs, msg.c_str(), msg.size())) {
    return;
  }
  res_sender->close_response();
}

hq_client::hq_client(int fd)
  : fd_(fd), req_()
{
  picoev_add(hq_loop::get_loop(), fd_, 0, 0, NULL, this);
  _reset();
}

hq_client::~hq_client()
{
  if (picoev_is_active(hq_loop::get_loop(), fd_)) {
    picoev_del(hq_loop::get_loop(), fd_);
  }
  close(fd_);
}

void hq_client::_reset()
{
  req_.reset();
  res_.status = 0;
  res_.sendbuf.clear();
  keep_alive_ = false;
  picoev_set_events(hq_loop::get_loop(), fd_, PICOEV_READ);
  picoev_set_callback(hq_loop::get_loop(), fd_,
		      hq_picoev_cb<hq_client, &hq_client::_read_request>, NULL);
}

void hq_client::_read_request(int fd, int revents)
{
  assert(fd == fd_);
  assert((revents & ~(PICOEV_READ | PICOEV_TIMEOUT)) == 0);
  
  // read request
  if (! req_.read_request(fd_)) {
    delete this;
    return;
  }
  if (! req_.is_complete()) {
    return;
  }
  // if the connection is a worker, then...
  if (req_.method() == "START_WORKER") {
    int newfd = dup(fd_);
    if (newfd == -1) {
      picolog::error() << "dup(2) failed, " << hq_util::strerror(errno);
      hq_handler::send_error(req_, this, 500, "dup failure");
      return;
    }
    new hq_worker(newfd, req_);
    delete this;
    return;
  }
  // is a client
  picoev_del(hq_loop::get_loop(), fd_);
  hq_handler::dispatch_request(req_, this);
}

void hq_client::send_file_response(int status, const string& msg, const hq_headers& headers, int fd)
{
  _prepare_response(status, msg, headers, fd);
  _write_sendfile_cb(fd_, PICOEV_WRITE);
}

bool hq_client::open_response(int status, const string& msg, const hq_headers& headers, const char* data, size_t len)
{
  // TODO check content-length and content-encoding to privent res. splitting
  _prepare_response(status, msg, headers, -1);
  res_.closed_by_sender = false;
  if (len != 0) {
    res_.sendbuf.push(data, len);
  }
  return _write_sendbuf(true);
}

bool hq_client::send_response(const char* data, size_t len)
{
  res_.sendbuf.push(data, len);
  return _write_sendbuf(true);
}

void hq_client::close_response()
{
  res_.closed_by_sender = true;
  if (res_.sendbuf.empty()) {
    _finalize_response(true);
  }
}

void hq_client::_prepare_response(int status, const string& msg, const hq_headers& headers, const int sendfile_fd)
{
  picoev_add(hq_loop::get_loop(), fd_, 0, 0, NULL, this);
  
  res_.status = status;
  res_.sendbuf.clear();
  if (sendfile_fd != -1) {
    res_.closed_by_sender = true;
    res_.sendfile.fd = sendfile_fd;
    res_.sendfile.pos = 0;
    struct stat st;
    int r = fstat(res_.sendfile.fd, &st);
    assert(r == 0); // fstat error is really fatal
    res_.sendfile.size = st.st_size;
    picoev_set_callback(hq_loop::get_loop(), fd_,
			hq_picoev_cb<hq_client, &hq_client::_write_sendfile_cb>,
			NULL);
  } else {
    res_.closed_by_sender = false;
    res_.sendfile.fd = -1;
    res_.sendfile.pos = 0;
    res_.sendfile.size = 0;
    picoev_set_callback(hq_loop::get_loop(), fd_,
			hq_picoev_cb<hq_client, &hq_client::_write_sendbuf_cb>,
			NULL);
  }
  
  {
    char buf[sizeof("HTTP/1.1 -1234567890 ")];
    sprintf(buf, "HTTP/1.1 %d ", status);
    res_.sendbuf.push(buf, strlen(buf));
  }
  res_.sendbuf.push(msg);
  res_.sendbuf.push("\r\n", 2);
  bool have_content_length = false;
  if (res_.sendfile.fd != -1) {
    char buf[sizeof("Content-Length: \r\n") + 24];
    sprintf(buf, "Content-Length: %llu\r\n",
	    (unsigned long long)res_.sendfile.size);
    res_.sendbuf.push(buf, strlen(buf));
    have_content_length = true;
  }
  for (hq_headers::const_iterator i = headers.begin();
       i != headers.end();
       ++i) {
    const static string connection("connection"),
      content_length("content-length");
    if (hq_util::lceq(i->first, connection)) {
      // skip
    } else {
      fprintf(stderr, "comparing '%s', '%s'\n", i->first.c_str(),
	      content_length.c_str());
      if (! have_content_length && hq_util::lceq(i->first, content_length)) {
	have_content_length = true;
      }
      res_.sendbuf.push(i->first);
      res_.sendbuf.push(": ", 2);
      res_.sendbuf.push(i->second);
      res_.sendbuf.push("\r\n", 2);
    }
  }
  // TODO add support for content-encoding: chunked
  bool can_keep_alive = have_content_length;
  if (have_content_length) {
    for (hq_headers::const_iterator i = req_.headers().begin();
	 i != req_.headers().end();
	 ++i) {
      const static string connection("connection");
      if (hq_util::lceq(i->first, connection)) {
	const static string keep_alive("keep-alive");
	if (! hq_util::lceq(i->second, keep_alive)) {
	  can_keep_alive = false;
	}
	break;
      }
    }
  }
  keep_alive_ = can_keep_alive;
  res_.sendbuf.push(keep_alive_
		    ? "Connection: keep-alive\r\n"
		    : "Connection: close\r\n");
  res_.sendbuf.push("\r\n", 2);
}

void hq_client::_finalize_response(bool success)
{
  if (hq_log_access::log_ != NULL) {
    hq_log_access::log_->log(fd_, req_.method(), req_.path(),
			     req_.minor_version(), res_.status);
  }
  if (success && keep_alive_) {
    _reset();
  } else {
    delete this;
  }
}

void hq_client::_write_sendfile_cb(int fd, int revents)
{
  assert(fd_ == fd);
  assert((revents & ~(PICOEV_WRITE | PICOEV_TIMEOUT)) == 0);
  
  // flush header
  if (! res_.sendbuf.empty()) {
    if (! _write_sendbuf(false)) {
      return;
    }
    if (! res_.sendbuf.empty()) {
      return;
    }
  }
  
  // no more headers in buffer, send file
 RETRY:
  int r;
#if HQ_IS_LINUX
  r = sendfile(fd_, res_.sendfile.fd, &res_.sendfile.pos, 1048576);
#elif HQ_IS_BSD
  {
    off_t len = min((off_t)1048576, res_.sendfile.size);
    r = sendfile(res_.sendfile.fd, fd_, res_.sendfile.pos, &len, NULL, 0);
    if (r == 0 || errno == EAGAIN) {
      res_.sendfile.pos += len;
    }
  }
#else
  #error "do not know the sendfile API for this OS"
#endif
  if (r == 0) {
    if (res_.sendfile.pos == res_.sendfile.size) {
      _finalize_response(true);
      return;
    }
  } else if (errno == EINTR) {
    goto RETRY;
  } else if (! (errno == EAGAIN || errno == EWOULDBLOCK)) {
    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		     << " sendfile(2) failed while sending response, "
		     << hq_util::strerror(errno);
    _finalize_response(false);
    return;
  }
  picoev_set_events(hq_loop::get_loop(), fd_, PICOEV_WRITE);
}

void hq_client::_write_sendbuf_cb(int fd, int revents)
{
  assert(fd_ == fd);
  assert((revents & ~(PICOEV_WRITE | PICOEV_TIMEOUT)) == 0);
  
  if (! _write_sendbuf(true)) {
    return;
  }
  if (res_.sendbuf.empty() && res_.closed_by_sender) {
    _finalize_response(true);
  }
}

bool hq_client::_write_sendbuf(bool disactivate_poll_when_empty)
{
  int r;
  
 RETRY:
  if ((r = write(fd_, res_.sendbuf.buffer(), res_.sendbuf.size())) != -1) {
    res_.sendbuf.advance(r);
  } else if (errno == EINTR) {
    goto RETRY;
  } else if (! (errno == EINTR || errno == EWOULDBLOCK)) {
    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd_)
		     << " write(2) failed while sending response, "
		     << hq_util::strerror(errno);
    _finalize_response(false);
    return false;
  }
  picoev_set_events(hq_loop::get_loop(), fd_,
		    res_.sendbuf.empty() ? 0 : PICOEV_WRITE);
  return true;
}

hq_worker::handler::handler()
  : junction_(NULL)
{
}

hq_worker::handler::~handler()
{
  // TODO gracefully shutdown
}

bool hq_worker::handler::dispatch(const hq_req_reader& req, hq_res_sender* res_sender)
{
  // obtain worker or register myself
  hq_worker* worker = NULL;
  {
    cac_mutex_t<junction>::lockref junction(junction_);
    if (junction->workers.empty()) {
      junction->reqs.push_back(req_queue_entry(&req, res_sender));
    } else {
      worker = junction->workers.front();
      junction->workers.pop_front();
    }
  }
  
  if (worker != NULL) {
    worker->_start(&req, res_sender);
  }
  
  return true;
}

void hq_worker::handler::_start_worker_or_register(hq_worker* worker)
{
  cac_mutex_t<junction>::lockref junction(junction_);
  
  if (junction->reqs.empty()) {
    junction->workers.push_back(worker);
    return;
  }
  req_queue_entry r(junction->reqs.front());
  junction->reqs.pop_front();
  worker->_start(r.req, r.res_sender);
}

hq_worker::hq_worker(int fd, const hq_req_reader&)
  : fd_(fd), req_(NULL), res_sender_(NULL), buf_()
{
  picoev_add(hq_loop::get_loop(), fd_, 0, 0, NULL, this);
  buf_.push("HTTP/1.1 101 Upgrade\r\n"
	    "Upgrade: HTTPWORKER/0.9\r\n\r\n");
  _send_upgrade(fd, PICOEV_WRITE);
}

hq_worker::~hq_worker()
{
  assert(req_ == NULL);
  assert(res_sender_ == NULL);
  picoev_del(hq_loop::get_loop(), fd_);
  close(fd_);
}

void hq_worker::_send_upgrade(int fd, int revents)
{
  assert(fd == fd_);
  assert((revents & ~(PICOEV_WRITE | PICOEV_TIMEOUT)) == 0);
  
  if (! _send_buffer()) {
    picolog::info() << picolog::mem_fun(hq_util::gethostof, fd)
		    << " worker closed the connection unexpectedly, "
		    << hq_util::strerror(errno);
    delete this;
  }
  if (! buf_.empty()) {
    picoev_set_events(hq_loop::get_loop(), fd_, PICOEV_WRITE);
    picoev_set_callback(hq_loop::get_loop(), fd_,
			hq_picoev_cb<hq_worker, &hq_worker::_send_upgrade>,
			NULL);
  } else {
    _prepare_next();
  }
}

void hq_worker::_prepare_next()
{
  picoev_set_events(hq_loop::get_loop(), fd_, 0);
  // fetch the request immediately or register myself to dispatcher
  handler_._start_worker_or_register(this);
}

void hq_worker::_start(const hq_req_reader* req, hq_res_sender* res_sender)
{
  req_ = req;
  res_sender_ = res_sender;
  
  // build request
  buf_.clear();
  buf_.push(req_->method());
  buf_.push(' ');
  buf_.push(req_->path());
  buf_.push(" HTTP/1.0\r\n", sizeof(" HTTP/1.0\r\n") - 1);
  for (hq_headers::const_iterator i = req_->headers().begin();
       i != req_->headers().end();
       ++i) {
    // TODO sanitize the headers?
    buf_.push(i->first);
    buf_.push(": ", 2);
    buf_.push(i->second);
    buf_.push("\r\n", 2);
  }
  buf_.push("\r\n", 2);
  
  _send_request();
}

void hq_worker::_send_request_cb(int fd, int revents)
{
  assert(fd == fd_);
  assert((revents & ~(PICOEV_WRITE | PICOEV_TIMEOUT)) == 0);
  
  _send_request();
}

void hq_worker::_send_request()
{
  if (! _send_buffer()) {
    _return_error(500, "worker connection reset");
    goto CLOSE;
  }
  if (! buf_.empty()) {
    picoev_set_events(hq_loop::get_loop(), fd_, PICOEV_WRITE);
    picoev_set_callback(hq_loop::get_loop(), fd_,
			hq_picoev_cb<hq_worker, &hq_worker::_send_request_cb>,
			NULL);
    return;
  }
  // sent all data, wait for response
  picoev_set_events(hq_loop::get_loop(), fd_, PICOEV_READ);
  picoev_set_callback(hq_loop::get_loop(), fd_,
		      hq_picoev_cb<hq_worker,
		                   &hq_worker::_read_response_header>,
		      NULL);
  buf_.clear();
  return;
  
 CLOSE:
  delete this;
}

void hq_worker::_read_response_header(int fd, int revents)
{
  assert(fd == fd_);
  assert((revents & ~(PICOEV_READ | PICOEV_TIMEOUT)) == 0);
  
  int status, minor_version, r;
  string msg;
  hq_headers hdrs;
  
 RETRY:
  if ((r = read(fd_, buf_.prepare(READ_MAX), READ_MAX)) == 0) {
    // closed
    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		     << " worker closed the connection";
    _return_error(500, "connection closed by worker");
    goto CLOSE;
  } else if (r == -1) {
    if (r == EINTR) {
      goto RETRY;
    } else if (r == EAGAIN || r == EWOULDBLOCK) {
      return;
    } else {
      picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		       << " failed to read response from worker, "
		       << hq_util::strerror(errno);
      _return_error(500, "worker connection error");
      goto CLOSE;
    }
  }
  // read some bytes
  buf_.adjust_size(r);

  { // try to parse the response
    const char* msg_p;
    phr_header headers[MAX_HEADERS];
    size_t msg_len, num_headers = MAX_HEADERS;
    r = phr_parse_response(buf_.buffer(), buf_.size(), &minor_version, &status,
			   &msg_p, &msg_len, headers, &num_headers, 0);
    if (r == -1) { // error
      picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		       << " received a broken HTTP request";
      _return_error(500, "worker response error");
      goto CLOSE;
    } else if (r == -2) { // partial
      return;
    }
    // got response
    msg = string(msg_p, msg_len);
    for (size_t i = 0; i < num_headers; ++i) {
      // TODO prune headers
      hdrs.push_back(make_pair(string(headers[i].name,
				      headers[i].name + headers[i].name_len),
			       string(headers[i].value,
				      headers[i].value + headers[i].value_len)));
    }
    buf_.advance(r);
  }
  // TODO check content boundary, etc.
  if (! res_sender_->open_response(status, msg, hdrs, buf_.buffer(),
				   buf_.size())) {
    res_sender_ = NULL;
  }
  buf_.clear();
  picoev_set_callback(hq_loop::get_loop(), fd_,
		      hq_picoev_cb<hq_worker, &hq_worker::_read_response_body>,
		      NULL);
  return;
  
 CLOSE:
  delete this;
}

void hq_worker::_read_response_body(int fd, int revents)
{
  assert(fd == fd_);
  assert((revents & ~(PICOEV_READ | PICOEV_TIMEOUT)) == 0);
  
  char* buf = buf_.prepare(READ_MAX);
  int r;
  
 RETRY:
  if ((r = read(fd_, buf, READ_MAX)) == 0) {
    // closed
    goto CLOSE;
  } else if (r == -1) {
    if (r == EINTR) {
      goto RETRY;
    } else if (r == EAGAIN || r == EWOULDBLOCK) {
      return;
    } else {
      picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		       << " failed to read response content from worker, "
		       << hq_util::strerror(errno);
      goto CLOSE;
    }
  }
  // got data
  if (res_sender_ != NULL) {
    if (! res_sender_->send_response(buf, r)) {
      res_sender_ = NULL;
    }
  }
  
  return;
  
 CLOSE:
  if (res_sender_ != NULL) {
    res_sender_->close_response();
    req_ = NULL;
    res_sender_ = NULL;
  }
  delete this;
}

void hq_worker::_return_error(int status, const string& msg)
{
  assert(res_sender_ != NULL);
  
  hq_handler::send_error(*req_, res_sender_, status, msg);
  req_ = NULL;
  res_sender_ = NULL;
}

bool hq_worker::_send_buffer()
{
  int r;
  
 RETRY:
  if ((r = write(fd_, buf_.buffer(), buf_.size())) == -1) {
    if (r == EINTR) {
      goto RETRY;
    } else if (r == EAGAIN || r == EWOULDBLOCK) {
      return true;
    } else {
      return false;
    }
  }
  buf_.advance(r);
  
  return true;
}

hq_worker::handler hq_worker::handler_;

void hq_worker::setup()
{
  hq_handler::handlers_.push_back(&handler_);
}

static void setup_sock(int fd)
{
  int on = 1, r;
  r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  assert(r == 0);
  r = fcntl(fd, F_SETFL, O_NONBLOCK);
  assert(r == 0);
}

hq_listener::config::config()
  : picoopt::config_base<config>("port", required_argument,
				 "=[host:]port"),
    called_cnt_(0)
{
}

int hq_listener::config::setup(const string* hostport, string& err)
{
  unsigned short port;
  if (sscanf(hostport->c_str(), "%hu", &port) != 1) {
    err = "port should be a number";
    return 1;
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd != -1);
  int r, flag = 1;
  r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
  assert(r == 0);
  struct sockaddr_in sa;
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = htonl(0);
  if (bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
    err = "failed to bind to port";
    return 1;
  }
  setup_sock(fd);
  r = listen(fd, SOMAXCONN);
  assert(r == 0);
  new hq_listener(fd);
  
  called_cnt_++;
  return 0;
}

int hq_listener::config::post_setup(string& err)
{
  if (called_cnt_ < 1) {
    err = "should be set more than once";
    return 1;
  }
  return 0;
}

hq_listener::poll_guard::poll_guard()
  : locked_(false)
{
  if (pthread_mutex_trylock(&hq_listener::listeners_mutex_) != 0)
    return;
  // if the lock succeeded add the file descriptors to the poll list
  locked_ = true;
  for (list<hq_listener*>::iterator i = listeners_.begin();
       i != listeners_.end();
       ++i) {
    picoev_add(hq_loop::get_loop(), (*i)->listen_fd_, PICOEV_READ, 0,
	       hq_picoev_cb<hq_listener, &hq_listener::_accept>, *i);
  }
}

hq_listener::poll_guard::~poll_guard()
{
  if (! locked_)
    return;
  // remove the descriptors from the poll list and unlock
  for (list<hq_listener*>::iterator i = listeners_.begin();
       i != listeners_.end();
       ++i) {
    picoev_del(hq_loop::get_loop(), (*i)->listen_fd_);
  }
  pthread_mutex_unlock(&hq_listener::listeners_mutex_);
}

hq_listener::hq_listener(int listen_fd)
  : listen_fd_(listen_fd)
{
  mutex_guard mg(&listeners_mutex_);
  listeners_.push_back(this);
}

void hq_listener::_accept(int fd, int revents)
{
  assert(fd == listen_fd_);
  assert((revents & ~(PICOEV_READ | PICOEV_TIMEOUT)) == 0);
  
  int newfd = accept(listen_fd_, NULL, NULL);
  if (newfd == -1) {
    return;
  }
  setup_sock(newfd);
  new hq_client(newfd);
}

std::list<hq_listener*> hq_listener::listeners_;
pthread_mutex_t hq_listener::listeners_mutex_ = PTHREAD_MUTEX_INITIALIZER;

hq_loop::hq_loop()
{
  *loop_ = picoev_create_loop(TIMEOUT_SECS);
}

hq_loop::~hq_loop()
{
  picoev_destroy_loop(*loop_);
}

void hq_loop::run_loop()
{
  while (1) {
    hq_listener::poll_guard pg;
    picoev_loop_once(*loop_, TIMEOUT_SECS);
  }
}

hq_tls<picoev_loop*> hq_loop::loop_;

hq_static_handler::config::config()
  : picoopt::config_base<config>("static", required_argument,
				 "=virtual_path=static_path")
{
}

int hq_static_handler::config::setup(const string* mapping, string& err)
{
  string::size_type eq_at = mapping->find('=');
  if (eq_at == string::npos) {
    err = "not like: virtual_path=real_path";
    return 1;
  }
  
  string vpath(mapping->substr(0, eq_at)), dir(mapping->substr(eq_at + 1));
  if (! vpath.empty() && *vpath.rbegin() != '/') {
    vpath.push_back('/');
  }
  if (! dir.empty() && *dir.rbegin() != '/') {
    dir.push_back('/');
  }
  
  hq_handler::handlers_.push_back(new hq_static_handler(vpath, dir));
  return 0;
}

bool hq_static_handler::dispatch(const hq_req_reader& req, hq_res_sender* res_sender)
{
  if (req.path().compare(0, vpath_.size(), vpath_) != 0) {
    return false;
  }
  if (*req.path().rbegin() == '/') {
    send_error(req, res_sender, 403, "directory listing not supported");
    return true;
  }

  string realpath(dir_ + req.path().substr(vpath_.size()));
  int fd;
  if ((fd = open(realpath.c_str(), O_RDONLY)) == -1) {
    switch (errno) {
    case ENOENT:
      picolog::error() << "file not found: " << realpath;
      send_error(req, res_sender, 404, "not found");
      break;
    default:
      picolog::error() << "access denied to file: " << realpath << ", "
		       << hq_util::strerror(errno);
      send_error(req, res_sender, 403, "access denied");
      break;
    }
    return true;
  }
  
  string mime_type(hq_util::get_mime_type(hq_util::get_ext(realpath)));
  hq_headers hdrs;
  hdrs.push_back(hq_headers::value_type("Content-Type", mime_type));
  res_sender->send_file_response(200, "OK", hdrs, fd);
  
  return true;
}

int hq_log_access::config::setup(const string* filename, string& err)
{
  if (log_ != NULL) {
    delete log_;
    log_ = NULL;
  }
  FILE* fp;
  if ((fp = fopen(filename->c_str(), "a")) == NULL) {
    err = string("could not open file:") + *filename + ", "
      + hq_util::strerror(errno);
    return 1;
  }
  log_ = new hq_log_access(fp);
  return 0;
}

hq_log_access::~hq_log_access()
{
  fclose(fp_);
}

void hq_log_access::log(int fd, const string& method, const string& path,
			int minor_version, int status)
{
  fprintf(fp_, "%s - - [%s] \"%s %s HTTP/1.%d\" %d -\n",
	  hq_util::gethostof(fd).c_str(), picolog::now().c_str(),
	  method.c_str(), path.c_str(), minor_version, status);
  fflush(fp_);
}

hq_log_access* hq_log_access::log_ = NULL;

hq_headers::const_iterator hq_util::find_header(const hq_headers& hdrs, const string& name)
{
  hq_headers::const_iterator i;
  for (i= hdrs.begin(); i != hdrs.end(); ++i) {
    if (i->first.size() != name.size()) {
      goto NEXT;
    }
    for (size_t j = 0; j < name.size(); ++j) {
      if (tolower(i->first[j]) != tolower(name[j])) {
	goto NEXT;
      }
    }
    // equals
    break;
  NEXT:
    ;
  }
  return i;
}

string hq_util::get_mime_type(const string& ext)
{
#define MAP(e, m) if (ext == e) return m
  MAP("htm", "text/html");
  MAP("html", "text/html");
  MAP("gif", "image/gif");
  MAP("jpg", "image/jpeg");
  MAP("jpeg", "image/jpeg");
  MAP("png", "image/png");
#undef MAP
  return "text/plain";
}

string hq_util::get_ext(const string& path)
{
  string::const_iterator i = path.end();
  if (i != path.begin()) {
    do {
      --i;
      switch (*i) {
      case '.':
	return string(i + 1, path.end());
      case '/':
	break;
      }
    } while (i != path.begin());
  }
  return string();
}

string hq_util::gethostof(int fd)
{
  sockaddr_in sin;
  socklen_t slen = sizeof(sin);
  if (getpeername(fd, (sockaddr*)&sin, &slen) == 0
      && sin.sin_family == AF_INET) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    mutex_guard g(&m);
    return inet_ntoa(sin.sin_addr);
  } else {
    char buf[32];
    sprintf(buf, "fd=%d", fd);
    return buf;
  }
}

string hq_util::strerror(int err)
{
  char buf[128];
  strerror_r(err, buf, sizeof(buf));
  return buf;
}

bool hq_util::lceq(const char* x, const char* y)
{
#define LC(c) ('A' <= c && c <= 'Z' ? c + 0x20 : c)
  for (; *x != '\0'; x++, y++)
    if (LC(*x) != LC(*y))
      return false;
  return *y == '\0';
}

bool hq_util::lceq(const string& x, const string& y)
{
  return x.size() == y.size() && lceq(x.c_str(), y.c_str());
}

struct hq_help : public picoopt::config_base<hq_help> {
  hq_help()
    : picoopt::config_base<hq_help>("help", no_argument, "  print this help")
  {}
  virtual int setup(const string*, string&) {
    picoopt::print_help(stdout, "HQ - a queue-based HTTP server");
    exit(0);
  }
};

struct hq_log_level : public picoopt::config_base<hq_log_level> {
  hq_log_level()
    : picoopt::config_base<hq_log_level>("log-level", required_argument,
					 "=debug|info|warn|error|crit|none")
  {}
  virtual int setup(const string* level, string& err) {
    for (int i = 0; i < picolog::NUM_LEVELS; i++) {
      if (hq_util::lceq(picolog::level_labels[i], *level)) {
	picolog::set_log_level(i);
	return 0;
      }
    }
    const static string none("none");
    if (hq_util::lceq(*level, none)) {
      picolog::set_log_level(picolog::NUM_LEVELS);
      return 0;
    }
    err = string("unknown log level:") + *level;
    return 1;
  }
};

int main(int argc, char** argv)
{
  picoev_init(MAX_FDS);
  
  int r;
  if ((r = picoopt::parse_args(argc, argv)) != 0) {
    exit(r);
  }
  argc -= optind;
  argv += optind;
  
  hq_worker::setup();
  
  hq_loop loop;
  loop.run_loop();
  
  return 0;
}
