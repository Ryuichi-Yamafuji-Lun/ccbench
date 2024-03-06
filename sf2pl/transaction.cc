
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <limits>

#include "../include/backoff.hh"
#include "../include/debug.hh"
#include "../include/procedure.hh"
#include "../include/result.hh"
#include "include/common.hh"
#include "include/transaction.hh"

using namespace std;

extern void display_procedure_vector(std::vector<Procedure> &pro);

#ifdef __x86_64__
#define Pause()  __asm__ __volatile__ ( "pause" : : : )
#else
#define Pause() {}  // On non-x86 we simply spin
#endif

/**
 * @brief Search xxx set
 * @detail Search element of local set corresponding to given key.
 * In this prototype system, the value to be updated for each worker thread 
 * is fixed for high performance, so it is only necessary to check the key match.
 * @param Key [in] the key of key-value
 * @return Corresponding element of local set
 */
inline SetElement<Tuple> *TxExecutor::searchReadSet(uint64_t key) {
  for (auto itr = read_set_.begin(); itr != read_set_.end(); ++itr) {
    if ((*itr).key_ == key) return &(*itr);
  }

  return nullptr;
}

/**
 * @brief Search xxx set
 * @detail Search element of local set corresponding to given key.
 * In this prototype system, the value to be updated for each worker thread 
 * is fixed for high performance, so it is only necessary to check the key match.
 * @param Key [in] the key of key-value
 * @return Corresponding element of local set
 */
inline SetElement<Tuple> *TxExecutor::searchWriteSet(uint64_t key) {
  for (auto itr = write_set_.begin(); itr != write_set_.end(); ++itr) {
    if ((*itr).key_ == key) return &(*itr);
  }

  return nullptr;
}

/**
 * @brief function about abort.
 * Clean-up local read/write set.
 * Release locks.
 * @return void
 */
void TxExecutor::abort() {
  /**
   * Release locks
   */
  unlockList();

  ++sres_->local_abort_counts_;

#if BACK_OFF
#if ADD_ANALYSIS
  uint64_t start(rdtscp());
#endif

  Backoff::backoff(FLAGS_clocks_per_us);

#if ADD_ANALYSIS
  sres_->local_backoff_latency_ += rdtscp() - start;
#endif

#endif
}

/**
 * @brief success termination of transaction.
 * @return void
 */
void TxExecutor::commit() {
  for (auto itr = write_set_.begin(); itr != write_set_.end(); ++itr) {
    /**
     * update payload.
     */
    memcpy((*itr).rcdptr_->val_, write_val_, VAL_SIZE);
  }

  /**
   * Release locks 
   * Release indicators 
   * clear up local read and write set
   */
  unlockList();
  //printf("CO THID: %d TS: %ld\n", this->thid_, this->thread_timestamp_.load());
  /**
   * Clean-up local variables
   */
  this->current_attempt_ = 0;
  this->thread_timestamp_.store(NO_TIMESTAMP);
  this->conflict_timestamp_.store(NO_TIMESTAMP);
  /**
   * Clear announceTS of thread
   */
  announce_timestamps[this->thid_].store(NO_TIMESTAMP);

}

/**
 * @brief restart transaction.
 * @return void
 */
void TxExecutor::restart() {
  // Check TIMESTAMP if unlocked then move
  //printf("RE Thid: %d TS: %ld CTHID: %d CTS: %ld\n", this->thid_, this->thread_timestamp_.load(), this->conflict_thid_ ,this->conflict_timestamp_.load());
  //retry loop
  while (this->conflict_timestamp_ == announce_timestamps[this->conflict_thid_]) {
    Pause();
  } 
}

/**
 * @brief Initialize function of transaction.
 * Allocate timestamp.
 * @return void
 */
void TxExecutor::begin() { 
  this->status_ = TransactionStatus::inFlight;
  // check if this is first attempt
  if (this->current_attempt_ > 0) {
    // wait for conflict transaction to commit
    restart();
  }
  ++this->current_attempt_;
}

/**
 * @brief Transaction read function.
 * @param [in] key The key of key-value
 */
