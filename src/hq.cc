#include <stdio.h>

extern "C" {
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
}
#include <algorithm>

extern "C" {
#include "picohttpparser/picohttpparser.h"
}
#include "hq.hh"

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
  const char* method, * path;
  phr_header hdrs[MAX_HEADERS];
  size_t method_len, path_len, num_hdrs = MAX_HEADERS;
  int minor_version, r;

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
  num_hdrs = MAX_HEADERS;
  r = phr_parse_request(buf_.buffer(), buf_.size(), &method, &method_len, &path,
			&path_len, &minor_version, hdrs, &num_hdrs, 0);
  if (r == -1) { // error
    // TODO LOG
    return false;
  } else if (r == -2) { // partial
    return true;
  }
  // got request
  method_.insert(method_.end(), method, method + method_len);
  path_.insert(path_.end(), path, path + path_len);
  for (size_t i = 0; i < num_hdrs; i++) {
    if (hdrs[i].name == NULL) {
      // continuing line
      assert(i != 0);
      headers_.back().second.insert(headers_.back().second.end(), hdrs[i].value,
				    hdrs[i].value + hdrs[i].value_len);
    } else {
      headers_.push_back(make_pair(string(hdrs[i].name,
					  hdrs[i].name + hdrs[i].name_len),
				   string(hdrs[i].value,
					  hdrs[i].value + hdrs[i].value_len)));
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
  static hq_worker::handler worker_handler;
  if (worker_handler.dispatch(req, res_sender)) {
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
  picoev_del(hq_loop::get_loop(), fd_);
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

bool hq_client::open_response(int status, const string& msg, const hq_headers& headers, const char* data, size_t len)
{
  picoev_add(hq_loop::get_loop(), fd_, 0, 0,
	     hq_picoev_cb<hq_client, &hq_client::_write_sendbuf_cb>, this);
  
  res_.sendbuf.clear();
  res_.closed_by_sender = false;
  
  // TODO support for keep-alive and HTTP/1.1
  char buf[sizeof("HTTP/1.0 -1234567890 ")];
  sprintf(buf, "HTTP/1.1 %d ", status);
  res_.sendbuf.push(buf, strlen(buf));
  res_.sendbuf.push(msg);
  res_.sendbuf.push("\r\n", 2);
  for (hq_headers::const_iterator i = headers.begin(); i != headers.end();
       ++i) {
    // TODO should we sanitize headers like Connection here?
    res_.sendbuf.push(i->first);
    res_.sendbuf.push(": ", 2);
    res_.sendbuf.push(i->second);
    res_.sendbuf.push("\r\n", 2);
  }
  res_.sendbuf.push("\r\n", 2);
  
  // TODO check content-length and content-encoding to privent res. splitting
  
  if (len != 0) {
    res_.sendbuf.push(data, len);
  }
  
  return _write_sendbuf();
}

bool hq_client::send_response(const char* data, size_t len)
{
  res_.sendbuf.push(data, len);
  return _write_sendbuf();
}

void hq_client::close_response()
{
  res_.closed_by_sender = true;
  if (res_.sendbuf.empty()) {
    _finalize_response();
  }
}

void hq_client::_finalize_response()
{
  // TODO support for persistent connection
  delete this;
}

void hq_client::_write_sendbuf_cb(int fd, int revents)
{
  assert(fd_ == fd);
  assert((revents & ~(PICOEV_WRITE | PICOEV_TIMEOUT)) == 0);
  
  if (! _write_sendbuf()) {
    return;
  }
  if (res_.sendbuf.empty() && res_.closed_by_sender) {
    _finalize_response();
  }
}

bool hq_client::_write_sendbuf()
{
  int r;
  
 RETRY:
  r = write(fd_, res_.sendbuf.buffer(), res_.sendbuf.size());
  switch (r) {
  case 0: // closed by peer
    // TODO LOG
    goto ON_CLOSE;
  case -1: // error
    switch (errno) {
    case EINTR:
      goto RETRY;
    case EWOULDBLOCK:
      picoev_set_events(hq_loop::get_loop(), fd_, PICOEV_WRITE);
      break;
    default:
      // TODO LOG
      goto ON_CLOSE;
    }
    break;
  default: // wrote some bytes
    res_.sendbuf.advance(r);
    if (res_.sendbuf.empty()) {
      picoev_set_events(hq_loop::get_loop(), fd_, 0);
    }
    break;
  }
  return true;
  
 ON_CLOSE:
  delete this;
  return false;
}

hq_worker::handler::handler()
  : junction_(NULL)
{
  pthread_cond_init(&junction_cond_, NULL);
}

hq_worker::handler::~handler()
{
  // TODO gracefully shutdown
  pthread_cond_destroy(&junction_cond_);
}

bool hq_worker::handler::dispatch(const hq_req_reader& req, hq_res_sender* res_sender)
{
  cac_mutex_t<junction>::lockref junction(junction_);
  junction->reqs.push_back(req_queue_entry(&req, res_sender));
  if (! junction->workers.empty()) {
    pthread_cond_signal(&junction_cond_);
  }
  return true;
}

bool hq_worker::handler::_fetch_request(const hq_req_reader*& req, hq_res_sender*& res_sender)
{
  cac_mutex_t<junction>::lockref junction(junction_);
  
  while (junction->reqs.empty()) {
    pthread_cond_wait(&junction_cond_, junction_.mutex());
  }

  req = junction->reqs.front().req;
  res_sender = junction->reqs.front().res_sender;
  junction->reqs.pop_front();
  
  return true;
}

hq_worker::hq_worker(int fd, const hq_req_reader&)
  : fd_(fd), req_(NULL), res_sender_(NULL), buf_()
{
  _start();
}

hq_worker::~hq_worker()
{
  assert(req_ == NULL);
  assert(res_sender_ == NULL);
  picoev_del(hq_loop::get_loop(), fd_);
  close(fd_);
}

void hq_worker::_start()
{
  // fetch entry from queue
  while (! handler_._fetch_request(req_, res_sender_)) {
  }
  
  // build request
  buf_.clear();
  buf_.push(req_->method());
  buf_.push(' ');
  buf_.push(req_->path());
  buf_.push(' ');
  buf_.push("HTTP/1.0");
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
  if ((r = write(fd_, buf_.buffer(), buf_.size())) == 0) {
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
  const char* msg;
  phr_header headers[MAX_HEADERS];
  size_t msg_len, num_headers;
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

  // try to parse the response
  num_headers = MAX_HEADERS;
  r = phr_parse_response(buf_.buffer(), buf_.size(), &minor_version, &status,
			 &msg, &msg_len, headers, &num_headers, 0);
  if (r == -1) { // error
    // TODO LOG
    _return_error(500, "worker response error");
    goto CLOSE;
  } else if (r == -2) { // partial
    return;
  }
  
  // got response
  for (size_t i = 0; i < num_headers; ++i) {
    // TODO prune headers
    hdrs.push_back(make_pair(string(headers[i].name,
				    headers[i].name + headers[i].name_len),
			     string(headers[i].value,
				    headers[i].value + headers[i].value_len)));
  }
  // TODO check content boundary, etc.
  if (! res_sender_->open_response(status, string(msg, msg + msg_len), hdrs,
				   buf_.buffer(), buf_.size())) {
    // TODO LOG
    res_sender_ = NULL;
  }
  buf_.advance(buf_.size());
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

hq_loop::hq_loop(int listen_fd)
  : listen_fd_(listen_fd), loop_(NULL)
{
  loop_ = picoev_create_loop(TIMEOUT_SECS);
  picoev_add(loop_, listen_fd_, PICOEV_READ, 0,
	     hq_picoev_cb<hq_loop, &hq_loop::accept_conn>, this);
}

void hq_loop::accept_conn(int fd, int revents)
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

picoev_loop* hq_loop::get_loop()
{
  // TODO fixme
  return NULL;
}

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
  fputs("hello world!\n", stdout);
  return 0;
}
