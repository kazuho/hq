extern "C" {
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
}
#include <algorithm>

#include "config.h"

extern "C" {
#include "picohttpparser/picohttpparser.h"
}
#include "hq.hh"

#define MAX_FDS 1024
#define READ_MAX 131072
#define MAX_HEADERS 256
#define TIMEOUT_SECS 60

using namespace std;

hq_req_reader::hq_req_reader()
  : buf_(), method_(), path_(), headers_(), content_(NULL),
    state_(READ_REQUEST)
{
}

hq_req_reader::~hq_req_reader()
{
  delete content_;
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
    // TODO LOG
    return false;
  } else if (r == -1) { // error
    if (errno == EINTR) {
      goto RETRY;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    } else {
      // TODO log error
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
      // TODO LOG
      return false;
    } else if (r == -2) { // partial
      return true;
    }
    // got request
    method_ = string(method, method + method_len);
    path_ = string(path, path + path_len);
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
    // TODO LOG
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
    // TODO LOG
    return false;
  } else if (r == -1) { // error
    if (errno == EINTR) {
      goto RETRY;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    } else {
      // TODO LOG
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

void hq_handler::dispatch_request(const hq_req_reader& req, hq_res_sender* res_sender)
{
  // TODO should be configurable and support other handlers (like static content) as well
  if (hq_worker::handler_.dispatch(req, res_sender)) {
    return;
  }
  send_error(req, res_sender, 500, "no handler");
}

void hq_handler::send_error(const hq_req_reader& req, hq_res_sender* res_sender, int status, const string& msg)
{
  hq_headers hdrs;
  hdrs.push_back(hq_headers::value_type("Content-Type",
					"text/plain; charset=us-ascii"));
  if (! res_sender->open_response(500, msg, hdrs, msg.c_str(), msg.size())) {
    return;
  }
  res_sender->close_response();
}

hq_client::hq_client(int fd)
  : fd_(fd), req_()
{
  _start();
}

hq_client::~hq_client()
{
  if (picoev_is_active(hq_loop::get_loop(), fd_)) {
    picoev_del(hq_loop::get_loop(), fd_);
  }
  close(fd_);
}

void hq_client::_start()
{
  picoev_add(hq_loop::get_loop(), fd_, PICOEV_READ, 0,
	     hq_picoev_cb<hq_client, &hq_client::_read_request>, this);
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
  // request is complete, remove from event listener
  picoev_del(hq_loop::get_loop(), fd_);
  // if the connection is a worker, then...
  if (req_.method() == "START_WORKER") {
    int newfd = dup(fd_);
    if (newfd == -1) {
      // TODO LOG
      hq_handler::send_error(req_, this, 500, "dup failure");
      return;
    }
    new hq_worker(newfd, req_);
    delete this;
    return;
  }
  // is a client
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
    _finalize_response();
  }
}

void hq_client::_prepare_response(int status, const string& msg, const hq_headers& headers, const int sendfile_fd)
{
  picoev_add(hq_loop::get_loop(), fd_, 0, 0,
	     hq_picoev_cb<hq_client, &hq_client::_write_sendbuf_cb>, this);
  
  res_.sendbuf.clear();
  if (sendfile_fd != -1) {
    res_.closed_by_sender = true;
    res_.sendfile.fd = sendfile_fd;
    res_.sendfile.pos = 0;
    struct stat st;
    int r = fstat(res_.sendfile.fd, &st);
    assert(r == 0); // fstat error is really fatal
    res_.sendfile.size = st.st_size;
  } else {
    res_.closed_by_sender = false;
    res_.sendfile.fd = -1;
    res_.sendfile.pos = 0;
    res_.sendfile.size = 0;
  }
  
  // TODO support for keep-alive and HTTP/1.1
  {
    char buf[sizeof("HTTP/1.0 -1234567890 ")];
    sprintf(buf, "HTTP/1.1 %d ", status);
    res_.sendbuf.push(buf, strlen(buf));
  }
  res_.sendbuf.push(msg);
  res_.sendbuf.push("\r\n", 2);
  if (res_.sendfile.fd != -1) {
    char buf[sizeof("Content-Length: \r\n") + 24];
    sprintf(buf, "Content-Length: %llu\r\n",
	    (unsigned long long)res_.sendfile.size);
    res_.sendbuf.push(buf, strlen(buf));
  }
  for (hq_headers::const_iterator i = headers.begin();
       i != headers.end();
       ++i) {
    // TODO should we sanitize headers like Connection, Content-Length here?
    res_.sendbuf.push(i->first);
    res_.sendbuf.push(": ", 2);
    res_.sendbuf.push(i->second);
    res_.sendbuf.push("\r\n", 2);
  }
  res_.sendbuf.push("\r\n", 2);
}

void hq_client::_finalize_response()
{
  // TODO support for persistent connection
  delete this;
}

void hq_client::_write_sendfile_cb(int fd, int revents)
{
  assert(fd_ == fd);
  assert((revents & ~(PICOEV_WRITE | PICOEV_TIMEOUT)) == 0);
  
  // flush header
  if (! res_.sendbuf.empty()) {
    if (! _write_sendbuf(false)) {
      goto ON_CLOSE;
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
    off_t len = 1048576;
    if ((r = sendfile(fd_, res_.sendfile.fd, res_.sendfile.pos, &len, NULL, 0))
	== 0) {
      res_.sendfile.pos += len;
    }
  }
#else
  #error "do not know the sendfile API for this OS"
#endif
  if (r == 0) {
    if (res_.sendfile.pos == res_.sendfile.size) {
      _finalize_response();
      return;
    }
  } else if (errno == EINTR) {
    goto RETRY;
  } else if (! (errno == EAGAIN || EWOULDBLOCK)) {
    // TODO LOG
    goto ON_CLOSE;
  }
  picoev_set_events(hq_loop::get_loop(), fd_, PICOEV_WRITE);
  return;
  
 ON_CLOSE:
  delete this;
}

void hq_client::_write_sendbuf_cb(int fd, int revents)
{
  assert(fd_ == fd);
  assert((revents & ~(PICOEV_WRITE | PICOEV_TIMEOUT)) == 0);
  
  if (! _write_sendbuf(true)) {
    return;
  }
  if (res_.sendbuf.empty() && res_.closed_by_sender) {
    _finalize_response();
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
    // TODO LOG
    goto ON_CLOSE;
  }
  picoev_set_events(hq_loop::get_loop(), fd_,
		    res_.sendbuf.empty() ? 0 : PICOEV_WRITE);
  return true;
  
 ON_CLOSE:
  delete this;
  return false;
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
  _prepare_next();
}

hq_worker::~hq_worker()
{
  assert(req_ == NULL);
  assert(res_sender_ == NULL);
  if (picoev_is_active(hq_loop::get_loop(), fd_)) {
    picoev_del(hq_loop::get_loop(), fd_);
  }
  close(fd_);
}

void hq_worker::_prepare_next()
{
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
  
  picoev_add(hq_loop::get_loop(), fd_, PICOEV_READ, 0,
	     hq_picoev_cb<hq_worker, &hq_worker::_send_request_cb>, this);
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
  int r;
  
 RETRY:
  if ((r = write(fd_, buf_.buffer(), buf_.size())) == -1) {
    if (r == EINTR) {
      goto RETRY;
    } else if (r == EAGAIN || r == EWOULDBLOCK) {
      return;
    } else {
      _return_error(500, "worker connection reset");
      goto CLOSE;
    }
  }
  // sent some data
  buf_.advance(r);
  if (! buf_.empty()) {
    return;
  }
  // sent all data, wait for response
  picoev_set_events(hq_loop::get_loop(), fd_, PICOEV_READ);
  picoev_set_callback(hq_loop::get_loop(), fd_,
		      hq_picoev_cb<hq_worker,&hq_worker::_read_response_header>,
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
    // TODO LOG
    _return_error(500, "connection closed by worker");
    goto CLOSE;
  } else if (r == -1) {
    if (r == EINTR) {
      goto RETRY;
    } else if (r == EAGAIN || r == EWOULDBLOCK) {
      return;
    } else {
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
      // TODO LOG
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
    // TODO LOG
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
    // TODO LOG
    goto CLOSE;
  } else if (r == -1) {
    if (r == EINTR) {
      goto RETRY;
    } else if (r == EAGAIN || r == EWOULDBLOCK) {
      return;
    } else {
      // TODO LOG
      goto CLOSE;
    }
  }
  // got data
  if (res_sender_ != NULL) {
    if (! res_sender_->send_response(buf, r)) {
      // TODO LOG
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

hq_worker::handler hq_worker::handler_;

static void setup_sock(int fd)
{
  int on = 1, r;
  r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  assert(r == 0);
  r = fcntl(fd, F_SETFL, O_NONBLOCK);
  assert(r == 0);
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

hq_headers::const_iterator hq_util::find_header(const hq_headers& hdrs, const string& name)
{
  hq_headers::const_iterator i = hdrs.begin();
  if (i != hdrs.end()) {
    do {
      if (i->first.size() != name.size()) {
	goto NOT_EQUAL;
      }
      for (size_t j = 0; j < name.size(); ++j) {
	if (tolower(i->first[j]) != tolower(name[j])) {
	  goto NOT_EQUAL;
	}
      }
      break;
    NOT_EQUAL:
      ;
    } while (++i != hdrs.end());
  }
  return i;
}

int main(int argc, char** argv)
{
  int listen_fd, flag, r;
  
  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(listen_fd != -1);
  flag = 1;
  r = setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
  assert(r == 0);
  struct sockaddr_in sa;
  sa.sin_family = AF_INET;
  sa.sin_port = htons(10987);
  sa.sin_addr.s_addr = htonl(0);
  r = bind(listen_fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
  assert(r == 0);
  setup_sock(listen_fd);
  r = listen(listen_fd, SOMAXCONN);
  assert(r == 0);
  
  picoev_init(MAX_FDS);
  
  new hq_listener(listen_fd);
  
  hq_loop loop;
  loop.run_loop();
  
  return 0;
}
