#ifndef ZLOG_KVSTORE_SNAPSHOT_H
#define ZLOG_KVSTORE_SNAPSHOT_H
#include <cstdint>
#include <string>
#include <vector>
#include "node.h"

class Snapshot {
 public:
  Snapshot(const NodeRef root, uint64_t seq, std::vector<std::string> desc) :
    root(root), seq(seq), desc(desc)
  {}

  const NodeRef root;
  const uint64_t seq;

  // TODO: remove in favor of some sort of pointer to this state. for example
  // let's have a special RootNodeRef that has additional metadata or
  // something along those lines.
  const std::vector<std::string> desc;
};

#endif
