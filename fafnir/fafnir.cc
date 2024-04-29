
#include <ctype.h>  //isdigit,
#include <pthread.h>
#include <string.h>       //strlen,
#include <sys/syscall.h>  //syscall(SYS_gettid),
#include <sys/types.h>    //syscall(SYS_gettid),
#include <unistd.h>       //syscall(SYS_gettid),
#include <x86intrin.h>

#include <iostream>
#include <string>  //string
#include <thread>

#define GLOBAL_VALUE_DEFINE

#include "../include/atomic_wrapper.hh"
#include "../include/backoff.hh"
#include "../include/cpu.hh"
#include "../include/debug.hh"
#include "../include/fence.hh"
#include "../include/int64byte.hh"
#include "../include/masstree_wrapper.hh"
#include "../include/procedure.hh"
#include "../include/random.hh"
#include "../include/result.hh"
#include "../include/tsc.hh"
#include "../include/util.hh"
#include "../include/zipf.hh"
#include "include/common.hh"
#include "include/result.hh"
#include "include/transaction.hh"
#include "include/util.hh"

void worker(size_t thid, char &ready, const bool &start, const bool &quit) {
  Result &myres = std::ref(FAFNIRResult[thid]);
  Xoroshiro128Plus rnd;
  rnd.init();
  TxExecutor trans(thid, (Result *) &myres);
  FastZipf zipf(&rnd, FLAGS_zipf_skew, FLAGS_tuple_num);
  Backoff backoff(FLAGS_clocks_per_us);

#if MASSTREE_USE
  MasstreeWrapper<Tuple>::thread_init(int(thid));
#endif

#ifdef Linux
  setThreadAffinity(thid);
  // printf("Thread #%d: on CPU %d\n", *myid, sched_getcpu());
  // printf("sysconf(_SC_NPROCESSORS_CONF) %ld\n",
  // sysconf(_SC_NPROCESSORS_CONF));
#endif  // Linux

  storeRelease(ready, 1);
  while (!loadAcquire(start)) _mm_pause();
  while (!loadAcquire(quit)) {
    makeProcedure(trans.pro_set_, rnd, zipf, FLAGS_tuple_num, FLAGS_max_ope, FLAGS_thread_num,
                  FLAGS_rratio, FLAGS_rmw, FLAGS_ycsb, false, thid, myres);
RETRY:
    if (loadAcquire(quit)) break;
    if (thid == 0) leaderBackoffWork(backoff, FAFNIRResult);

    trans.begin();
    for (auto itr = trans.pro_set_.begin(); itr != trans.pro_set_.end();
         ++itr) {
      if ((*itr).ope_ == Ope::READ) {
        trans.read((*itr).key_);
      } else if ((*itr).ope_ == Ope::WRITE) {
        trans.write((*itr).key_);
      } else if ((*itr).ope_ == Ope::READ_MODIFY_WRITE) {
        trans.readWrite((*itr).key_);
      } else {
        ERR;
      }

      if (trans.status_ == TransactionStatus::aborted) {
        trans.abort();
        // Does not abort goes to retry
        goto RETRY;
      }
    }

    trans.commit();
    /**
     * local_commit_counts is used at ../include/backoff.hh to calculate about
     * backoff.
     */
    storeRelease(myres.local_commit_counts_,
                 loadAcquire(myres.local_commit_counts_) + 1);
  }

  return;
}

// Global variables for fafnir
alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> conflict_clock{1};
alignas(CACHE_LINE_SIZE) std::vector<std::unique_ptr<std::atomic<uint64_t>>> announce_timestamps;
alignas(CACHE_LINE_SIZE) std::vector<std::unique_ptr<std::atomic<uint64_t>>> read_indicators;
alignas(CACHE_LINE_SIZE) std::atomic<uint64_t>* write_locks;
// Initiate global timer
GlobalTimer timer;
std::atomic<bool> timer_thread_active(true);
static const uint64_t pause_timer = 30;

