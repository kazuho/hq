#ifndef HQ_HH
#define HQ_HH

// system headers
extern "C" {
#include <assert.h>
}
#include <cstdio>
#include <list>
#include <set>
#include <string>
#include <vector>

// non-system headers
extern "C" {
#include "picoev/picoev.h"
}
#include "cac/cac_mutex.h"
#include "picoopt/picoopt.h"

/**
 * utility class for running a thread
 */
template<typename T> class hq_runnable {
protected:
  T* obj_;
  void* (T::*func_)();
  pthread_t* tid_;
  bool stop_requested_;
public:
  hq_runnable(T* obj, void* (T::*func)()) : obj_(obj), func_(func), tid_(NULL), stop_requested_(false) {}
  ~hq_runnable() {
    if (tid_ != NULL) {
      request_stop();
      join(NULL);
    }
  }
  void start(const pthread_attr_t* attr = NULL) {
    assert(tid_ == NULL);
    tid_ = new pthread_t;
    pthread_create(tid_, attr, _start, this);
  }
  bool stop_requested() const { return stop_requested_; }
  void request_stop() { stop_requested_ = true; }
  void join(void** value_ptr) {
    assert(tid_ != NULL);
    pthread_join(*tid_, value_ptr);
    delete tid_;
    tid_ = NULL;
  }
private:
  hq_runnable(const hq_runnable&); // not defined
  hq_runnable& operator=(const hq_runnable&); // not defined
private:
  static void* _start(void* _self) {
    hq_runnable* self = (hq_runnable*)_self;
    return (self->obj_->*self->func_)();
  }
};

/**
 * utility class for tls
 */
template <typename T> class hq_tls {
protected:
  pthread_key_t key_;
public:
  hq_tls() {
    pthread_key_create(&key_, _dtor);
  }
  ~hq_tls() {
    pthread_key_delete(key_);
  }
  const T& operator*() const { return *_get(); }
  T& operator*() { return *_get(); }
  const T* operator->() const { return _get(); }
  T* operator->() { return _get(); }
protected:
  T* _get() {
    T* t = reinterpret_cast<T*>(pthread_getspecific(key_));
    if (t == NULL) {
      t = new T();
      pthread_setspecific(key_, t);
    }
    return t;
  }
  static void _dtor(void* p) {
    if (p != NULL) {
      delete reinterpret_cast<T*>(p);
    }
  }
};

/**
 * RAII for locking/unlocking a mutex
 */
class mutex_guard {
protected:
  pthread_mutex_t* m_;
public:
  mutex_guard(pthread_mutex_t* m) : m_(m) { pthread_mutex_lock(m); }
  ~mutex_guard() { pthread_mutex_unlock(m_); }
};

/**
 * utility class for type-safe callback generation
 */
template <typename T, void (T::*FUNC)(int fd, int revents)>
void hq_picoev_cb(picoev_loop*, int fd, int revents, void* cb_arg) {
  T* obj = reinterpret_cast<T*>(cb_arg);
  (obj->*FUNC)(fd, revents);
}

/**
 * list of headers
 */
typedef std::list<std::pair<std::string, std::string> > hq_headers;

/**
 * buffer
 */
