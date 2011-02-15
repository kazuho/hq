extern "C" {
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
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
    if (! buf_.empty()) {
      picolog::info() << picolog::mem_fun(hq_util::gethostof, fd)
		      << " closed by peer while reading the request";
    }
    return false;
  } else if (r == -1) { // error
    if (errno == EINTR) {
      goto RETRY;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    } else {
      picolog::error() << picolog::mem_fun(hq_util::gethostof, fd) << ' '
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
  static const hq_util::lcstr transfer_encoding("transfer-encoding");
  if (hq_util::find_header(headers_, transfer_encoding) != headers_.end()) {
    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		     << "sorry requests using chunked encoding not supported";
    return false;
  }
  static const hq_util::lcstr content_length("content-length");
  hq_headers::const_iterator clen_iter
    = hq_util::find_header(headers_, content_length);
  if (clen_iter == headers_.end()) {
    state_ = READ_COMPLETE;
    return true;
  }
  // have content-length
  if ((content_length_
       = hq_util::parse_positive_number(clen_iter->second.c_str(),
					clen_iter->second.size()))
      == -1) {
    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		     << " got an invalid content-length header, closing";
    return false;
  }
  content_ = new hq_buffer();
  if (content_length_ <= (int64_t)buf_.size()) {
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
  int maxlen = min<int64_t>(content_length_ - content_->size(), 1048576), r;
  
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

hq_client::io_timeout_config::io_timeout_config()
  : picoopt::config_base<io_timeout_config>("io-timeout", required_argument,
					    "=seconds")
{
}

int hq_client::io_timeout_config::setup(const string* secs, string& err)
{
  if (sscanf(secs->c_str(), "%d", &timeout_) != 1 || timeout_ <= 0) {
    err = "timeout should be a positive number";
    return 1;
  }
  return 0;
}

hq_client::hq_client(role r, int fd)
  : role_(r), fd_(fd), req_()
{
  ++*cac_mutex_t<size_t>::lockref(cnt_);
  picoev_add(hq_loop::get_loop(), fd_, 0, 0, NULL, this);
  _reset(false);
}

hq_client::~hq_client()
{
  if (picoev_is_active(hq_loop::get_loop(), fd_)) {
    picoev_del(hq_loop::get_loop(), fd_);
  }
  close(fd_);
  --*cac_mutex_t<size_t>::lockref(cnt_);
}

void hq_client::_reset(bool is_keep_alive)
{
  req_.reset();
  res_.status = 0;
  res_.sendbuf.clear();
  keep_alive_ = false;
  picoev_set_events(hq_loop::get_loop(), fd_, PICOEV_READ);
  picoev_set_timeout(hq_loop::get_loop(), fd_, timeout());
  picoev_set_callback(hq_loop::get_loop(), fd_,
		      hq_picoev_cb<hq_client, &hq_client::_read_request>, NULL);
  if (is_keep_alive) {
    hq_loop::register_stoppable(this);
  }
}

void hq_client::_read_request(int fd, int revents)
{
  assert(fd == fd_);
  assert((revents & ~(PICOEV_READ | PICOEV_TIMEOUT)) == 0);
  
  // timeout
  if ((revents & PICOEV_TIMEOUT) != 0) {
    picolog::info() << picolog::mem_fun(hq_util::gethostof, fd_)
		    << " timeout while reading request";
    delete this;
    return;
  }
  
  if (req_.empty()) {
    hq_loop::unregister_stoppable(this);
  }
  
  // read request
  if (! req_.read_request(fd_)) {
    delete this;
    return;
  }
  if (! req_.is_complete()) {
    picoev_set_timeout(hq_loop::get_loop(), fd_, timeout());
    return;
  }
  // if the connection is a worker, then...
  if (req_.method() == "START_WORKER") {
    if (! (role_ == ROLE_ANY || role_ == ROLE_WORKER)) {
      picoev_del(hq_loop::get_loop(), fd_);
      hq_handler::send_error(req_, this, 403, "method not allowed");
      return;
    }
    int newfd = dup(fd_);
    if (newfd == -1) {
      picolog::error() << "dup(2) failed, " << hq_util::strerror(errno);
      picoev_del(hq_loop::get_loop(), fd_);
      hq_handler::send_error(req_, this, 500, "dup failure");
      return;
    }
    new hq_worker(newfd, req_);
    delete this;
    return;
  }
  // is a client
  if (! (role_ == ROLE_ANY || role_ == ROLE_CLIENT)) {
    picoev_del(hq_loop::get_loop(), fd_);
    hq_handler::send_error(req_, this, 403, "method not allowed");
    return;
  }
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
  _prepare_response(status, msg, headers, -1);
  res_.closed_by_sender = false;
  if (len != 0) {
    switch (res_.mode) {
    case RESPONSE_MODE_HTTP10:
      _push_http10_data(data, len);
      break;
    case RESPONSE_MODE_CHUNKED:
      _push_chunked_data(data, len);
      break;
    default:
      assert(0);
    }
  }
  return _write_sendbuf(true);
}

bool hq_client::send_response(const char* data, size_t len)
{
  if (len == 0) {
    return true;
  }
  switch (res_.mode) {
  case RESPONSE_MODE_HTTP10:
    _push_http10_data(data, len);
    break;
  case RESPONSE_MODE_CHUNKED:
    _push_chunked_data(data, len);
    break;
  default:
    assert(0);
  }
  return _write_sendbuf(true);
}

void hq_client::close_response()
{
  switch (res_.mode) {
  case RESPONSE_MODE_HTTP10:
    if (res_.u.http10.off != res_.u.http10.content_length) {
      keep_alive_ = false;
    }
    break;
  case RESPONSE_MODE_CHUNKED:
    res_.sendbuf.push("0\r\n\r\n", 5);
    if (! _write_sendbuf(true)) {
      _finalize_response(false);
      return;
    }
    break;
  default:
    assert(0);
  }
  
  res_.closed_by_sender = true;
  if (res_.sendbuf.empty()) {
    _finalize_response(true);
  }
}

void hq_client::_prepare_response(int status, const string& msg, const hq_headers& headers, const int sendfile_fd)
{
  picoev_add(hq_loop::get_loop(), fd_, 0, 0, NULL, this);
  picoev_set_timeout(hq_loop::get_loop(), fd_, timeout());
  
  res_.status = status;
  res_.sendbuf.clear();
  if (sendfile_fd != -1) {
    res_.closed_by_sender = true;
    res_.mode = RESPONSE_MODE_SENDFILE;
    res_.u.sendfile.fd = sendfile_fd;
    res_.u.sendfile.pos = 0;
    struct stat st;
    int r = fstat(res_.u.sendfile.fd, &st);
    assert(r == 0); // fstat error is really fatal
    res_.u.sendfile.size = st.st_size;
    picoev_set_callback(hq_loop::get_loop(), fd_,
			hq_picoev_cb<hq_client, &hq_client::_write_sendfile_cb>,
			NULL);
  } else {
    res_.closed_by_sender = false;
    res_.mode = RESPONSE_MODE_HTTP10;
    res_.u.http10.off = 0;
    res_.u.http10.content_length = -1;
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
  if (res_.mode == RESPONSE_MODE_SENDFILE) {
    char buf[sizeof("Content-Length: \r\n") + 24];
    sprintf(buf, "Content-Length: %llu\r\n",
	    (unsigned long long)res_.u.sendfile.size);
    res_.sendbuf.push(buf, strlen(buf));
    have_content_length = true;
  }
  const static hq_util::lcstr connection("connection"),
    keep_alive("keep-alive"), content_length("content-length"),
    transfer_encoding("transfer-encoding");
  for (hq_headers::const_iterator i = headers.begin();
       i != headers.end();
       ++i) {
    if (hq_util::lceq(i->first, connection)
	|| hq_util::lceq(i->first, transfer_encoding)) {
      // skip transfer-related headers from worker
    } else {
      if (! have_content_length && hq_util::lceq(i->first, content_length)) {
	have_content_length = true;
	if (res_.mode == RESPONSE_MODE_HTTP10) {
	  res_.u.http10.content_length
	    = hq_util::parse_positive_number(i->second.c_str(),
					     i->second.size());
	  assert(res_.u.http10.content_length != -1); // should've been verified
	}
      }
      res_.sendbuf.push(i->first);
      res_.sendbuf.push(": ", 2);
      res_.sendbuf.push(i->second);
      res_.sendbuf.push("\r\n", 2);
    }
  }
  hq_headers::const_iterator connection_header
    = hq_util::find_header(req_.headers(), connection);
  keep_alive_ = req_.minor_version() >= 1
    ? (connection_header == req_.headers().end()
       || hq_util::lceq(connection_header->second, keep_alive))
    : (have_content_length
       && connection_header != req_.headers().end()
       && hq_util::lceq(connection_header->second, keep_alive));
  if (keep_alive_ && ! have_content_length) {
    // use chunked encoding
    res_.sendbuf.push("Transfer-Encoding: chunked\r\n");
    res_.mode = RESPONSE_MODE_CHUNKED;
  }
  // override the keep-alive flag after deciding the transfer method
  if (hq_loop::stop_requested()) {
    keep_alive_ = false;
  }
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
    _reset(true);
  } else {
    delete this;
  }
}

void hq_client::_write_sendfile_cb(int fd, int revents)
{
  assert(fd_ == fd);
  assert((revents & ~(PICOEV_WRITE | PICOEV_TIMEOUT)) == 0);
  assert(res_.mode == RESPONSE_MODE_SENDFILE);
  
  // timeout
  if ((revents & PICOEV_TIMEOUT) != 0) {
    picolog::info() << picolog::mem_fun(hq_util::gethostof, fd_)
		    << " timeout while sending response";
    _finalize_response(false);
    return;
  }
  
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
  r = sendfile(fd_, res_.u.sendfile.fd, &res_.u.sendfile.pos, 1048576);
  if (r >= 0) r = 0; // for compat. w. bsd-style sendfile
#elif HQ_IS_BSD
  {
    off_t len = min((off_t)1048576, res_.u.sendfile.size);
    r = sendfile(res_.u.sendfile.fd, fd_, res_.u.sendfile.pos, &len, NULL, 0);
    if (r == 0 || errno == EAGAIN) {
      res_.u.sendfile.pos += len;
    }
  }
#else
  #error "do not know the sendfile API for this OS"
#endif
  if (r == 0) {
    if (res_.u.sendfile.pos == res_.u.sendfile.size) {
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
  assert(res_.mode == RESPONSE_MODE_HTTP10
	 || res_.mode == RESPONSE_MODE_CHUNKED);
  
  // timeout
  if ((revents & PICOEV_TIMEOUT) != 0) {
    picolog::info() << picolog::mem_fun(hq_util::gethostof, fd_)
		    << " timeout while sending response";
    _finalize_response(false);
    return;
  }
  
  if (! _write_sendbuf(true)) {
    _finalize_response(false);
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

void hq_client::_push_http10_data(const char* data, size_t len)
{
  res_.sendbuf.push(data, len);
  res_.u.http10.off += len;
  assert(res_.u.http10.content_length == -1
	 || res_.u.http10.off <= res_.u.http10.content_length);
}

void hq_client::_push_chunked_data(const char* data, size_t len)
{
  while (len != 0) {
    unsigned chunk_len = (unsigned)min(len, (size_t)1048576);
    char buf[16];
    sprintf(buf, "%x\r\n", chunk_len);
    res_.sendbuf.push(buf, strlen(buf));
    res_.sendbuf.push(data, chunk_len);
    res_.sendbuf.push("\r\n", 2);
    data += chunk_len;
    len -= chunk_len;
  }
}

void hq_client::from_loop_request_stop()
{
  delete this;
}

cac_mutex_t<size_t> hq_client::cnt_(NULL);
int hq_client::timeout_ = 90;

hq_worker::queue_timeout_config::queue_timeout_config()
  : picoopt::config_base<queue_timeout_config>("queue-timeout",
					       required_argument,
					       "=seconds")
{
}

int hq_worker::queue_timeout_config::setup(const string* seconds, string& err)
{
  if (sscanf(seconds->c_str(), "%d", &queue_timeout_) != 1
      || queue_timeout_ <= 0) {
    err = "queue-timeout should be a positive number";
    return 1;
  }
  return 0;
}

hq_worker::handler::req_queue_entry::req_queue_entry(const hq_req_reader* r,
						     hq_res_sender* rs)
  : req(r), res_sender(rs)
{
  time(&at);
}

hq_worker::handler::handler()
  : junction_(NULL)
{
}

hq_worker::handler::~handler()
{
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
      worker = junction->workers.front().first;
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
    junction->workers.push_back(make_pair(worker, time(NULL)));
    return;
  }
  req_queue_entry r(junction->reqs.front());
  junction->reqs.pop_front();
  worker->_start(r.req, r.res_sender);
}

void hq_worker::handler::trash_idle_connections()
{
  if (queue_timeout_ == 0) {
    return;
  }
  
  cac_mutex_t<junction>::lockref junction(junction_);
  time_t now = time(NULL);
  
  while (! junction->reqs.empty()) {
    req_queue_entry r(*junction->reqs.begin());
    if (now <= r.at + queue_timeout_) {
      break;
    }
    picolog::error() << "could not handle " << r.req->method() << ' '
		     << r.req->path() << ", timeout occured"; 
    hq_handler::send_error(*r.req, r.res_sender, 500,
			   "Request Timeout");
    junction->reqs.pop_front();
  }
  while (! junction->workers.empty()) {
    pair<hq_worker*, time_t> e(*junction->workers.begin());
    if (now <= e.second + queue_timeout_) {
      break;
    }
    // TODO log the closing, but how to get the fd_?
    delete e.first;
    junction->workers.pop_front();
  }
}

hq_worker::hq_worker(int fd, const hq_req_reader&)
  : fd_(fd), req_(NULL), res_sender_(NULL), buf_(), keep_alive_(true)
{
  ++*cac_mutex_t<size_t>::lockref(cnt_);
  picoev_add(hq_loop::get_loop(), fd_, 0, 0, NULL, this);
  buf_.push("HTTP/1.1 101 Upgrade\r\n"
	    "Upgrade: HTTPWORKER/0.9\r\n\r\n");
  _send_upgrade(fd, PICOEV_WRITE);
}

hq_worker::~hq_worker()
{
  assert(req_ == NULL);
  assert(res_sender_ == NULL);
  if (picoev_is_active(hq_loop::get_loop(), fd_)) {
    picoev_del(hq_loop::get_loop(), fd_);
  }
  close(fd_);
  --*cac_mutex_t<size_t>::lockref(cnt_);
}

void hq_worker::_send_upgrade(int fd, int revents)
{
  assert(fd == fd_);
  assert((revents & ~(PICOEV_WRITE | PICOEV_TIMEOUT)) == 0);
  
  // timeout
  if ((revents & PICOEV_TIMEOUT) != 0) {
    picolog::info() << picolog::mem_fun(hq_util::gethostof,fd)
		    << " closing worker due to timeout while sending request";
    delete this;
    return;
  }
  
  if (! _send_buffer()) {
    picolog::info() << picolog::mem_fun(hq_util::gethostof, fd)
		    << " worker closed the connection unexpectedly, "
		    << hq_util::strerror(errno);
    delete this;
    return;
  }
  if (! buf_.empty()) {
    picoev_set_events(hq_loop::get_loop(), fd_, PICOEV_WRITE);
    picoev_set_timeout(hq_loop::get_loop(), fd_, hq_client::timeout());
    picoev_set_callback(hq_loop::get_loop(), fd_,
			hq_picoev_cb<hq_worker, &hq_worker::_send_upgrade>,
			NULL);
  } else {
    _prepare_next();
  }
}

void hq_worker::_prepare_next()
{
  assert(req_ == NULL);
  assert(res_sender_ == NULL);
  
  if (picoev_is_active(hq_loop::get_loop(), fd_)) {
    picoev_del(hq_loop::get_loop(), fd_);
  }
  // fetch the request immediately or register myself to dispatcher
  handler_->_start_worker_or_register(this);
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
  buf_.push(" HTTP/1.1\r\n", sizeof(" HTTP/1.1\r\n") - 1);
  buf_.push("Connection: keep-alive\r\n");
  for (hq_headers::const_iterator i = req_->headers().begin();
       i != req_->headers().end();
       ++i) {
    static const hq_util::lcstr connection("connection");
    if (hq_util::lceq(i->first, connection)) {
      // skip
    } else {
      buf_.push(i->first);
      buf_.push(": ", 2);
      buf_.push(i->second);
      buf_.push("\r\n", 2);
    }
  }
  buf_.push("\r\n", 2);
  
  picoev_add(hq_loop::get_loop(), fd_, 0, 0, NULL, this);
  _send_request();
}

void hq_worker::_send_request_cb(int fd, int revents)
{
  assert(fd == fd_);
  assert((revents & ~(PICOEV_WRITE | PICOEV_TIMEOUT)) == 0);
  
  // timeout
  if ((revents & PICOEV_TIMEOUT) != 0) {
    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd_)
		     << " timeout while sending request to worker";
    _return_error(500, "worker timeout");
    delete this;
    return;
  }
  
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
    picoev_set_timeout(hq_loop::get_loop(), fd_, hq_client::timeout());
    picoev_set_callback(hq_loop::get_loop(), fd_,
			hq_picoev_cb<hq_worker, &hq_worker::_send_request_cb>,
			NULL);
    return;
  }
  // sent all data, wait for response
  picoev_set_events(hq_loop::get_loop(), fd_, PICOEV_READ);
  picoev_set_timeout(hq_loop::get_loop(), fd_, hq_client::timeout());
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
  
  // timeout
  if ((revents & PICOEV_TIMEOUT) != 0) {
    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd_)
		     << " timeout while receiving response from worker";
    _return_error(500, "worker timeout");
    delete this;
    return;
  }
  
  int status, minor_version, r;
  string msg;
  hq_headers hdrs;
  
  bool closed;
  if (! _recv_buffer(&closed)) {
    if (closed) {
      picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
		       << " worker closed the connection";
    }
    _return_error(500, "connection closed by worker");
    goto CLOSE;
  }

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
    keep_alive_ = minor_version >= 1;
    res_.mode = RESPONSE_MODE_HTTP10;
    res_.u.http10.content_length = -1;
    for (size_t i = 0; i < num_headers; ++i) {
      static const hq_util::lcstr connection("connection");
      if (hq_util::lceq(headers[i].name, headers[i].name_len, connection)) {
	static const hq_util::lcstr keep_alive("keep-alive");
	keep_alive_ = hq_util::lceq(headers[i].value, headers[i].value_len,
				    keep_alive);
      } else {
	static const hq_util::lcstr transfer_encoding("transfer-encoding"),
	  content_length("content-length");
	if (hq_util::lceq(headers[i].name, headers[i].name_len,
			  transfer_encoding)) {
	  static const hq_util::lcstr chunked("chunked");
	  if (hq_util::lceq(headers[i].value, headers[i].value_len, chunked)) {
	    res_.mode = RESPONSE_MODE_CHUNKED;
	  } else {
	    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd_)
			     << " received unknown transfer-encoding: \""
			     << string(headers[i].value, headers[i].value_len)
			     << "\" from worker, closing";
	    _return_error(500, "worker response error");
	    goto CLOSE;
	  }
	} else if (hq_util::lceq(headers[i].name, headers[i].name_len,
				 content_length)) {
	  res_.mode = RESPONSE_MODE_HTTP10;
	  if ((res_.u.http10.content_length =
	       hq_util::parse_positive_number(headers[i].value,
					      headers[i].value_len))
	      == -1) {
	    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd)
			     << (" worker returned an invalid content-length"
				 " header, closing");
	    _return_error(500, "worker response error");
	    goto CLOSE;
	  }
	}
	hdrs.push_back(make_pair(string(headers[i].name,
					headers[i].name + headers[i].name_len),
				 string(headers[i].value,
					headers[i].value
					+ headers[i].value_len)));
      }
    }
    buf_.advance(r);
  }
  
  // adjust keep-alive-related flags
  if (res_.mode == RESPONSE_MODE_HTTP10 && res_.u.http10.content_length == -1) {
    keep_alive_ = false;
  }
  // open the response handler
  switch (res_.mode) {
  case RESPONSE_MODE_HTTP10:
    {
      int64_t send_len = buf_.size();
      if (res_.u.http10.content_length != -1) {
	send_len = min(send_len, res_.u.http10.content_length);
      }
      if (! res_sender_->open_response(status, msg, hdrs, buf_.buffer(),
				       send_len)) {
	req_ = NULL;
	res_sender_ = NULL;
      }
      buf_.advance(send_len);
      res_.u.http10.off = send_len;
      if (res_.u.http10.content_length != -1
	  && res_.u.http10.off == res_.u.http10.content_length) {
	if (res_sender_ != NULL) {
	  res_sender_->close_response();
	  req_ = NULL;
	  res_sender_ = NULL;
	}
	if (! buf_.empty()) {
	  picolog::warn() << picolog::mem_fun(hq_util::gethostof, fd)
			   << " worker sent too many bytes of data, closing";
	  goto CLOSE;
	}
	_prepare_next();
	return;
      }
      buf_.clear();
      picoev_set_callback(hq_loop::get_loop(), fd_,
			  hq_picoev_cb<hq_worker,
			               &hq_worker::_read_response_http10>,
			  NULL);
    }
    return;
  case RESPONSE_MODE_CHUNKED:
    res_.u.chunked.chunk_off = 0;
    res_.u.chunked.chunk_size = -1;
    if (! res_sender_->open_response(status, msg, hdrs, NULL, 0)) {
      req_ = NULL;
      res_sender_ = NULL;
    }
    picoev_set_callback(hq_loop::get_loop(), fd_,
			hq_picoev_cb<hq_worker,
			             &hq_worker::_read_response_chunked>,
			NULL);
    _push_chunked_data();
    return;
  }
  
  assert(0);
  return;
  
 CLOSE:
  delete this;
}

