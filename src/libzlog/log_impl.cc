#include "log_impl.h"

#include <cerrno>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "proto/zlog.pb.h"
#include "include/zlog/log.h"
#include "include/zlog/backend.h"

#include "stripe_history.h"

/*
 * We can use Ceph API to query and make some intelligent decisiosn about what
 * the stripe size should be at runtime. In any case logs are not created
 * programmatically where this is needed. They are created for instance with a
 * CLI tool, and in any case the width can be changed online.
 */
#define DEFAULT_STRIPE_SIZE 100

namespace zlog {

Log::~Log() {}

std::string LogImpl::metalog_oid_from_name(const std::string& name)
{
  std::stringstream ss;
  ss << name << ".meta";
  return ss.str();
}

int Log::CreateWithStripeWidth(Backend *backend, const std::string& name,
    SeqrClient *seqr, int stripe_size, Log **logptr)
{
  Log *logTable=NULL;
  int ret=0;
  if(name!="logTable"){
    ret = Open(backend, "logTable", seqr, &logTable);
    if(ret != Backend::ZLOG_OK){
      ret = Create(backend, "logTable", seqr, &logTable);
      if(ret != Backend::ZLOG_OK){
        std::cerr<<"Log Table now found"<<std::endl;
      }
    }
  }

  if (stripe_size <= 0) {
    std::cerr << "Invalid stripe size (" << stripe_size << " <= 0)" << std::endl;
    return -EINVAL;
  }

  if (name.length() == 0) {
    std::cerr << "Invalid log name (empty string)" << std::endl;
    return -EINVAL;
  }

  std::string new_name = name;


  //find version for log
  if(logTable!=NULL && seqr!=NULL){
    uint64_t logTableTail;
    ret = logTable->CheckTail(&logTableTail);
    uint64_t i=0;
    uint64_t version = 0;
    for(i = 0;i < logTableTail; i++){
      std::string entry;
      ret=logTable->Read(i, &entry);
      if(entry.compare(0, name.length()+1, name + '.')){
        version = stoi(entry.substr(name.length()+1));
      }
    }
    new_name = name + '.'+std::to_string(version);
    ret = backend->Exists(LogImpl::metalog_oid_from_name(new_name));
    if(ret)
      version++;
    else{
      std::cerr << "Failed to create log " << name << ". Already Exists." << std::endl;
    }
    new_name = name + '.'+std::to_string(version);
    ret = logTable->Append(Slice(new_name), &logTableTail);
  }

  // Setup the first projection
  StripeHistory hist;
  hist.AddStripe(0, 0, stripe_size);
  const auto hist_data = hist.Serialize();

  // create the log metadata/head object
  std::string metalog_oid = LogImpl::metalog_oid_from_name(new_name);
  ret = backend->CreateHeadObject(metalog_oid, hist_data);
  if (ret != Backend::ZLOG_OK) {
    std::cerr << "Failed to create log " << name << " ret "
      << ret << " (" << strerror(-ret) << ")" << std::endl;
    return ret;
  }

  LogImpl *impl = new LogImpl;

  impl->new_backend = backend;
  impl->name_ = new_name;
  impl->metalog_oid_ = metalog_oid;
  impl->seqr = seqr;
  impl->mapper_.SetName(new_name);

  ret = impl->RefreshProjection();
  if (ret) {
    delete impl;
    return ret;
  }

  *logptr = impl;

  return 0;
}

int Log::Create(Backend *backend, const std::string& name,
    SeqrClient *seqr, Log **logptr)
{
  return CreateWithStripeWidth(backend, name, seqr, DEFAULT_STRIPE_SIZE, logptr);
}

int Log::Open(Backend *backend, const std::string& name,
    SeqrClient *seqr, Log **logptr)
{
  if (name.length() == 0) {
    std::cerr << "Invalid log name (empty string)" << std::endl;
    return -EINVAL;
  }
  std::string new_name = name;

  Log *logTable=NULL;
  int ret=0;
  if(name!="logTable"){
    ret = Open(backend, "logTable", seqr, &logTable);
    if(ret != Backend::ZLOG_OK){
      ret = Create(backend, "logTable", seqr, &logTable);
      if(ret != Backend::ZLOG_OK){
        std::cerr<<"Log Table now found"<<std::endl;
      }
    }
  }

  //find version for log
  if(logTable!=NULL && seqr!=NULL){
    uint64_t logTableTail;
    ret = logTable->CheckTail(&logTableTail);
    uint64_t i=0;
    uint64_t version = 0;
    for(i = 0;i < logTableTail; i++){
      std::string entry;
      ret=logTable->Read(i, &entry);
      if(entry.compare(0, name.length()+1, name + '.')){
        version = stoi(entry.substr(name.length()+1));
      }
    }
    new_name = name + '.'+std::to_string(version);
  }

  /*
   * Check that the log metadata/head object exists. The projection and other
   * state is read during RefreshProjection.
   */
  std::string metalog_oid = LogImpl::metalog_oid_from_name(new_name);
  ret = backend->Exists(metalog_oid);
  if (ret) {
    std::cerr << "Failed to open log meta object " << metalog_oid << " ret " <<
      ret << std::endl;
    return ret;
  }

  LogImpl *impl = new LogImpl;

  impl->new_backend = backend;
  impl->name_ = new_name;
  impl->metalog_oid_ = metalog_oid;
  impl->seqr = seqr;
  impl->mapper_.SetName(new_name);

  ret = impl->RefreshProjection();
  if (ret) {
    delete impl;
    return ret;
  }

  *logptr = impl;

  return 0;
}

int LogImpl::StripeWidth()
{
  return mapper_.CurrentStripeWidth();
}

class new_stripe_notifier {
 public:
  new_stripe_notifier(std::mutex *lock, std::condition_variable *cond, bool *pred) :
    lock_(lock), cond_(cond), pred_(pred)
  {}

