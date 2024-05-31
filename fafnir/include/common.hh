#pragma once

#include <atomic>
#include <vector>
#include <memory>
#include <chrono>
#include <mutex>
#include <deque>

#include "../../include/cache_line_size.hh"
#include "../../include/int64byte.hh"
#include "../../include/masstree_wrapper.hh"
#include "tuple.hh"
#include "global_timer.hh"

#include "gflags/gflags.h"
#include "glog/logging.h"

#ifdef GLOBAL_VALUE_DEFINE
#define GLOBAL

#if MASSTREE_USE
alignas(CACHE_LINE_SIZE) GLOBAL MasstreeWrapper<Tuple> MT;
#endif

#else
#define GLOBAL extern

#if MASSTREE_USE
alignas(CACHE_LINE_SIZE) GLOBAL MasstreeWrapper<Tuple> MT;
#endif

#endif

#ifdef GLOBAL_VALUE_DEFINE
DEFINE_uint64(clocks_per_us, 2100,
              "CPU_MHz. Use this info for measuring time.");
DEFINE_uint64(extime, 3, "Execution time[sec].");
DEFINE_uint64(max_ope, 10,
              "Total number of operations per single transaction.");
DEFINE_bool(rmw, false,
            "True means read modify write, false means blind write.");
DEFINE_uint64(rratio, 0, "read ratio of single transaction.");
DEFINE_uint64(thread_num, 2, "Total number of worker threads.");
DEFINE_uint64(tuple_num, 1, "Total number of records.");
DEFINE_bool(ycsb, true,
            "True uses zipf_skew, false uses faster random generator.");
DEFINE_double(zipf_skew, 0, "zipf skew. 0 ~ 0.999...");
#else
DECLARE_uint64(clocks_per_us);
DECLARE_uint64(extime);
DECLARE_uint64(max_ope);
DECLARE_bool(rmw);
DECLARE_uint64(rratio);
DECLARE_uint64(thread_num);
DECLARE_uint64(tuple_num);
DECLARE_bool(ycsb);
DECLARE_double(zipf_skew);
#endif

// Constant
// Constant for No Timestamp (Cannot be 0 or errors will occur)
static const uint64_t NO_TIMESTAMP = 0xFFFFFFFFFFFFFFFFULL;

// GLOBAL 
alignas(CACHE_LINE_SIZE) GLOBAL Tuple *Table;
// Conflict Clock
extern std::atomic<uint64_t> conflict_clock;
// global dynamic array of Announce Timestamp 
extern std::deque<std::atomic<uint64_t>*> announce_timestamps;
// readIndicator [NUM_TUPLE * MAX_THR] 
extern std::deque<std::atomic<uint64_t>*> read_indicators;
// wlock [NUM_TUPLE]
extern std::atomic<uint64_t>* write_locks;
// Long transaction tracker
extern GlobalTimer timer;
// mutex for resizing
extern std::mutex mtx;
// check how many threads added
extern std::atomic<uint64_t> counter;