bool hq_worker::_recv_buffer(bool* closed)
{
  if (closed != NULL) {
    *closed = false;
  }
  
  char* buf = buf_.prepare(READ_MAX);
  ssize_t r;
  
 RETRY:
  if ((r = read(fd_, buf, READ_MAX)) == 0) {
    if (closed != NULL) {
      *closed = true;
    }
    // closed
    return false;
  } else if (r == -1) {
    if (errno == EINTR) {
      goto RETRY;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // ok
    } else {
      picolog::error() << picolog::mem_fun(hq_util::gethostof, fd_)
		       << " got an I/O error while reading data from worker, "
		       << hq_util::strerror(errno);
      return false;
    }
  } else {
    buf_.adjust_size(r);
  }
  
  return true;
}

void hq_worker::_read_response_http10(int fd, int revents)
{
  assert(fd == fd_);
  assert((revents & ~(PICOEV_READ | PICOEV_TIMEOUT)) == 0);
  assert(buf_.empty());
  
  size_t send_len;
  
  if ((revents & PICOEV_TIMEOUT) != 0) {
    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd_)
		     << " timeout while receiving response content from worker";
    goto CLOSE;
  }
  
  if (! _recv_buffer()) {
    goto CLOSE;
  }
  
  send_len = res_.u.http10.content_length != -1
    ? (size_t)min<int64_t>(buf_.size(), res_.u.http10.content_length)
    : buf_.size();
  if (res_sender_ != NULL) {
    if (! res_sender_->send_response(buf_.buffer(), send_len)) {
      req_ = NULL;
      res_sender_ = NULL;
    }
  }
  buf_.advance(send_len);
  res_.u.http10.off += send_len;
  if (res_.u.http10.content_length != -1
      && res_.u.http10.off == res_.u.http10.content_length) {
    if (res_sender_ != NULL) {
      res_sender_->close_response();
      req_ = NULL;
      res_sender_ = NULL;
    }
    if (! buf_.empty()) {
      picolog::warn() << picolog::mem_fun(hq_util::gethostof, fd)
		      << " worker sent too many bytes of data, closing";
      keep_alive_ = false;
    }
    if (keep_alive_) {
      _prepare_next();
      return;
    }
    goto CLOSE;
  }
  
  assert(buf_.empty());
  return;
  
 CLOSE:
  if (res_sender_ != NULL) {
    res_sender_->close_response();
    req_ = NULL;
    res_sender_ = NULL;
  }
  delete this;
}