// Check for long transactions
void TimerCheckerThread(std::vector<std::thread>& thv, std::vector<char>& readys, bool& start, bool& quit) {
  while (timer_thread_active) {

    // Check for long transactions over 40ms
    if (timer.elapsed() > std::chrono::milliseconds(pause_timer)){
      // Dynamically add a new worker thread
      std::thread newThread(worker, thv.size(), std::ref(readys[thv.size()]), std::ref(start), std::ref(quit));
      thv.push_back(std::move(newThread));

      // Add Thread to announce timestamp
      announce_timestamps.resize(thv.size());
      announce_timestamps[thv.size() - 1] = std::make_unique<std::atomic<uint64_t>>(NO_TIMESTAMP);


      // Add Thread to readIndicator
      uint64_t NEW_NUM_RI_WORD = FLAGS_tuple_num * thv.size();
      read_indicators.resize(NEW_NUM_RI_WORD);

      for(size_t i = 0; i < FLAGS_tuple_num; ++i) {
        auto insert_index = (i + 1) * (thv.size());
        read_indicators.insert(read_indicators.begin() + insert_index,std::make_unique<std::atomic<uint64_t>>(NO_TIMESTAMP));
      }

      // update thread size
      FLAGS_thread_num = thv.size();

    }

    // Sleep for 40ms
    std::this_thread::sleep_for(std::chrono::milliseconds(pause_timer));
  }
}

int main(int argc, char *argv[]) try {
  gflags::SetUsageMessage("FafnirDT benchmark.");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  chkArg();
  makeDB();
  // set constants
  static const uint64_t NUM_RI = FLAGS_tuple_num;

  static const uint64_t INITIAL_THREAD = FLAGS_thread_num;

  static const uint64_t NUM_RI_WORD = NUM_RI * INITIAL_THREAD;
  // Initialize announce_timstamps
  // Set timestamps to the number of initial threads
  announce_timestamps.resize(INITIAL_THREAD);
  for (size_t i = 0; i < INITIAL_THREAD; i++) {
      announce_timestamps[i] = std::make_unique<std::atomic<uint64_t>>(NO_TIMESTAMP);
  }
  // wlocks setup [NUM_TUPLE]
  write_locks = new std::atomic<uint64_t>[NUM_RI];
  for (size_t i = 0; i < NUM_RI; i++){
    write_locks[i].store(-1, std::memory_order_relaxed);
  }
  // readIndicator setup [NUM_THREAD x NUM_TUPLE]
  read_indicators.resize(NUM_RI_WORD);
  for (size_t i = 0; i < NUM_RI_WORD; i++) {
      read_indicators[i] = std::make_unique<std::atomic<uint64_t>>(NO_TIMESTAMP);
  }

  alignas(CACHE_LINE_SIZE) bool start = false;
  alignas(CACHE_LINE_SIZE) bool quit = false;
  initResult();
  std::vector<char> readys(FLAGS_thread_num);
  std::vector<std::thread> thv;
  for (size_t i = 0; i < FLAGS_thread_num; ++i)
    thv.emplace_back(worker, i, std::ref(readys[i]), std::ref(start),
                     std::ref(quit));
  waitForReady(readys);
  // begin timer
  timer.start();
  std::thread timer_thread(TimerCheckerThread, std::ref(thv), std::ref(readys), std::ref(start), std::ref(quit));
  storeRelease(start, true);
  for (size_t i = 0; i < FLAGS_extime; ++i) {
    sleepMs(1000);
  }
  storeRelease(quit, true);

  for (auto &th : thv) th.join();
  // end work
  // stop global_timer
  timer_thread_active = false;
  timer_thread.join();
  timer.stop();

  // Deallocate memory for write_locks
  delete[] write_locks;

  for (unsigned int i = 0; i < FLAGS_thread_num; ++i) {
    FAFNIRResult[0].addLocalAllResult(FAFNIRResult[i]);
  }
  ShowOptParameters();
  FAFNIRResult[0].displayAllResult(FLAGS_clocks_per_us, FLAGS_extime, FLAGS_thread_num);

  return 0;
} catch (bad_alloc&) {
  ERR;
}
