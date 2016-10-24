#ifndef LIBZLOG_INTERNAL_HPP
#define LIBZLOG_INTERNAL_HPP
#include <condition_variable>
#include <mutex>
#include "include/zlog/log.h"
#include "libseq/libseqr.h"
#include "log_mapper.h"
#include "include/zlog/backend.h"

namespace zlog {

class LogImpl : public Log {
 public:
  LogImpl() {}
#if BACKEND_SUPPORT_DISABLED
  LogImpl() : backend(NULL), backend_ver(2), new_stripe_pending_(false) {}
#endif

  /*
   * Create cut.
   */
  int CreateCut(uint64_t *pepoch, uint64_t *maxpos);

  /*
   * Set log stripe width
   */
  int SetStripeWidth(int width);

  /*
   * (v2 only): adds a new empty stripe
   */
  int CreateNewStripe(uint64_t last_epoch);

  /*
   * Find and optionally increment the current tail position.
   */
  int CheckTail(uint64_t *pposition, bool increment);
  int CheckTail(uint64_t *pposition);

  /*
   * Return a batch of positions.
   */
  int CheckTail(std::vector<uint64_t>& positions, size_t count);

  /*
   * Append data to the log and return its position.
   */
  int Append(const Slice& data, uint64_t *pposition = NULL);

  int OpenStream(uint64_t stream_id, zlog::Stream **streamptr);

  /*
   * Append data to multiple streams and return its position.
   */
  int MultiAppend(const Slice& data,
      const std::set<uint64_t>& stream_ids, uint64_t *pposition = NULL);

  /*
   * Append data asynchronously to the log and return its position.
   */
  int AioAppend(zlog::AioCompletion *c, const Slice& data,
      uint64_t *pposition = NULL);

  /*
   * Read data asynchronously from the log.
   */
  int AioRead(uint64_t position, zlog::AioCompletion *c,
      std::string *datap);

  /*
   * Mark a position as unused.
   */
  int Fill(uint64_t position);

  /*
   *
   */
  int Read(uint64_t position, std::string *data);

  /*
   *
   */
  int Trim(uint64_t position);

  /*
   * Return the stream membership for a log entry position.
   */
  int StreamMembership(std::set<uint64_t>& stream_ids, uint64_t position);
  int StreamMembership(uint64_t epoch, std::set<uint64_t>& stream_ids, uint64_t position);
  int Fill(uint64_t epoch, uint64_t position);

  // Seal an epoch across a set of objects and return the next position.
  int Seal(const std::vector<std::string>& objects,
      uint64_t epoch, uint64_t *next_pos);

  static std::string metalog_oid_from_name(const std::string& name);

  LogImpl(const LogImpl& rhs);
  LogImpl& operator=(const LogImpl& rhs);

  int RefreshProjection();

  int Read(uint64_t epoch, uint64_t position, std::string *data);

  int StreamHeader(const std::string& data, std::set<uint64_t>& stream_ids,
      size_t *header_size = NULL);

  /*
   * When next == true
   *   - position: new log tail
   *   - stream_backpointers: back pointers for each stream in stream_ids
   *
   * When next == false
   *   - position: current log tail
   *   - stream_backpointers: back pointers for each stream in stream_ids
   */
  int CheckTail(const std::set<uint64_t>& stream_ids,
      std::map<uint64_t, std::vector<uint64_t>>& stream_backpointers,
      uint64_t *position, bool next);

  int StripeWidth();

  std::string pool2_;
  std::string name_;
  std::string metalog_oid_;
  SeqrClient *seqr;

  Backend *new_backend;

#if BACKEND_SUPPORT_DISABLE
  TmpBackend *backend;
  int backend_ver;

  void set_backend_v2() {
    assert(backend);
    delete backend;
    backend = TmpBackend::CreateV2();
    backend_ver = 2;
  }

  bool new_stripe_pending_;
#endif

  LogMapper mapper_;

  std::condition_variable new_stripe_cond_;
  std::mutex lock_;
};

}

#endif