void hq_worker::_read_response_chunked(int fd, int revents)
{
  assert(fd == fd_);
  assert((revents & ~(PICOEV_READ | PICOEV_TIMEOUT)) == 0);
  
  if ((revents & PICOEV_TIMEOUT) != 0) {
    picolog::error() << picolog::mem_fun(hq_util::gethostof, fd_)
		     << " timeout while receiving response content from worker";
    delete this;
    return;
  }
  
  if (! _recv_buffer()) {
    if (res_sender_ != NULL) {
      res_sender_->close_response();
      req_ = NULL;
      res_sender_ = NULL;
    }
    delete this;
    return;
  }
  
  _push_chunked_data();
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

void hq_worker::_push_chunked_data()
{
  while (1) {
    
    // handling of trailing bytes after chunked content
    if (res_.u.chunked.chunk_off == res_.u.chunked.chunk_size) {
      switch (res_.u.chunked.chunk_size) {
      case -1: // is start, nothing to do
	break;
      default: // not at EOF, read trailing CRLF of current chunk
	if (buf_.size() < 2) {
	  return;
	}
	if (memcmp(buf_.buffer(), "\r\n", 2) != 0) {
	  picolog::error() << picolog::mem_fun(hq_util::gethostof, fd_)
			   << (" received a broken chunked content from worker,"
			       " closing");
	  keep_alive_ = false;
	  goto CLOSE;
	}
	buf_.advance(2);
	res_.u.chunked.chunk_size = -1;
	break;
      case 0: // EOF, skip until we find CRLFCRLF
	// TODO add support for trailing headers? (ignored for the time being)
	while (buf_.size() >= 4) {
	  if (memcmp(buf_.buffer(), "\r\n\r\n", 4) == 0) {
	    buf_.advance(4);
	    goto CLOSE;
	  }
	  buf_.advance(1);
	}
	return;
      }
    }
    
    // read the chunk header
    if (res_.u.chunked.chunk_size == -1) {
      const static char* CRLF = "\r\n";
      const char* crlf = search(buf_.buffer(), buf_.buffer() + buf_.size(),
				CRLF, CRLF + 2);
      if (crlf == buf_.buffer() + buf_.size()) {
	if (buf_.size() >= 65536) {
	  picolog::error() << picolog::mem_fun(hq_util::gethostof, fd_)
			   << " cannot handle chunk-ext over 64kb, closing";
	  keep_alive_ = false;
	  goto CLOSE;
	}
	return;
      }
      {
	long long unsigned t;
	if (sscanf(buf_.buffer(), "%llx", &t) != 1) {
	  picolog::error() << picolog::mem_fun(hq_util::gethostof, fd_)
			   << (" received a broken chunked content from worker,"
			       " closing");
	  keep_alive_ = false;
	  goto CLOSE;
	}
	res_.u.chunked.chunk_size = t;
      }
      res_.u.chunked.chunk_off = 0;
      if (res_.u.chunked.chunk_size == 0) {
	// for eof we keep the last CRLF to detect the end of trailing headers by CRLFCRLF, kinda nasty...
	buf_.advance(crlf - buf_.buffer());
	continue;
      }
      buf_.advance(crlf - buf_.buffer() + 2);
    }
    
    // push the chunked body
    while (res_.u.chunked.chunk_off < res_.u.chunked.chunk_size) {
      if (buf_.size() == 0) {
	return;
      }
      size_t sz = min<size_t>(min<int64_t>(res_.u.chunked.chunk_size
					   - res_.u.chunked.chunk_off,
					   1048576),
			      buf_.size());
      if (res_sender_ != NULL) {
	if (! res_sender_->send_response(buf_.buffer(), sz)) {
	  req_ = NULL;
	  res_sender_ = NULL;
	}
      }
      buf_.advance(sz);
      res_.u.chunked.chunk_off += sz;
    }
    
  }

 CLOSE:
  if (res_sender_ != NULL) {
    res_sender_->close_response();
    req_ = NULL;
    res_sender_ = NULL;
  }
  if (keep_alive_) {
    _prepare_next();
    return;
  }
  delete this;
}

cac_mutex_t<size_t> hq_worker::cnt_(NULL);
hq_worker::handler* hq_worker::handler_ = NULL;
int hq_worker::queue_timeout_ = 30;

void hq_worker::setup()
{
  assert(handler_ == NULL);
  handler_ = new handler;
  hq_handler::handlers_.push_back(handler_);
}

static void setup_sock(int fd)
{
  int on = 1, r;
  r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  assert(r == 0);
  r = fcntl(fd, F_SETFL, O_NONBLOCK);
  assert(r == 0);
}

hq_listener::port_config::port_config()
  : picoopt::config_base<port_config>("port", required_argument,
				 "=[host:]port")
{
}

int hq_listener::port_config::setup(const string* hostport, string& err)
{
  return hq_listener::_create_listener(hq_client::ROLE_ANY, *hostport, err);
}

int hq_listener::port_config::post_setup(string& err)
{
  if (hq_listener::listeners_.empty()) {
    err = "should be set more than once";
    return 1;
  }
  return 0;
}

#if 0

hq_listener::client_port_config::client_port_config()
  :  picoopt::config_base<client_port_config>("client-port", required_argument,
					      "=[host:]port")
{
}

int hq_listener::client_port_config::setup(const string* hostport, string& err)
{
  return hq_listener::_create_listener(hq_client::ROLE_CLIENT, *hostport, err);
}

hq_listener::worker_port_config::worker_port_config()
  :  picoopt::config_base<worker_port_config>("worker-port", required_argument,
					      "=[host:]port")
{
}

int hq_listener::worker_port_config::setup(const string* hostport, string& err)
{
  return hq_listener::_create_listener(hq_client::ROLE_WORKER, *hostport, err);
}

#endif

hq_listener::max_connections_config::max_connections_config()
  : picoopt::config_base<max_connections_config>("max-connections",
						  required_argument, "=number")
{
}

int hq_listener::max_connections_config::setup(const string* maxconn,
					       string& err)
{
  if (sscanf(maxconn->c_str(), "%d", &max_connections_) != 1
      || max_connections_ < 1) {
    err= "argumentshould be a positive number";
    return 1;
  }
  return 0;
}

hq_listener::poll_guard::poll_guard()
  : locked_(false)
{
  if (pthread_mutex_trylock(&hq_listener::listeners_mutex_) != 0)
    return;
  // TODO locking two objects using temporary variables, may cause deadlocks
  if ((int)(*cac_mutex_t<size_t>::lockref(hq_worker::cnt_)
	    + *cac_mutex_t<size_t>::lockref(hq_client::cnt_))
      >= max_connections_) {
    static time_t last_alert_at = 0;
    time_t now;
    time(&now);
    if (last_alert_at + 10 <= now) {
      picolog::warn() << "too many connection";
      last_alert_at = now;
    }
    pthread_mutex_unlock(&hq_listener::listeners_mutex_);
    return;
  }
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
    // TODO we need to continue accepting workers if there are any pending clients
    if (hq_loop::stop_requested()) {
      delete *i;
    }
  }
  if (hq_loop::stop_requested()) {
    listeners_.clear();
  }
  pthread_mutex_unlock(&hq_listener::listeners_mutex_);
}