class hq_buffer {
protected:
  std::vector<char> buf_;
  size_t size_;
public:
  /**
   * constructor
   */
  hq_buffer() : buf_(), size_(0) {}
  /**
   * returns pointer to buffered data
   */
  const char* buffer() const { return &*buf_.begin(); }
  /**
   * returns pointer to buffer
   */
  char* buffer() { return &*buf_.begin(); }
  /**
   * whether or not empty
   */
  bool empty() const { return size_ == 0; }
  /**
   * returns size of the data stored in the buffer
   */
  size_t size() const { return size_; }
  /**
   * clears the buffer
   */
  void clear() {
    buf_.clear();
    size_ = 0;
  }
  /**
   * push data onto the buffer
   */
  void push(const char byte) {
    assert(size_ <= buf_.size());
    buf_.erase(buf_.begin() + size_, buf_.end());
    buf_.push_back(byte);
    size_++;
  }
  /**
   * push data onto the buffer
   */
  void push(const char* data, size_t len) {
    assert(size_ <= buf_.size());
    buf_.erase(buf_.begin() + size_, buf_.end());
    buf_.insert(buf_.end(), data, data + len);
    size_ += len;
  }
  /**
   * push data onto the buffer
   */
  void push(const std::string& s) {
    assert(size_ <= buf_.size());
    buf_.erase(buf_.begin() + size_, buf_.end());
    buf_.insert(buf_.end(), &*s.begin(), &*s.end());
    size_ += s.size();
  }
  /**
   * prepares space for additional sz bytes in the buffer and returns pointer to the space
   */
  char* prepare(size_t sz) {
    assert(size_ <= buf_.size());
    if (size_ + sz > buf_.size()) {
      buf_.insert(buf_.end(), size_ + sz - buf_.size(), '\0');
    }
    return &*buf_.begin() + size_;
  }
  /**
   * adjusts size by given difference
   */
  void adjust_size(ptrdiff_t diff) { size_ += diff; }
  /**
   * pops first given bytes from the buffer
   */
  void advance(size_t diff) {
    assert(diff <= size_);
    buf_.erase(buf_.begin(), buf_.begin() + diff);
    size_ -= diff;
  }
private:
  hq_buffer(const hq_buffer&); // not used
  hq_buffer& operator=(const hq_buffer&); // not used
};

/**
 * the event loop
 */
class hq_loop {
public:
  /**
   * objects that can unregister itself from the event loop should implement
   * this interface
   */
  class stoppable {
    friend class hq_loop;
  protected:
    ~stoppable() {}
    /**
     * called when the loop is requested to exit.  The called object should
     * unregister itself by calling unregister_stoppable and perform other
     * cleanup tasks.
     */
    virtual void from_loop_request_stop() = 0;
  };
public:
  /**
   * constructor
   */
  hq_loop();
  /**
   * desctructor
   */
  ~hq_loop();
  /**
   * main loop
   */
  void run_loop();
private:
  hq_loop(const hq_loop&); // not used
  hq_loop& operator=(const hq_loop&); // not used
protected:
  static hq_tls<picoev_loop*> loop_;
  static hq_tls<std::set<stoppable*> > stoppables_;
  static volatile bool stop_;
public:
  /**
   * returns the picoev_loop of the current thread
   */
  static picoev_loop* get_loop() { return *loop_; }
  /**
   * returns if stop is requested
   */
  static bool stop_requested() { return stop_; }
  /**
   * requests all of the loops to stop
   */
  static void request_stop() { stop_ = true; }
  /**
   * registers a stoppable object to the loop
   */
  static void register_stoppable(stoppable* s) { stoppables_->insert(s); }
  /**
   * unregisters a stoppable object from the loop
   */
  static void unregister_stoppable(stoppable* s) { stoppables_->erase(s); }
};

/**
 * reads a HTTP request
 */
class hq_req_reader {
protected:
  hq_buffer buf_;
  std::string method_;
  std::string path_;
  int minor_version_;
  hq_headers headers_;
  hq_buffer* content_;
  int64_t content_length_;
  enum {
    READ_REQUEST,
    READ_CONTENT,
    READ_COMPLETE
  } state_;
public:
  /**
   * constructor
   */
  hq_req_reader();
  /**
   * destructor
   */
  ~hq_req_reader();
  /**
   * clears the reader
   */
  void reset();
  /**
   * reads a HTTP request
   * @return false on error otherwise true (caller should check if the request has been completely received by calling is_complete())
   */
  bool read_request(int fd);
  /**
   * returns whether or not the entire request has been received
   */
  bool is_complete() const { return state_ == READ_COMPLETE; }
  /**
   * returns true if not a single byte has been read by the parser
   */
  bool empty() const { return state_ == READ_REQUEST && buf_.empty(); }
  /**
   * returns the method
   */
  const std::string& method() const { return method_; }
  /**
   * returns path
   */
  const std::string& path() const { return path_; }
  /**
   * returns minor version of the HTTP protocl being used
   */
  int minor_version() const { return minor_version_; }
  /**
   * returns the request headers
   */
  const hq_headers& headers() const { return headers_; }
  /**
   * returns the request content (or null if none)
   */
  const hq_buffer* content() const { return content_; }
  /**
   * returns length of the request content
   */
  size_t content_length() const { return content_length_; }
protected:
  bool _read_request(int fd);
  bool _read_content(int fd);
private:
  hq_req_reader(const hq_req_reader&); // not used
  hq_req_reader& operator=(const hq_req_reader&); // not used
public:
  static size_t max_request_length_;
};