void TxExecutor::read(uint64_t key) {
  return;
#if ADD_ANALYSIS
  uint64_t start = rdtscp();
#endif  // ADD_ANALYSIS
  
  /**
   * read-own-writes or re-read from local read set.
   */
  if (searchWriteSet(key) || searchReadSet(key)) goto FINISH_READ;
  /**
   * Search tuple from data structure.
   */
  Tuple *tuple;
#if MASSTREE_USE
  tuple = MT.get_value(key);
#if ADD_ANALYSIS
  ++sres_->local_tree_traversal_;
#endif
#else
  tuple = get_tuple(Table, key);
#endif

  if (tuple->lock_.r_trylock()) {
    r_lock_list_.emplace_back(&tuple->lock_);
    read_set_.emplace_back(key, tuple, tuple->val_);
    // switch the byte on the read indicator 
    int index = key * MAX_THREADS + this->thid_;
    read_indicators[index].store(1, std::memory_order_relaxed);
  } else {
    /**
     * Slow Path
     */
    // Get timestamp if first time conflict
    getTimestamp();

    while( true ){
      if (tuple->lock_.r_trylock()) {
        r_lock_list_.emplace_back(&tuple->lock_);
        read_set_.emplace_back(key, tuple, tuple->val_);
        // switch the byte on the read indicator 
        int index = key * MAX_THREADS + this->thid_;
        read_indicators[index].store(1, std::memory_order_relaxed);
        break;
      }
      // Check conflict thread and conflict thread timestamp
      // writer thid because only writer will cause conflict
      this->conflict_thid_ = tuple->writer_thid_;
      this->conflict_timestamp_.store(announce_timestamps[this->conflict_thid_]);
      // Check if timestamp is smaller
      if (this->conflict_timestamp_.load() < this->thread_timestamp_.load()){
        // Add the restart mechanism
        this->status_ = TransactionStatus::aborted;
        goto FINISH_READ;
      }
      
      Pause();
    }

  }

FINISH_READ:
#if ADD_ANALYSIS
  sres_->local_read_latency_ += rdtscp() - start;
#endif
  return;
}

/**
 * @brief transaction write operation
 * @param [in] key The key of key-value
 * @return void
 * Problem in write
 */
void TxExecutor::write(uint64_t key) {
#if ADD_ANALYSIS
  uint64_t start = rdtscp();
#endif
  // if it already wrote the key object once.
  if (searchWriteSet(key)) goto FINISH_WRITE;
  
  // check to see if key is in read_set_
  for (auto rItr = read_set_.begin(); rItr != read_set_.end(); ++rItr) {
    if ((*rItr).key_ == key) {  // hit
      if (!(*rItr).rcdptr_->lock_.tryupgrade()) {
        /**
         * Slow Path
         */
        // Get timestamp if first time conflict
        getTimestamp();

        Tuple *tuple = get_tuple(Table, key);
        //printf("GL Thid: %d CTHID: %d CTS: %ld\n", this->thid_,this->conflict_thid_ ,this->conflict_timestamp_.load());
         while ( true ) {
          if ((*rItr).rcdptr_->lock_.tryupgrade()) {
            break;
          }
          // check if it is write or read lock for conflict MAY BE CAUSING DEADLOCK 
          if (tuple->write_flag_ == true) {
            // write conflict
            this->conflict_thid_ = tuple->writer_thid_;
            this->conflict_timestamp_.store(announce_timestamps[this->conflict_thid_]);
          } else {
            // read conflict
            // Check conflict threads and their timestamps based on reader_thid_ 
            uint64_t index = key * MAX_THREADS;
            for(uint64_t i = 0; i < MAX_THREADS; i++){
              uint64_t current_index = index + i;
              if (read_indicators[current_index].load(std::memory_order_relaxed) == 1) {
                if(this->conflict_timestamp_ < announce_timestamps[i]){
                  this->conflict_thid_ = i;
                  this->conflict_timestamp_.store(announce_timestamps[i]);
                }
              }
            }
          }

          // Check if timestamp is smaller
          if (this->conflict_timestamp_.load() < this->thread_timestamp_.load()){
            // Add the restart mechanism
            this->status_ = TransactionStatus::aborted;
            goto FINISH_WRITE;
          }
          
          Pause();
        }
      }

      // upgrade success
      // remove old element of read lock list.
      for (auto lItr = r_lock_list_.begin(); lItr != r_lock_list_.end();
           ++lItr) {
        if (*lItr == &((*rItr).rcdptr_->lock_)) {
          write_set_.emplace_back(key, (*rItr).rcdptr_);
          w_lock_list_.emplace_back(&(*rItr).rcdptr_->lock_);
          r_lock_list_.erase(lItr);
          break;
        }
      }

      read_set_.erase(rItr);
      goto FINISH_WRITE;
    }
  }
  /**
   * Search tuple from data structure.
   * moved tuple here since necessary
   */
  Tuple *tuple;
#if MASSTREE_USE
  tuple = MT.get_value(key);
#if ADD_ANALYSIS
  ++sres_->local_tree_traversal_;
#endif
#else
  tuple = get_tuple(Table, key);
#endif
  
  if (!tuple->lock_.w_trylock()) {
    
    /**
     * Slow Path
     */

    getTimestamp();
    //printf("GL Thid: %d TS: %ld CTHID: %d CTS: %ld\n", this->thid_, this->thread_timestamp_.load(), this->conflict_thid_ ,this->conflict_timestamp_.load());
    while ( true ) {
      if (tuple->lock_.w_trylock()) {
        break;
      }
      
      // check if it is write or read lock for conflict atomic load check if write lock is -1
      if (tuple->write_flag_ == true) {
        // write conflict
        this->conflict_thid_ = tuple->writer_thid_;
        this->conflict_timestamp_.store(announce_timestamps[this->conflict_thid_]);
      } else {
        // read conflict
        // Check conflict threads and their timestamps based on reader_thid_ 
        // set index to appropriate key for the thread
        uint64_t index = key * MAX_THREADS;
        for(uint64_t i = 0; i < MAX_THREADS; i++){
          uint64_t current_index = index + i;
          if (read_indicators[current_index].load(std::memory_order_relaxed) == 1) {
            if(this->conflict_timestamp_ < announce_timestamps[i]){
              this->conflict_thid_ = i;
              this->conflict_timestamp_.store(announce_timestamps[i]);
            }
          }
        }
      }

      // Check if timestamp is smaller 
      if (this->conflict_timestamp_.load() < this->thread_timestamp_.load()){
        this->status_ = TransactionStatus::aborted;
        goto FINISH_WRITE;
      }
      Pause();
    }
  }

  /**
   * Register the contents to write lock list and write set.
   * Add to tuple
   */
  w_lock_list_.emplace_back(&tuple->lock_);
  write_set_.emplace_back(key, tuple);
  tuple->write_flag_ = true;
  tuple->writer_thid_ = this->thid_;

FINISH_WRITE:
#if ADD_ANALYSIS
  sres_->local_write_latency_ += rdtscp() - start;
#endif  // ADD_ANALYSIS
  return;
}

