// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef YB_SERVER_LOGICAL_CLOCK_H_
#define YB_SERVER_LOGICAL_CLOCK_H_

#include <string>

#include "yb/server/clock.h"
#include "yb/util/metrics.h"
#include "yb/util/status.h"

namespace yb {
class MonoDelta;
class MonoTime;
namespace server {

// An implementation of Clock that behaves as a plain Lamport Clock.
// In a single node, single tablet, setting this generates exactly the
// same Timestamp sequence as the original MvccManager did, but it can be
// updated to make sure replicas generate new hybrid_times on becoming leader.
// This can be used as a deterministic hybrid_time generator that has the same
// consistency properties as a HybridTime clock.
//
// The Wait* methods are unavailable in this implementation and will
// return Status::ServiceUnavailable().
//
// NOTE: this class is thread safe.
class LogicalClock : public Clock {
 public:

  virtual CHECKED_STATUS Init() override { return Status::OK(); }

  virtual HybridTime Now() override;

  // In the logical clock this call is equivalent to Now();
  virtual HybridTime NowLatest() override;

  virtual CHECKED_STATUS Update(const HybridTime& to_update) override;

  // The Wait*() functions are not available for this clock.
  virtual CHECKED_STATUS WaitUntilAfter(const HybridTime& then,
                                const MonoTime& deadline) override;
  virtual CHECKED_STATUS WaitUntilAfterLocally(const HybridTime& then,
                                       const MonoTime& deadline) override;

  virtual bool IsAfter(HybridTime t) override;

  virtual void RegisterMetrics(const scoped_refptr<MetricEntity>& metric_entity) override;

  virtual std::string Stringify(HybridTime hybrid_time) override;

  // Logical clock doesn't support COMMIT_WAIT.
  virtual bool SupportsExternalConsistencyMode(ExternalConsistencyMode mode) override {
    return mode != COMMIT_WAIT;
  }

  // Creates a logical clock whose first output value on a Now() call is 'hybrid_time'.
  static LogicalClock* CreateStartingAt(const HybridTime& hybrid_time);

 private:
  // Should use LogicalClock::CreatingStartingAt()
  explicit LogicalClock(HybridTime::val_type initial_time) : now_(initial_time) {}

  // Used to get the hybrid_time for metrics.
  uint64_t NowForMetrics();

  base::subtle::Atomic64 now_;

  FunctionGaugeDetacher metric_detacher_;
};

}  // namespace server
}  // namespace yb

#endif /* YB_SERVER_LOGICAL_CLOCK_H_ */
