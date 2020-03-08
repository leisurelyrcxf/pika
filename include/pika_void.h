// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef PIKA_VOID_H
#define PIKA_VOID_H

#include "pika_command.h"

/*
 * pubsub
 */
class VoidCmd : public Cmd {
 public:
  VoidCmd(const std::string& name, int arity, uint16_t flag)
     : Cmd(name,  arity, flag) {}
  void Do(std::shared_ptr<Partition> partition = nullptr) override;
  Cmd* Clone() override {
    return new VoidCmd(*this);
  }
 private:
  void DoInitial() override;
};

#endif  // PIKA_VOID_H
