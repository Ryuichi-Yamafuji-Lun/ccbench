#pragma once

#include <atomic>
#include <mutex>

#include "../../include/cache_line_size.hh"
#include "../../include/inline.hh"
#include "../../include/rwlock.hh"

using namespace std;

class Tuple {
public:
  alignas(CACHE_LINE_SIZE) RWLock lock_;
  char val_[VAL_SIZE];
  
  // Write Index CHANGE TO ATOMIC WRITE INDICATOR LOCK
  int writer_thid_;
  bool write_flag_;
};
