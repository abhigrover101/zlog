#include <string>
#include <cstdlib>
#include <sstream>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <thread>
#include <rados/librados.hpp>
#include <gtest/gtest.h>
#include "../../zstate/objects/register.h"

static std::string randstr(void)
{
    std::stringstream ss;
    ss << boost::uuids::random_generator()();
    return ss.str();
}

static void make_context(librados::Rados& rados, librados::IoCtx& ioctx)
{
  assert(!rados.init(NULL));
  assert(!rados.conf_read_file(NULL));
  assert(!rados.connect());

  std::stringstream ss;
  ss << boost::uuids::random_generator()();
  std::string pool = ss.str();

  rados.pool_create("contrail");
  assert(!rados.ioctx_create("contrail", ioctx));
}

static void get_log(librados::IoCtx& ioctx, zlog::Log& log, std::string name,
    zlog::SeqrClient *client)
{
  int ret = zlog::Log::OpenOrCreate(ioctx, name, 13, client, log);
  ASSERT_EQ(ret, 0);
}

static void get_stream(zlog::Log& log, uint64_t stream_id, zlog::Log::Stream& stream)
{
  int ret = log.OpenStream(stream_id, stream);
  ASSERT_EQ(ret, 0);
}

TEST(Register, DefaultValue) {
  librados::Rados rados;
  librados::IoCtx ioctx;
  make_context(rados, ioctx);

  zlog::SeqrClient client("localhost", "5678");
  ASSERT_NO_THROW(client.Connect());

  std::string log_name = randstr();
  zlog::Log log;
  get_log(ioctx, log, log_name, &client);

  zlog::Log::Stream stream;
  get_stream(log, 0, stream);

  Register reg(&stream);

  int value;
  ASSERT_EQ(0, reg.Read(&value));
  ASSERT_EQ(0, value);
}

TEST(Register, Basic) {
  librados::Rados rados;
  librados::IoCtx ioctx;
  make_context(rados, ioctx);

  zlog::SeqrClient client("localhost", "5678");
  ASSERT_NO_THROW(client.Connect());

  std::string log_name = randstr();

  zlog::Log log;
  get_log(ioctx, log, log_name, &client);

  zlog::Log::Stream stream;
  get_stream(log, 0, stream);

  Register reg(&stream);

  int value;
  ASSERT_EQ(0, reg.Write(5));
  ASSERT_EQ(0, reg.Read(&value));
  ASSERT_EQ(5, value);
  ASSERT_EQ(0, reg.Write(5));
  ASSERT_EQ(0, reg.Write(500));
  ASSERT_EQ(0, reg.Write(333));
  ASSERT_EQ(0, reg.Read(&value));
  ASSERT_EQ(333, value);
}

static void thrash_log(librados::Rados *rados, std::string pool_name, std::string log_name)
{
  librados::IoCtx ioctx;
  assert(!rados->ioctx_create(pool_name.c_str(), ioctx));

  zlog::SeqrClient client("localhost", "5678");
  ASSERT_NO_THROW(client.Connect());

  zlog::Log log;
  get_log(ioctx, log, log_name, &client);

  zlog::Log::Stream stream;
  get_stream(log, 0, stream);

  Register reg(&stream);

  for (int i = 0; i < 100; i++) {
    int value = std::rand() + 1; // ensure positive
    ASSERT_EQ(0, reg.Write(value));
    ASSERT_EQ(0, reg.Read(&value));
  }
}

TEST(Register, MultiThreaded) {
  librados::Rados rados;
  assert(!rados.init(NULL));
  assert(!rados.conf_read_file(NULL));
  assert(!rados.connect());

  std::string pool_name = "contrail";
  rados.pool_create(pool_name.c_str());

  std::string log_name = randstr();

  librados::IoCtx ioctx;
  assert(!rados.ioctx_create(pool_name.c_str(), ioctx));

  zlog::SeqrClient client("localhost", "5678");
  ASSERT_NO_THROW(client.Connect());

  zlog::Log log;
  get_log(ioctx, log, log_name, &client);

  std::vector<std::thread> threads;
  for (int i = 0; i < 3; i++) {
    std::thread t(thrash_log, &rados, pool_name, log_name);
    threads.push_back(std::move(t));
  }

  for (auto it = threads.begin(); it != threads.end(); it++)
    it->join();

  zlog::Log::Stream stream;
  get_stream(log, 0, stream);

  Register reg(&stream);

  int value;
  ASSERT_EQ(0, reg.Read(&value));
  ASSERT_LT(0, value);

  ASSERT_EQ(0, reg.Write(10));
  ASSERT_EQ(0, reg.Read(&value));
  ASSERT_EQ(10, value);
}
