#pragma once

#include <string.h>

#include <iostream>
#include <set>
#include <vector>

#include "../../include/string.hpp"
#include "../../include/util.hpp"
#include "../../include/inline.hpp"
#include "../../include/procedure.hpp"

#include "common.hpp"
#include "result.hpp"
#include "tictoc_op_element.hpp"
#include "tuple.hpp"

enum class TransactionStatus : uint8_t {
  inFlight,
  committed,
  aborted,
};

extern void writeValGenerator(char *writeVal, size_t val_size, size_t thid);

class TxExecutor {
public:
  int thid;
  uint64_t commit_ts;
  uint64_t appro_commit_ts;
  TicTocResult* tres;
  bool wonly = false;
  vector<Procedure> proSet;

  TransactionStatus status;
  vector<SetElement<Tuple>> readSet;
  vector<SetElement<Tuple>> writeSet;
  vector<Op_element<Tuple>> cll; // current lock list;
  //use for lockWriteSet() to record locks;

  char writeVal[VAL_SIZE];
  char returnVal[VAL_SIZE];

  TxExecutor(int myid, TicTocResult* tres_) : thid(myid), tres(tres_) {
    readSet.reserve(MAX_OPE);
    writeSet.reserve(MAX_OPE);
    cll.reserve(MAX_OPE);
    proSet.reserve(MAX_OPE);

    genStringRepeatedNumber(writeVal, VAL_SIZE, thid);
  }

  void tbegin();
  char* tread(uint64_t key);
  void twrite(uint64_t key);
  bool validationPhase();
  bool preemptive_aborts(const TsWord& v1);
  void abort();
  void writePhase();
  void lockWriteSet();
  void unlockCLL();
  SetElement<Tuple> *searchWriteSet(uint64_t key);
  SetElement<Tuple> *searchReadSet(uint64_t key);
  void dispWS();

  Tuple* get_tuple(Tuple *table, uint64_t key) {
    return &table[key];
  }
};
