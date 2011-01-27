#ifndef HQ_HH
#define HQ_HH

// system headers
extern "C" {
#include <assert.h>
}
#include <list>
#include <string>
#include <vector>

// non-system headers
extern "C" {
#include "picoev/picoev.h"
}
#include "cac/cac_mutex.h"

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
  const T& operator->() const { return *_get(); }
  T& operator->() { return *_get(); }
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
 * reads a HTTP request
 */
class hq_req_reader {
protected:
  hq_buffer buf_;
  std::string method_;
  std::string path_;
  hq_headers headers_;
  hq_buffer* content_;
  size_t content_length_;
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
   * reads a HTTP request
   * @return false on error otherwise true (caller should check if the request has been completely received by calling is_complete())
   */
  bool read_request(int fd);
  /**
   * returns whether or not the entire request has been received
   */
  bool is_complete() const { return state_ == READ_COMPLETE; }
  const std::string& method() const { return method_; }
  const std::string& path() const { return path_; }
  const hq_headers& headers() const { return headers_; }
  const hq_buffer* content() const { return content_; }
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
   * @return whether or not the request was dispatched
   */
  virtual bool dispatch(const hq_req_reader& req, hq_res_sender* res_sender) = 0;
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
class hq_client : public hq_res_sender {
protected:
  int fd_;
  hq_req_reader req_;
  struct {
    hq_buffer sendbuf;
    bool closed_by_sender;
    struct {
      int fd;
      off_t pos, size;
    } sendfile;
  } res_;
public:
  /**
   * constructor
   */
  hq_client(int fd);
  /**
   * destructor
   */
  virtual ~hq_client();
protected:
  void _start();
  void _read_request(int fd, int revents);
  virtual void send_file_response(int status, const std::string& msg, const hq_headers& headers, int fd);
  virtual bool open_response(int status, const std::string& msg, const hq_headers& headers, const char* data, size_t len);
  virtual bool send_response(const char* data, size_t len);
  virtual void close_response();
  void _prepare_response(int status, const std::string& msg, const hq_headers& hedaers, const int sendfile_fd);
  void _finalize_response();
  void _write_sendfile_cb(int fd, int revents);
  void _write_sendbuf_cb(int fd, int revents);
  bool _write_sendbuf(bool disactivate_poll_when_empty);
};

/**
 * Worker
 */
class hq_worker {
public:
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
      req_queue_entry(const hq_req_reader* r, hq_res_sender* rs)
	: req(r), res_sender(rs) {}
    };
  protected:
    /**
     * list of unassociated requests and idle workers protected by a mutex
     */
    struct junction {
      std::list<req_queue_entry> reqs;
      std::list<hq_worker*> workers;
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
  protected:
    /**
     * starts a worker if any pending request exists, or registers the worker to wait for requests
     */
    void _start_worker_or_register(hq_worker* worker);
  };
protected:
  int fd_;
  const hq_req_reader* req_;
  hq_res_sender* res_sender_;
  hq_buffer buf_; // used for both send and recv
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
  void _prepare_next();
  void _start(const hq_req_reader* req, hq_res_sender* res_sender);
  void _send_request_cb(int fd, int revents);
  void _send_request();
  void _read_response_header(int fd, int revents);
  void _read_response_body(int fd, int revents);
  void _return_error(int status, const std::string& msg);
public:
  static handler handler_;
};
  
class hq_loop {
protected:
  int listen_fd_;
public:
  /**
   * constructor
   */
  hq_loop(int listen_fd);
  /**
   * desctructor
   */
  ~hq_loop();
  /**
   * main loop
   */
  void run_loop();
protected:
  void accept_conn(int fd, int revents);
private:
  hq_loop(const hq_loop&); // not used
  hq_loop& operator=(const hq_loop&); // not used
protected:
  static hq_tls<picoev_loop*> loop_;
public:
  static picoev_loop* get_loop();
};

class hq_util {
public:
  static hq_headers::const_iterator find_header(const hq_headers& hdrs, const std::string& name);
};

#endif