  ~new_stripe_notifier() {
    std::unique_lock<std::mutex> l(*lock_);
    *pred_ = false;
    cond_->notify_all();
  }

 private:
  std::mutex *lock_;
  std::condition_variable *cond_;
  bool *pred_;
};

#ifdef BACKEND_SUPPORT_DISABLED
/*
 * Create a new stripe empty stripe.
 *
 * TODO:
 *   CreateNewStripe, CreateCut, SetStripeWidth are all nearly identical
 *   methods that could be unified.
 */
int LogImpl::CreateNewStripe(uint64_t last_epoch)
{
  assert(backend_ver == 2);

  /*
   * If creation of a new stripe is pending then we will block on the
   * condition variable until the creating thread has completed or there was
   * an error.
   *
   * There are two ways that we rate limit new stripe creation. The first is
   * blocking if a new stripe is pending, and the second is that we retry if
   * once we start we notice that a new stripe was created since we began. The
   * second case is important for cases in which a bunch of contexts are
   * trying to create a new stripe but they never actually interact through
   * the pending flag.
   */
#if 1
  std::unique_lock<std::mutex> l(lock_);

  if (new_stripe_pending_) {
    //std::cout << "new stripe pending. waiting..." << std::endl;
    bool done = new_stripe_cond_.wait_for(l, std::chrono::seconds(1),
        [this](){ return !new_stripe_pending_; });
    if (!done)
      std::cerr << "waiting on new stripe: timeout!" << std::endl;
    return 0;
  }

  new_stripe_pending_ = true;
  l.unlock();

  new_stripe_notifier completed(&lock_,
      &new_stripe_cond_, &new_stripe_pending_);

  if (mapper_.Epoch() > last_epoch) {
    //std::cout << "    update occurred... trying again" << std::endl;
    return 0;
  }
#endif

  /*
   * Get the current projection. We'll add the new striping width when we
   * propose the next projection/epoch.
   */
  uint64_t epoch;
  zlog_proto::MetaLog config;
  int ret = new_backend->LatestProjection(metalog_oid_, &epoch, config);
  if (ret != Backend::ZLOG_OK) {
    std::cerr << "failed to get projection ret " << ret << std::endl;
    return ret;
  }

  /*
   * Each call to CreateNewStripe is followed by a projection refresh. If this
   * case holds then the projection will get an update relative to the last
   * update that triggered the stripe creation so we can return in this case
   * too!
   */
  if (epoch > last_epoch) {
    //std::cout << "    update occurred... trying again" << std::endl;
    return 0;
  }

  //std::cout << "creating new stripe!" << std::endl;

  StripeHistory hist;
  ret = hist.Deserialize(config);
  if (ret)
    return ret;
  assert(!hist.Empty());

  std::vector<std::string> objects;
  mapper_.LatestObjectSet(objects, hist);

  uint64_t max_position;
  ret = Seal(objects, epoch, &max_position);
  if (ret) {
    std::cerr << "failed to seal " << ret << std::endl;
    return ret;
  }

  /*
   * When we create a new empty stripe we have sealed the previous stripe and
   * gotten the max_position for that stripe. However, if no writes occurred
   * to the new stripe then max_position will be zero.
   */
  uint64_t next_epoch = epoch + 1;
  hist.CloneLatestStripe(max_position, next_epoch);
  const auto hist_data = hist.Serialize();

  /*
   * Propose the updated projection for the next epoch.
   */
  ret = new_backend->SetProjection(metalog_oid_, next_epoch, hist_data);
  if (ret != Backend::ZLOG_OK) {
    std::cerr << "failed to set new epoch " << next_epoch
      << " ret " << ret << std::endl;
    return ret;
  }

  return 0;
}
#endif

int LogImpl::SetStripeWidth(int width)
{
  /*
   * Get the current projection. We'll add the new striping width when we
   * propose the next projection/epoch.
   */
  uint64_t epoch;
  zlog_proto::MetaLog config;
  int ret = new_backend->LatestProjection(metalog_oid_, &epoch, config);
  if (ret != Backend::ZLOG_OK) {
    std::cerr << "failed to get projection ret " << ret << std::endl;
    return ret;
  }

  StripeHistory hist;
  ret = hist.Deserialize(config);
  if (ret)
    return ret;
  assert(!hist.Empty());

  std::vector<std::string> objects;
  mapper_.LatestObjectSet(objects, hist);

  uint64_t max_position;
  ret = Seal(objects, epoch, &max_position);
  if (ret) {
    std::cerr << "failed to seal " << ret << std::endl;
    return ret;
  }

  uint64_t next_epoch = epoch + 1;
  hist.AddStripe(max_position, next_epoch, width);
  const auto hist_data = hist.Serialize();

  /*
   * Propose the updated projection for the next epoch.
   */
  ret = new_backend->SetProjection(metalog_oid_, next_epoch, hist_data);
  if (ret != Backend::ZLOG_OK) {
    std::cerr << "failed to set new epoch " << next_epoch
      << " ret " << ret << std::endl;
    return ret;
  }

  return 0;
}

int LogImpl::CreateCut(uint64_t *pepoch, uint64_t *maxpos)
{
  /*
   * Get the current projection. We'll make a copy of this as the next
   * projection.
   */
  uint64_t epoch;
  zlog_proto::MetaLog config;
  int ret = new_backend->LatestProjection(metalog_oid_, &epoch, config);
  if (ret != Backend::ZLOG_OK) {
    std::cerr << "failed to get projection ret " << ret << std::endl;
    return ret;
  }

  StripeHistory hist;
  ret = hist.Deserialize(config);
  if (ret)
    return ret;
  assert(!hist.Empty());

  std::vector<std::string> objects;
  mapper_.LatestObjectSet(objects, hist);

  uint64_t max_position;
  ret = Seal(objects, epoch, &max_position);
  if (ret) {
    std::cerr << "failed to seal " << ret << std::endl;
    return ret;
  }

  /*
   * Propose the next epoch / projection.
   */
  uint64_t next_epoch = epoch + 1;
  ret = new_backend->SetProjection(metalog_oid_, next_epoch, config);
  if (ret != Backend::ZLOG_OK) {
    std::cerr << "failed to set new epoch " << next_epoch
      << " ret " << ret << std::endl;
    return ret;
  }

  *pepoch = next_epoch;
  *maxpos = max_position;

  return 0;
}

int LogImpl::Seal(const std::vector<std::string>& objects,
    uint64_t epoch, uint64_t *next_pos)
{
  /*
   * Seal each object
   */
  for (const auto& oid : objects) {
    int ret = new_backend->Seal(oid, epoch);
    if (ret != Backend::ZLOG_OK) {
      std::cerr << "failed to seal object" << std::endl;
      return ret;
    }
  }

  /*
   * Get next position from each object
   */
  uint64_t max_position;
  bool first = true;
  for (const auto& oid : objects) {
    /*
     * The max_position function should only be called on objects that have
     * been sealed, thus here we return from any error includes -ENOENT. The
     * epoch tag must also match the sealed epoch in the object otherwise
     * we'll receive -EINVAL.
     */
    uint64_t this_pos;
    int ret = new_backend->MaxPos(oid, epoch, &this_pos);
    if (ret != Backend::ZLOG_OK) {
      std::cerr << "failed to find max pos ret " << ret << std::endl;
      return ret;
    }

    if (first) {
      max_position = this_pos;
      first = false;
      continue;
    }

    max_position = std::max(max_position, this_pos);
  }

  *next_pos = max_position;

  return 0;
}

int LogImpl::Delete(){
  for (;;) {
  //deleting the metalog object
    int ret = new_backend->Delete(metalog_oid_);
    if (ret < 0) {
      std::cerr << "delete failed ret " << ret << std::endl;
    }
    return ret;
  }
  assert(0);
}

int LogImpl::RefreshProjection()
{
  for (;;) {
    uint64_t epoch;
    zlog_proto::MetaLog config;
    int ret = new_backend->LatestProjection(metalog_oid_, &epoch, config);
    if (ret != Backend::ZLOG_OK) {
      std::cerr << "failed to get projection ret " << ret << std::endl;
      sleep(1);
      continue;
    }

    StripeHistory hist;
    ret = hist.Deserialize(config);
    if (ret) {
      std::cerr << "RefreshProjection: failed to decode..." << std::endl << std::flush;
      return ret;
    }
    assert(!hist.Empty());

    mapper_.SetHistory(hist, epoch);

    break;
  }

  return 0;
}

int LogImpl::CheckTail(uint64_t *pposition, bool increment)
{
  for (;;) {
    int ret = seqr->CheckTail(mapper_.Epoch(), new_backend->pool(),
        name_, pposition, increment);
    if (ret == -EAGAIN) {
      //std::cerr << "check tail ret -EAGAIN" << std::endl;
      sleep(1);
      continue;
    } else if (ret == -ERANGE) {
      //std::cerr << "check tail ret -ERANGE" << std::endl;
      ret = RefreshProjection();
      if (ret)
        return ret;
      continue;
    }
    return ret;
  }
  assert(0);
}

int LogImpl::CheckTail(uint64_t *pposition)
{
  return CheckTail(pposition, false);
}

int LogImpl::CheckTail(std::vector<uint64_t>& positions, size_t count)
{
  if (count <= 0 || count > 100)
    return -EINVAL;

  for (;;) {
    std::vector<uint64_t> result;
    int ret = seqr->CheckTail(mapper_.Epoch(), new_backend->pool(),
        name_, result, count);
    if (ret == -EAGAIN) {
      //std::cerr << "check tail ret -EAGAIN" << std::endl;
      sleep(1);
      continue;
    } else if (ret == -ERANGE) {
      //std::cerr << "check tail ret -ERANGE" << std::endl;
      ret = RefreshProjection();
      if (ret)
        return ret;
      continue;
    }
    if (ret == 0)
      positions.swap(result);
    return ret;
  }
  assert(0);
}

int LogImpl::CheckTail(const std::set<uint64_t>& stream_ids,
    std::map<uint64_t, std::vector<uint64_t>>& stream_backpointers,
    uint64_t *pposition, bool increment)
{
  for (;;) {
    int ret = seqr->CheckTail(mapper_.Epoch(), new_backend->pool(),
        name_, stream_ids, stream_backpointers, pposition, increment);
    if (ret == -EAGAIN) {
      //std::cerr << "check tail ret -EAGAIN" << std::endl;
      sleep(1);
      continue;
    } else if (ret == -ERANGE) {
      //std::cerr << "check tail ret -ERANGE" << std::endl;
      ret = RefreshProjection();
      if (ret)
        return ret;
      continue;
    }
    return ret;
  }
  assert(0);
}

/*
 * TODO:
 *
 * 1. When a stale epoch is encountered the projection is refreshed and the
 * append is retried with a new tail position retrieved from the sequencer.
 * This means that a hole is created each time an append hits a stale epoch.
 * If there are a lot of clients, a large hole is created each time the
 * projection is refreshed. Is this necessary in all cases? When could a
 * client try again with the same position? We could also mitigate this affect
 * a bit by having the sequencer, upon startup, begin finding log tails
 * proactively instead of waiting for a client to perform a check tail.
 *
 * 2. When a stale epoch return code occurs we continuously look for a new
 * projection and retry the append. to avoid just spinning and creating holes
 * we pause for a second. other options include waiting to observe an _actual_
 * change to a new projection. that is probably better...
 */
int LogImpl::Append(const Slice& data, uint64_t *pposition)
{
  for (;;) {
    uint64_t position;
    int ret = CheckTail(&position, true);
    if (ret)
      return ret;

    uint64_t epoch;
    std::string oid;
    mapper_.FindObject(position, &oid, &epoch);

    ret = new_backend->Write(oid, data, epoch, position);
    if (ret < 0 && ret != -EFBIG) {
      std::cerr << "append: failed ret " << ret << std::endl;
      return ret;
    }

    if (ret == Backend::ZLOG_OK) {
      if (pposition)
        *pposition = position;
      return 0;
    }

    if (ret == Backend::ZLOG_STALE_EPOCH) {
      sleep(1); // avoid spinning in this loop
      ret = RefreshProjection();
      if (ret)
        return ret;
      continue;
    }

#ifdef BACKEND_SUPPORT_DISABLED
    /*
     * When an object becomes too large we seal the entire stripe and create a
     * new stripe of empty objects. First we refresh the projection and try
     * the append again if the projection changed. Otherwise we will attempt
     * to create the new stripe.
     */
    if (ret == -EFBIG) {
      assert(backend_ver == 2);
      CreateNewStripe(epoch);
      ret = RefreshProjection();
      if (ret)
        return ret;
      continue;
    }
#endif

    assert(ret == Backend::ZLOG_READ_ONLY);
  }
  assert(0);
}

int LogImpl::Fill(uint64_t epoch, uint64_t position)
{
  for (;;) {
    std::string oid;
    mapper_.FindObject(position, &oid, NULL);

    int ret = new_backend->Fill(oid, epoch, position);
    if (ret < 0) {
      std::cerr << "fill: failed ret " << ret << std::endl;
      return ret;
    }

    if (ret == Backend::ZLOG_OK)
      return 0;

    if (ret == Backend::ZLOG_STALE_EPOCH) {
      ret = RefreshProjection();
      if (ret)
        return ret;
      continue;
    }

    assert(ret == Backend::ZLOG_READ_ONLY);
    return -EROFS;
  }
}

int LogImpl::Fill(uint64_t position)
{
  for (;;) {
    uint64_t epoch;
    std::string oid;
    mapper_.FindObject(position, &oid, &epoch);

    int ret = new_backend->Fill(oid, epoch, position);
    if (ret < 0) {
      std::cerr << "fill: failed ret " << ret << std::endl;
      return ret;
    }

    if (ret == Backend::ZLOG_OK)
      return 0;

    if (ret == Backend::ZLOG_STALE_EPOCH) {
      ret = RefreshProjection();
      if (ret)
        return ret;
      continue;
    }

    assert(ret == Backend::ZLOG_READ_ONLY);
    return -EROFS;
  }
}

int LogImpl::Trim(uint64_t position)
{
  for (;;) {
    uint64_t epoch;
    std::string oid;
    mapper_.FindObject(position, &oid, &epoch);

    int ret = new_backend->Trim(oid, epoch, position);
    if (ret < 0) {
      std::cerr << "trim: failed ret " << ret << std::endl;
      return ret;
    }

    if (ret == Backend::ZLOG_OK)
      return 0;

    if (ret == Backend::ZLOG_STALE_EPOCH) {
      ret = RefreshProjection();
      if (ret)
        return ret;
      continue;
    }

    assert(0);
  }
}

int LogImpl::Read(uint64_t epoch, uint64_t position, std::string *data)
{
  for (;;) {
    std::string oid;
    mapper_.FindObject(position, &oid, NULL);

    int ret = new_backend->Read(oid, epoch, position, data);
    if (ret < 0) {
      std::cerr << "read failed ret " << ret << std::endl;
      return ret;
    }

    if (ret == Backend::ZLOG_OK)
      return 0;
    else if (ret == Backend::ZLOG_NOT_WRITTEN)
      return -ENODEV;
    else if (ret == Backend::ZLOG_INVALIDATED)
      return -EFAULT;
    else if (ret == Backend::ZLOG_STALE_EPOCH) {
      ret = RefreshProjection();
      if (ret)
        return ret;
      continue;
    } else {
      std::cerr << "unknown reply";
      assert(0);
    }
  }
  assert(0);
}

int LogImpl::Read(uint64_t position, std::string *data)
{
  for (;;) {
    uint64_t epoch;
    std::string oid;
    mapper_.FindObject(position, &oid, &epoch);

    int ret = new_backend->Read(oid, epoch, position, data);
    if (ret < 0) {
      std::cerr << "read failed ret " << ret << std::endl;
      return ret;
    }

    if (ret == Backend::ZLOG_OK)
      return 0;
    else if (ret == Backend::ZLOG_NOT_WRITTEN)
      return -ENODEV;
    else if (ret == Backend::ZLOG_INVALIDATED)
      return -EFAULT;
    else if (ret == Backend::ZLOG_STALE_EPOCH) {
      ret = RefreshProjection();
      if (ret)
        return ret;
      continue;
    } else {
      std::cerr << "unknown reply";
      assert(0);
    }
  }
  assert(0);
}

}
