// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#if defined(__linux__) || defined(__GLIBC__)
#include <inttypes.h>
#include <sys/sysinfo.h>
#endif

#include "build.h"
#include "subprocess.h"

struct RealCommandRunner : public CommandRunner {
  explicit RealCommandRunner(const BuildConfig& config) : config_(config) {}
  size_t CanRunMore() const override;
  bool StartCommand(Edge* edge) override;
  bool WaitForCommand(Result* result) override;
  std::vector<Edge*> GetActiveEdges() override;
  void Abort() override;

  const BuildConfig& config_;
  SubprocessSet subprocs_;
  std::map<const Subprocess*, Edge*> subproc_to_edge_;
};

std::vector<Edge*> RealCommandRunner::GetActiveEdges() {
  std::vector<Edge*> edges;
  for (std::map<const Subprocess*, Edge*>::iterator e =
           subproc_to_edge_.begin();
       e != subproc_to_edge_.end(); ++e)
    edges.push_back(e->second);
  return edges;
}

void RealCommandRunner::Abort() {
  subprocs_.Clear();
}

size_t RealCommandRunner::CanRunMore() const {
  size_t subproc_number =
      subprocs_.running_.size() + subprocs_.finished_.size();

  int64_t capacity = config_.parallelism - subproc_number;

  if (config_.max_load_average > 0.0f) {
    int load_capacity = config_.max_load_average - GetLoadAverage();
    if (load_capacity < capacity)
      capacity = load_capacity;
  }

#if defined(__linux__) || defined(__GLIBC__)
  if (config_.max_used_memory > 0) {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
      int64_t used_ram = (si.totalram - si.freeram) * si.mem_unit;
      int64_t mem_capacity = config_.max_used_memory - used_ram;
      // fprintf(stderr,
      //         "limit: %" PRId64 " used_ram: %" PRId64 " capacity %" PRId64
      //         "\n", config_.max_used_memory, used_ram, mem_capacity);
      if (mem_capacity < 0) {
        fprintf(stderr,
                "Memory limit reached (%" PRId64 " lacking), throttling\n",
                mem_capacity);
        capacity = 0;
      } else {
        fprintf(stderr, "OK, %" PRId64 " RAM remaining\n", mem_capacity);
      }
    }
  } else {
    fprintf(stderr, "Memory limit not yet set...");
  }
#endif

  if (capacity < 0)
    capacity = 0;

  if (capacity == 0 && subprocs_.running_.empty())
    // Ensure that we make progress.
    capacity = 1;

  fprintf(stderr, "final capacity: %ld\n", capacity);
  return capacity;
}

bool RealCommandRunner::StartCommand(Edge* edge) {
  std::string command = edge->EvaluateCommand();
  Subprocess* subproc = subprocs_.Add(command, edge->use_console());
  if (!subproc)
    return false;
  subproc_to_edge_.insert(std::make_pair(subproc, edge));

  return true;
}

bool RealCommandRunner::WaitForCommand(Result* result) {
  Subprocess* subproc;
  while ((subproc = subprocs_.NextFinished()) == NULL) {
    bool interrupted = subprocs_.DoWork();
    if (interrupted)
      return false;
  }

  result->status = subproc->Finish();
  result->output = subproc->GetOutput();

  std::map<const Subprocess*, Edge*>::iterator e =
      subproc_to_edge_.find(subproc);
  result->edge = e->second;
  subproc_to_edge_.erase(e);

  delete subproc;
  return true;
}

CommandRunner* CommandRunner::factory(const BuildConfig& config) {
  return new RealCommandRunner(config);
}