/**
 * interface for sending responses
 */
class hq_res_sender {
public:
  /**
   * sends a file
   */
  virtual void send_file_response(int status, const std::string& msg, const hq_headers& headers, int fd) = 0;
  /**
   * starts a response
   */
  virtual bool open_response(int status, const std::string& msg, const hq_headers& headers, const char* data, size_t len) = 0;
  /**
   * pushes (portion of a) content
   */
  virtual bool send_response(const char* data, size_t len) = 0;
  /**
   * closes the response
   */
  virtual void close_response() = 0;
protected:
  /**
   * destructor, only called from the dtor of derived classes
   */
  ~hq_res_sender() {}
};

/**
 * interface for handling requests (handlers should inherit this interface)
 */
class hq_handler {
public:
  /**
   * destructor
   */
  virtual ~hq_handler() {}
  /**
   * dispatches a request
   */
  virtual bool dispatch(const hq_req_reader& req, hq_res_sender* res_sender) = 0;
public:
  static std::list<hq_handler*> handlers_;
public:
  /**
   * dispatches a request
   */
  static void dispatch_request(const hq_req_reader& req, hq_res_sender* res_sender);
  /**
   * sends an error response
   */
  static void send_error(const hq_req_reader& req, hq_res_sender* res_sender, int status, const std::string& msg);
};

/**
 * HTTP client
 */
class hq_client : public hq_res_sender, public hq_loop::stoppable {
public:
  struct io_timeout_config : public picoopt::config_base<io_timeout_config> {
    io_timeout_config();
    virtual int setup(const std::string* secs, std::string& err);
  };
  enum role {
    ROLE_ANY,
    ROLE_CLIENT,
    ROLE_WORKER
  };
  enum response_mode {
    RESPONSE_MODE_HTTP10, /* not chunked */
    RESPONSE_MODE_CHUNKED,
    RESPONSE_MODE_SENDFILE
  };
protected:
  role role_;
  int fd_;
  hq_req_reader req_;
  struct {
    int status;
    hq_buffer sendbuf;
    bool closed_by_sender;
    response_mode mode;
    union {
      struct {
	int64_t off;
	int64_t content_length; // -1 if none
      } http10;
      struct {
	int fd;
	off_t pos, size;
      } sendfile;
    } u;
  } res_;
  bool keep_alive_;
public:
  /**
   * constructor
   */
  hq_client(role r, int fd);
  /**
   * destructor
   */
  virtual ~hq_client();
protected:
  void _reset(bool is_keep_alive);
  void _read_request(int fd, int revents);
  virtual void send_file_response(int status, const std::string& msg, const hq_headers& headers, int fd);
  virtual bool open_response(int status, const std::string& msg, const hq_headers& headers, const char* data, size_t len);
  virtual bool send_response(const char* data, size_t len);
  virtual void close_response();
  void _prepare_response(int status, const std::string& msg, const hq_headers& hedaers, const int sendfile_fd);
  void _finalize_response(bool success);
  void _write_sendfile_cb(int fd, int revents);
  void _write_sendbuf_cb(int fd, int revents);
  bool _write_sendbuf(bool disactivate_poll_when_empty);
  void _push_http10_data(const char* data, size_t len);
  void _push_chunked_data(const char* data, size_t len);
  virtual void from_loop_request_stop();
private:
  hq_client(const hq_client&); // not defined
  hq_client& operator=(const hq_client&); // not defined
public:
  static cac_mutex_t<size_t> cnt_;
protected:
  static int timeout_;
  static int keepalive_timeout_;
public:
  static int timeout() { return timeout_; }
};