/**
 * @brief transaction readWrite (RMW) operation
 */
void TxExecutor::readWrite(uint64_t key) {
  // if it already wrote the key object once.
  if (searchWriteSet(key)) goto FINISH_WRITE;

  // check to see if key is in read_set_
  for (auto rItr = read_set_.begin(); rItr != read_set_.end(); ++rItr) {
    if ((*rItr).key_ == key) {  // hit
      if (!(*rItr).rcdptr_->lock_.tryupgrade()) {
        /**
         * Slow Path
         */
        getTimestamp();

        Tuple *tuple = get_tuple(Table, key);
         while ( true ) {
          if ((*rItr).rcdptr_->lock_.tryupgrade()) {
            break;
          }
          // check if it is write or read lock for conflict
          if (tuple->write_flag_ == true) {
            // write conflict
            this->conflict_thid_ = tuple->writer_thid_;
            this->conflict_timestamp_.store(announce_timestamps[this->conflict_thid_]);
          } else {
            // read conflict
            // Check conflict threads and their timestamps based on reader_thid_ 
            uint64_t index = key * MAX_THREADS;
            for(uint64_t i = 0; i < MAX_THREADS; i++){
              uint64_t current_index = index + i;
              if (read_indicators[current_index].load(std::memory_order_relaxed) == 1) {
                if(this->conflict_timestamp_ < announce_timestamps[i]){
                  this->conflict_thid_ = i;
                  this->conflict_timestamp_.store(announce_timestamps[i]);
                }
              }
            }
          }

          // Check if timestamp is smaller
          if (this->conflict_timestamp_.load() < this->thread_timestamp_.load()){
            this->status_ = TransactionStatus::aborted;
            goto FINISH_WRITE;
          }
          
          Pause();
        }
      }


      // upgrade success
      // remove old element of read set.
      for (auto lItr = r_lock_list_.begin(); lItr != r_lock_list_.end();
           ++lItr) {
        if (*lItr == &((*rItr).rcdptr_->lock_)) {
          write_set_.emplace_back(key, (*rItr).rcdptr_);
          w_lock_list_.emplace_back(&(*rItr).rcdptr_->lock_);
          r_lock_list_.erase(lItr);
          break;
        }
      }
      // remove from read_indicator 
      int key = (*rItr).key_;
      int index = key * MAX_THREADS + this->thid_;
      read_indicators[index].store(0,std::memory_order_relaxed);

      // remove from read_set_
      read_set_.erase(rItr);
      goto FINISH_WRITE;
    }
  }

  /**
   * Search tuple from data structure.
   */
  Tuple *tuple;
#if MASSTREE_USE
  tuple = MT.get_value(key);
#if ADD_ANALYSIS
  ++sres_->local_tree_traversal_;
#endif
#else
  tuple = get_tuple(Table, key);
#endif

  if (!tuple->lock_.w_trylock()) {
    /**
     * Starvation freedom
     */

    getTimestamp();

    while ( true ) {
      if (tuple->lock_.w_trylock()) {
        break;
      }
      
      // check if it is write or read lock for conflict
      if (tuple->write_flag_ == true) {
        // write conflict
        this->conflict_thid_ = tuple->writer_thid_;
        this->conflict_timestamp_.store(announce_timestamps[this->conflict_thid_]);
      } else {
        // read conflict
        // Check conflict threads and their timestamps based on reader_thid_ 
        uint64_t index = key * MAX_THREADS;
        for(uint64_t i = 0; i < MAX_THREADS; i++){
          uint64_t current_index = index + i;
          if (read_indicators[current_index].load(std::memory_order_relaxed) == 1) {
            if(this->conflict_timestamp_ < announce_timestamps[i]){
              this->conflict_thid_ = i;
              this->conflict_timestamp_.store(announce_timestamps[i]);
            }
          }
        }
      }
      
      // Check if timestamp is smaller
      if (this->conflict_timestamp_.load() < this->thread_timestamp_.load()){
        this->status_ = TransactionStatus::aborted;
        goto FINISH_WRITE;
      }
      
      Pause();

    }
  }

  // read payload
  memcpy(this->return_val_, tuple->val_, VAL_SIZE);
  // finish read.

  /**
   * Register the contents to write lock list and write set.
   */
  w_lock_list_.emplace_back(&tuple->lock_);
  write_set_.emplace_back(key, tuple);
  tuple->write_flag_ = true;
  tuple->writer_thid_ = this->thid_;

FINISH_WRITE:
  return;
}
/**
 * @brief Slow Path for 2PLSF
 * @return void
 */