hq_listener::~hq_listener()
{
  close(listen_fd_);
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
  new hq_client(role_, newfd);
}

std::list<hq_listener*> hq_listener::listeners_;
pthread_mutex_t hq_listener::listeners_mutex_ = PTHREAD_MUTEX_INITIALIZER;
int hq_listener::max_connections_ = 151;

int hq_listener::_create_listener(const hq_client::role& role,
				  const string& hostport, string& err)
{
  unsigned short port;
  // TODO parse the host: part
  // TODO IPv6 support
  if (sscanf(hostport.c_str(), "%hu", &port) != 1) {
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
  listeners_.push_back(new hq_listener(role, fd));
  
  return 0;
}

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
  while (hq_listener::num_listeners() != 0
	 || *cac_mutex_t<size_t>::lockref(hq_client::cnt_) != 0) {
    {
      hq_listener::poll_guard pg;
      picoev_loop_once(*loop_, 1);
      hq_worker::trash_idle_connections();
    }
    if (stop_) {
      while (! stoppables_->empty()) {
	(*stoppables_->begin())->from_loop_request_stop();
      }
    }
  }
}

hq_tls<picoev_loop*> hq_loop::loop_;
hq_tls<set<hq_loop::stoppable*> > hq_loop::stoppables_;
volatile bool hq_loop::stop_;

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