/**
 * Worker
 */
class hq_worker {
public:
  /**
   * timeout
   */
  struct queue_timeout_config : public picoopt::config_base<queue_timeout_config> {
    queue_timeout_config();
    virtual int setup(const std::string* seconds, std::string& err);
  };
  /**
   * handler
   */
  class handler : public hq_handler {
    friend class hq_worker;
    /**
     * represents a request unassociated to a worker
     */
    struct req_queue_entry {
      const hq_req_reader* req;
      hq_res_sender* res_sender;
      time_t at;
      req_queue_entry(const hq_req_reader* r, hq_res_sender* rs);
    };
  protected:
    /**
     * list of unassociated requests and idle workers protected by a mutex
     */
    struct junction {
      std::list<req_queue_entry> reqs;
      std::list<std::pair<hq_worker*, time_t> > workers;
    };
    cac_mutex_t<junction> junction_;
  public:
    /**
     * constructor
     */
    handler();
    /**
     * destructor
     */
    virtual ~handler();
    /**
     * dispatches a request
     */
    virtual bool dispatch(const hq_req_reader& req, hq_res_sender* res_sender);
    void trash_idle_connections();
  protected:
    /**
     * starts a worker if any pending request exists, or registers the worker to wait for requests
     */
    void _start_worker_or_register(hq_worker* worker);
  };
  enum response_mode {
    RESPONSE_MODE_HTTP10, // content-length or non-persistent
    RESPONSE_MODE_CHUNKED
  };
protected:
  int fd_;
  const hq_req_reader* req_;
  hq_res_sender* res_sender_;
  hq_buffer buf_; // used for both send and recv
  bool keep_alive_;
  struct {
    response_mode mode;
    union {
      struct {
	int64_t off;
	int64_t content_length; // -1 if no content-length
      } http10;
      struct {
	int64_t chunk_off;  // becomes chunk_off == chunk_size while waiting for trailing CRLFs
	int64_t chunk_size; // -1 if not initialized
      } chunked;
    } u;
  } res_;
public:
  /**
   * constructor
   */
  hq_worker(int fd, const hq_req_reader& req_params);
  /**
   * destructor
   */
  ~hq_worker();
protected:
  void _send_upgrade(int fd, int revents);
  void _prepare_next();
  void _start(const hq_req_reader* req, hq_res_sender* res_sender);
  void _send_request_cb(int fd, int revents);
  void _send_request();
  void _read_response_header(int fd, int revents);
  void _read_response_http10(int fd, int revents);
  void _read_response_chunked(int fd, int revents);
  void _return_error(int status, const std::string& msg);
  bool _send_buffer();
  bool _recv_buffer(bool* closed = NULL);
  void _push_chunked_data();
private:
  hq_worker(const hq_worker&); // not defined
  hq_worker& operator=(const hq_worker&); // not defined
public:
  static cac_mutex_t<size_t> cnt_;
protected:
  static handler* handler_;
  static int queue_timeout_;
public:
  static void setup();
  static void trash_idle_connections() { handler_->trash_idle_connections(); }
};

