// Host test of app/src/native/me_object_table.h: one writer thread publishes objects and replaces their values while reader
// threads look them up lock-free. Every answer must be "unknown" (before the key is published) or one of the values the
// writer stored for that object, never a torn or foreign one; after the writer is done every lookup must return the last
// value; a full table must tell the reader to ask the map. Run it a few times (and under -fsanitize=thread if available).
//   clang++ -std=c++20 -O2 -pthread -I app/src/native tests/cpu/test_native_object_table.cpp -o /tmp/test_native_object_table
#include <atomic>
#include <cstdio>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "me_object_table.h"

using namespace me::native;

struct Entry {
  uint32_t object;
  uint32_t version;
};

int main() {
  constexpr uint32_t kObjects = 1500;
  std::vector<uint32_t> objects;
  std::mt19937 rng(5);
  for (uint32_t i = 0; i < kObjects; ++i) objects.push_back(0x40000000u + (uint32_t(rng()) & 0x3EFFFFF0u));
  std::vector<std::vector<Entry>> pool(kObjects, std::vector<Entry>(6));  // stable addresses of the stored values
  for (uint32_t i = 0; i < kObjects; ++i)
    for (uint32_t v = 0; v < 6; ++v) pool[i][v] = {objects[i], v};

  ObjectEntryTable<const Entry*> table;
  std::atomic<bool> done{false};
  std::atomic<uint64_t> bad{0}, answered{0};
  std::map<uint32_t, uint32_t> last_version;

  std::vector<std::thread> readers;
  for (int t = 0; t < 3; ++t) {
    readers.emplace_back([&, t] {
      std::mt19937 r(100 + t);
      std::vector<uint32_t> seen_version(kObjects, 0);
      while (!done.load()) {
        const uint32_t i = uint32_t(r() % kObjects);
        const Entry* e = nullptr;
        const bool ok = table.Lookup(objects[i], e);
        if (!ok) { ++bad; continue; }
        if (e) {
          if (e->object != objects[i] || e->version >= 6 || e->version < seen_version[i]) ++bad;  // foreign or went back
          seen_version[i] = e->version;
        }
        ++answered;
      }
    });
  }
  // writer: insert every object, then replace values several times (versions only go up)
  for (uint32_t v = 0; v < 6; ++v)
    for (uint32_t i = 0; i < kObjects; ++i) {
      table.Publish(objects[i], &pool[i][v]);
      if ((i & 63) == 0) std::this_thread::yield();
    }
  done = true;
  for (auto& t : readers) t.join();
  uint32_t wrong_final = 0;
  for (uint32_t i = 0; i < kObjects; ++i) {
    const Entry* e = nullptr;
    if (!table.Lookup(objects[i], e) || !e || e->version != 5 || e->object != objects[i]) ++wrong_final;
  }
  // unknown object
  const Entry* e = reinterpret_cast<const Entry*>(0x1);
  const bool unknown_ok = table.Lookup(0x7E000010u, e) && e == nullptr;
  // overflow: many keys hashing into one probe window mark the table full; unknown lookups then defer to the map
  ObjectEntryTable<const Entry*> small;
  Entry dummy{0, 0};
  for (uint32_t i = 0; i < ObjectEntryTable<const Entry*>::kSlots + 8; ++i) small.Publish(0x40000010u + i * 16u, &dummy);
  const Entry* none = nullptr;
  const bool full_defers = !small.Lookup(0x7F000000u, none);
  std::printf("%llu lookups answered, %llu bad, %u wrong final values, unknown ok %d, full table defers %d\n",
              (unsigned long long)answered.load(), (unsigned long long)bad.load(), wrong_final, unknown_ok, full_defers);
  return (bad.load() || wrong_final || !unknown_ok || !full_defers) ? 1 : 0;
}