inline int lc(int c)
{
  return 'A' <= c && c <= 'Z' ? c + 0x20 : c;
}

hq_util::lcstr::lcstr(const string& s)
{
  for (string::const_iterator i = s.begin(); i != s.end(); ++i) {
    this->s.push_back(lc(*i));
  }
}

bool hq_util::lceq(const char* x, const char* y)
{
  for (; *x != '\0'; x++, y++)
    if (lc(*x) != lc(*y))
      return false;
  return *y == '\0';
}

bool hq_util::lceq(const string& x, const string& y)
{
  return x.size() == y.size() && lceq(x.c_str(), y.c_str());
}

bool hq_util::lceq(const char* x, const lcstr& y)
{
  for (string::const_iterator yi = y->begin(); yi != y->end(); ++x, ++yi)
    if (lc(*x) != *yi)
      return false;
  return *x == '\0';
}

bool hq_util::lceq(const string& x, const lcstr& y)
{
  return x.size() == y->size() && lceq(x.c_str(), y);
}

bool hq_util::lceq(const char* x, size_t xlen, const lcstr& y)
{
  if (xlen != y->size())
    return false;
  for (string::const_iterator yi = y->begin(); yi != y->end(); ++yi)
    if (lc(*x++) != *yi)
      return false;
  return true;
}

