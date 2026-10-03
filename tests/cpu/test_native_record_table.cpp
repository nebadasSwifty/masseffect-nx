// Host test of app/src/native/me_record_table.h: the dense start/order arrays (masseffect_native_fast_pair) and the slot scan
// against a verbatim copy of the ORIGINAL implementation (struct Original below: the slot array with valid/start/order that
// me_native_system.cpp used before), over random record streams with bucket collisions, evictions, replaced keys and
// consumed records. Every lookup must return the same record in all three, and so must a final sweep of the address space.
//   clang++ -std=c++20 -O2 -I app/src/native tests/cpu/test_native_record_table.cpp -o /tmp/test_native_record_table && /tmp/test_native_record_table
#include <cstdio>
#include <random>
#include <vector>

#include "me_record_table.h"

using namespace me::native;

struct Rec {
  uint32_t id = 0;
  std::vector<int> streams;  // a non-trivial member, as DrawRecord has
};

// --- the original implementation, as it was in me_native_system.cpp ---------------------------------------------------
struct Original {
  struct Slot {
    Rec record;
    uint32_t start = 0;
    uint64_t order = 0;
    bool valid = false;
  };
  static constexpr uint32_t kRecordBuckets = 4096, kRecordsPerBucket = 8;
  std::vector<Slot> table = std::vector<Slot>(kRecordBuckets * kRecordsPerBucket);
  uint64_t g_record_order = 0;
  void Insert(Rec&& record, uint32_t start) {
    Slot* bucket = &table[((start >> 13) & (kRecordBuckets - 1)) * kRecordsPerBucket];
    Slot* slot = nullptr;
    for (uint32_t i = 0; i < kRecordsPerBucket && !slot; ++i)
      if (bucket[i].valid && bucket[i].start == start) slot = &bucket[i];
    for (uint32_t i = 0; i < kRecordsPerBucket && !slot; ++i)
      if (!bucket[i].valid) slot = &bucket[i];
    if (!slot) {
      slot = &bucket[0];
      for (uint32_t i = 1; i < kRecordsPerBucket; ++i)
        if (bucket[i].order < slot->order) slot = &bucket[i];
    }
    slot->record = std::move(record);
    slot->start = start;
    slot->order = ++g_record_order;
    slot->valid = true;
  }
  Slot* Find(uint32_t address) {
    Slot* best = nullptr;
    for (uint32_t k = 0; k < 2; ++k) {
      const uint32_t b = ((address >> 13) - k) & (kRecordBuckets - 1);
      Slot* bucket = &table[b * kRecordsPerBucket];
      for (uint32_t i = 0; i < kRecordsPerBucket; ++i) {
        Slot& s = bucket[i];
        if (s.valid && s.start <= address && (!best || s.start > best->start)) best = &s;
      }
    }
    return best;
  }
};

int main() {
  std::mt19937_64 rng(11);
  Original orig;
  DrawRecordTable<Rec> dense, scan;
  int failures = 0;
  uint64_t lookups = 0, found = 0;
  uint32_t next_id = 1;
  // starts clustered in a few 8 KB windows so buckets fill up (> 8 live records per bucket) and wrap around the table
  auto random_start = [&]() -> uint32_t {
    const uint32_t window = uint32_t(rng() % 24);
    const uint32_t base = (window % 3 == 0) ? 0x1FFFE000u - window * 0x2000u : window * 0x2000u + 0x100000u;
    return (base + uint32_t(rng() % 0x2000u)) & 0x1FFFFFFFu;
  };
  for (int step = 0; step < 4000000; ++step) {
    const uint32_t action = uint32_t(rng() % 10);
    if (action < 5) {
      const uint32_t start = random_start();
      const uint32_t id = next_id++;
      orig.Insert(Rec{id, {}}, start);
      dense.Insert(Rec{id, {}}, start, true);
      scan.Insert(Rec{id, {}}, start, false);
    } else {
      const uint32_t address = (action < 9 ? random_start() : uint32_t(rng() & 0x1FFFFFFFu)) + (rng() % 3 == 0 ? 0x1000 : 0);
      auto* so = orig.Find(address);
      auto* sd = dense.FindDense(address);
      auto* ss = scan.FindScan(address);
      auto* sx = dense.FindScan(address);
      ++lookups;
      const bool same = (so == nullptr) == (sd == nullptr) && (so == nullptr) == (ss == nullptr) && sd == sx &&
                        (!so || (so->record.id == sd->record.id && so->record.id == ss->record.id && so->start == sd->start &&
                                 so->start == ss->start && dense.IndexOf(sd) == scan.IndexOf(ss)));
      if (!same) {
        std::printf("FAIL lookup %d at %08X\n", step, address);
        if (++failures > 10) return 1;
      }
      if (so) {
        ++found;
        if (rng() % 2 == 0) {
          so->valid = false;
          dense.Consume(sd);
          scan.Consume(ss);
        }
      }
    }
  }
  // final sweep over the address space
  for (uint32_t address = 0; address < 0x20000000u; address += 0x1000u) {
    auto* so = orig.Find(address);
    auto* sd = dense.FindDense(address);
    auto* ss = scan.FindScan(address);
    if ((so == nullptr) != (sd == nullptr) || (so == nullptr) != (ss == nullptr) ||
        (so && (so->record.id != sd->record.id || so->record.id != ss->record.id || so->start != sd->start))) {
      std::printf("FAIL final state at %08X\n", address);
      if (++failures > 10) return 1;
    }
  }
  std::printf("%llu lookups (%llu found), %d failures\n", (unsigned long long)lookups, (unsigned long long)found, failures);
  return failures ? 1 : 0;
}