void TxExecutor::getTimestamp() {
  // Get timestamp if first time conflict
  if (this->thread_timestamp_ == NO_TIMESTAMP){
    
    this->thread_timestamp_.store(conflict_clock.fetch_add(1));
    announce_timestamps[this->thid_].store(this->thread_timestamp_);
    assert(announce_timestamps[this->thid_] != NO_TIMESTAMP);
  }
}

/**
 * @brief unlock and clean-up local lock set.
 * @return void
 */
void TxExecutor::unlockList() {
  // setup the itr
  auto r_lock_itr = r_lock_list_.begin();
  auto r_set_itr = read_set_.begin(); 
  auto w_lock_itr = w_lock_list_.begin();
  auto w_set_itr = write_set_.begin();

  // read
  while (r_lock_itr != r_lock_list_.end() && r_set_itr != read_set_.end()) {

    // clear read_indicator byte
    int key = r_set_itr->key_;
    int index = key * MAX_THREADS + this->thid_;
    // Access global variables read_indicators and FLAG_thread_num.
    read_indicators[index].store(0, std::memory_order_relaxed);
    
    // unlock read_lock_
    (*r_lock_itr)->r_unlock();

    // Increase itr
    ++r_lock_itr;
    ++r_set_itr;
  }
    
  // write
  while (w_lock_itr != w_lock_list_.end() && w_set_itr != write_set_.end()) {
    // clear write indicator
    int key = w_set_itr->key_;
    
    Tuple *tuple = get_tuple(Table, key);
    if (tuple) {
      tuple->write_flag_ = false;
    }

    // unlock write_lock_
    (*w_lock_itr)->w_unlock();

    // Increase itr
    ++w_lock_itr;
    ++w_set_itr;
  }
  
  /**
   * Clean-up local lock set.
   * Clean-up local set
   */
  r_lock_list_.clear();
  w_lock_list_.clear();
  read_set_.clear();
  write_set_.clear();

}