hq_headers::const_iterator hq_util::find_header(const hq_headers& headers,
						const lcstr& name)
{
  // bind2nd cannot take functions that accept constrefs as args...
  for (hq_headers::const_iterator i = headers.begin();
       i != headers.end();
       ++i) {
    if (lceq(i->first, name)) {
      return i;
    }
  }
  return headers.end();
}

int64_t hq_util::parse_positive_number(const char* str, size_t len)
{
  int64_t r = 0;
  
  for (const char* p = str, * pMax = str + len; p != pMax; ++p) {
    if ('0' <= *p && *p <= '9') {
      r = r * 10 + *p - '0';
    } else {
      return -1;
    }
  }
  return r;
}

struct hq_help : public picoopt::config_base<hq_help> {
  hq_help()
    : picoopt::config_base<hq_help>("help", no_argument, "")
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

static int num_threads = 1;

struct hq_num_threads : public picoopt::config_base<hq_num_threads> {
  hq_num_threads()
    : picoopt::config_base<hq_num_threads>("num-threads", required_argument,
					   "=num_threads")
    {}
  virtual int setup(const string* num, string& err) {
    if (sscanf(num->c_str(), "%d", &num_threads) != 1 || num_threads < 1) {
      err = "invalid argument";
      return 1;
    }
    return 0;
  }
};

static void* loop_main(void*)
{
  hq_loop loop;
  loop.run_loop();
  return NULL;
}

static void on_signal(int sig)
{
  switch (sig) {
  case SIGTERM:
    picolog::info() << "received SIGTERM, quitting";
    hq_loop::request_stop();
    break;
  default:
    assert(0);
  }
}

int main(int argc, char** argv)
{
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, on_signal);
  
  picoev_init(MAX_FDS);
  
  int r;
  if ((r = picoopt::parse_args(argc, argv)) != 0) {
    exit(r);
  }
  argc -= optind;
  argv += optind;
  
  hq_worker::setup();
  
  if (num_threads == 1) {
    loop_main(NULL);
  } else {
    vector<pthread_t> threads;
    // start loop(s)
    for (int i = 0; i < num_threads; ++i) {
      threads.push_back(0);
      int err = pthread_create(&threads.back(), NULL, loop_main, NULL);
      if (err != 0) {
	perror("pthread_create failed");
	exit(2);
      }
    }
    // wait until all threads exit
    for (vector<pthread_t>::iterator i = threads.begin();
	 i != threads.end();
	 ++i) {
      pthread_join(*i, NULL);
    }
  }
  
  return 0;
}