class hq_listener {
public:
  struct port_config : public picoopt::config_base<port_config> {
    port_config();
    virtual int setup(const std::string* hostport, std::string& err);
    virtual int post_setup(std::string& err);
  };
#if 0
  struct client_port_config : public picoopt::config_base<client_port_config> {
    client_port_config();
    virtual int setup(const std::string* hostport, std::string& err);
  };
  struct worker_port_config : public picoopt::config_base<worker_port_config> {
    worker_port_config();
    virtual int setup(const std::string* hostport, std::string& err);
  };
#endif
  struct max_connections_config
    : public picoopt::config_base<max_connections_config> {
    max_connections_config();
    virtual int setup(const std::string* maxconn, std::string& err);
  };
  class poll_guard {
  protected:
    bool locked_;
  public:
    poll_guard();
    ~poll_guard();
  };
  friend class poll_guard;
protected:
  hq_client::role role_;
  int listen_fd_;
protected:
  void _accept(int fd, int revents);
private:
  hq_listener(const hq_client::role& role, int listen_fd) : role_(role), listen_fd_(listen_fd) {}
  hq_listener(const hq_listener&); // not defined
  ~hq_listener();
  hq_listener& operator=(const hq_listener&); // not defined
protected:
  static std::list<hq_listener*> listeners_;
  static pthread_mutex_t listeners_mutex_;
  static int max_connections_;
public:
  /**
   * returns number of the listeners
   */
  static size_t num_listeners() { return listeners_.size(); }
private:
  static int _create_listener(const hq_client::role& role, const std::string& hostport, std::string& err);
};

class hq_static_handler : public hq_handler {
public:
  class config : public picoopt::config_base<config> {
  public:
    config();
    virtual int setup(const std::string* mapping, std::string& err);
  };
protected:
  std::string vpath_; // with trailing slash
  std::string dir_; // with trailing slash
public:
  /**
   * destructor
   */
  ~hq_static_handler() {}
  /**
   * dispatches a request
   */
  virtual bool dispatch(const hq_req_reader& req, hq_res_sender* res_sender);
protected:
  hq_static_handler(const std::string& vpath, const std::string& dir)
    : vpath_(vpath), dir_(dir) {}
private:
  hq_static_handler(const hq_static_handler&); // not defined
  hq_static_handler& operator=(const hq_static_handler&); // not defined
};

class hq_log_access {
public:
  struct config : public picoopt::config_base<config> {
    config()
      : picoopt::config_base<config>("log-file", required_argument, "=file")
    {}
    virtual int setup(const std::string* filename, std::string& err);
  };
protected:
  FILE* fp_;
public:
  /**
   * destructor
   */
  ~hq_log_access();
  /**
   * logs an HTTP access
   */
  void log(int fd, const std::string& method, const std::string& path, int minor_version, int status);
protected:
  hq_log_access(FILE* fp) : fp_(fp) {}
public:
  static hq_log_access* log_;
};

class hq_util {
public:
  /**
   * return mime type assigned to the given extension
   */
  static std::string get_mime_type(const std::string& ext);
  /**
   * returns the extension portion of given path (excluding the ".") or an
   * empty string if not any
   */
  static std::string get_ext(const std::string& path);
  /**
   * returns the peer IP address as string
   */
  static std::string gethostof(int fd);
  /**
   * wrapper of strerror_r(3)
   */
  static std::string strerror(int err);
  /**
   * lower-case string class used for fast comparison
   */
  struct lcstr {
    std::string s;
    explicit lcstr(const std::string& s);
    const std::string* operator*() const { return &s; }
    const std::string* operator->() const { return &s; }
  };
  /**
   * checks if two strings are identical in lower-case (US-ascii only)
   */
  static bool lceq(const char* x, const char* y);
  /**
   * checks if two strings are identical in lower-case (US-ascii only)
   */
  static bool lceq(const std::string& x, const std::string& y);
  /**
   * checks if two strings are identical in lower-case (US-ascii only)
   */
  static bool lceq(const char* x, const lcstr& y);
  /**
   * checks if two strings are identical in lower-case (US-ascii only)
   */
  static bool lceq(const std::string& x, const lcstr& y);
  /**
   * checks if two strings are identical in lower-case (US-ascii only)
   */
  static bool lceq(const char* x, size_t xlen, const lcstr& y);
  /**
   * finds a header with given name from the header list
   */
  static hq_headers::const_iterator find_header(const hq_headers& headers, const lcstr& name);
  /**
   * parses a decimal number or returns -1 on error
   */
  static int64_t parse_positive_number(const char* str, size_t len);
};

#endif
