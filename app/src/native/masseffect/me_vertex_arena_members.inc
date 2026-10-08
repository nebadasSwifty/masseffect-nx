// Cross-frame vertex arena (docs/zero-copy-vertices.md, "Cross-frame vertex arena"): the renderer side. Included inside
// DrawsVulkanImpl by masseffect_native_draws.cpp, after me_draw_cache_members.inc. The logic is in me_vertex_arena.h,
// the settings in me_vertex_arena_cvars.inc. Hooks in Draw(): VaMeasure and VaBinding in the vertex upload loop after
// the in-frame dedupe missed, the per-binding buffer (va_mask) at the vertex bind and in the postponed sky; and
// Initialize (VaInit), UseSlot (VaSlotStarted, VaReport), BeforeSend (VaFlush), the destructor (VaDestroy).

  enum VaOutcome : int { kVaCopy = 0, kVaHit = 1, kVaStored = 2 };
  // SampleFingerprint reads every byte of a range up to two 4 KB blocks: for these the check of
  // masseffect_native_vertex_arena_check = 1 is a full content comparison (up to the 64-bit hash).
  static constexpr uint32_t kVaWholeFingerprint = 8192;

  // --- State ---------------------------------------------------------------------------------------------------------
  bool va_measure_ = false;  // masseffect_native_vertex_arena_measure
  std::unique_ptr<me::native::vertex_arena::Measure> va_measure_tab_;
  me::native::vertex_arena::Measure::Counts va_measure_reported_{};
  bool va_on_ = false;       // the arena serves hits and takes copies (a guard may switch it off)
  bool va_created_ = false;  // the buffer exists (draws recorded from it stay valid after a switch-off)
  int32_t va_check_ = 1;
  uint32_t va_min_bytes_ = 256;
  uint64_t va_max_undeclared_ = 4;
  bool va_full_at_store_ = false;  // a full hash is taken at every store (check 2, or the verification is on)
  me::native::vertex_arena::Arena va_;
  me::native::vertex_arena::ExemptPages va_exempt_;
  me::native::draw_cache::Verifier va_verify_;
  VkBuffer va_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory va_memory_ = VK_NULL_HANDLE;
  uint32_t va_type_ = 0;
  VkDeviceSize va_real_size_ = 0;
  bool va_coherent_ = true;
  uint8_t* va_data_ = nullptr;
  uint32_t va_recorded_mask_ = 0;  // bindings of the last vkCmdBindVertexBuffers taken from the arena
  const char* va_off_reason_ = nullptr;
  struct VaCounters {
    uint64_t lookups = 0, hits = 0, bytes_hit = 0, stored = 0, bytes_stored = 0, first_seen = 0, content_changed = 0,
             dirty_same = 0, dirty_changed = 0, undeclared = 0, undeclared_counted = 0, skip_size = 0,
             skip_exempt = 0, no_space = 0, exempt_from_measure = 0;
  };
  VaCounters va_c_{}, va_reported_{};
  uint64_t va_events_reported_ = 0, va_verified_reported_ = 0, va_recycles_reported_ = 0, va_waits_reported_ = 0;
  std::chrono::steady_clock::time_point va_report_{};

  // --- Set-up --------------------------------------------------------------------------------------------------------
  void VaInit() {
    namespace tc = me::native::texture_coherency;
    va_measure_ = REXCVAR_GET(masseffect_native_vertex_arena_measure);
    if (va_measure_) va_measure_tab_ = std::make_unique<me::native::vertex_arena::Measure>();
    if (REXCVAR_GET(masseffect_native_vertex_arena)) {
      const VkDeviceSize size = VkDeviceSize(REXCVAR_GET(masseffect_native_vertex_arena_mb)) << 20;
      void* mapped = nullptr;
      if (rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device_, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rex::ui::vulkan::util::MemoryPurpose::kUpload,
              va_buffer_, va_memory_, &va_type_, &va_real_size_) &&
          dfn_.vkMapMemory(device_, va_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        va_data_ = static_cast<uint8_t*>(mapped);
        va_coherent_ = (vulkan_device_->memory_types().host_coherent >> va_type_) & 0x1;
        va_.Configure(size, size_t(1) << 15);
        va_check_ = REXCVAR_GET(masseffect_native_vertex_arena_check);
        va_min_bytes_ = uint32_t(REXCVAR_GET(masseffect_native_vertex_arena_min_bytes));
        va_max_undeclared_ = uint64_t(std::max(REXCVAR_GET(masseffect_native_vertex_arena_max_undeclared), 0));
        const uint64_t first = uint64_t(std::max<int32_t>(0, REXCVAR_GET(masseffect_native_vertex_arena_verify_n)));
        const uint64_t every =
            uint64_t(std::max<int32_t>(0, REXCVAR_GET(masseffect_native_vertex_arena_verify_every)));
        // With check 2 every hit is already a full comparison: no separate verification.
        va_verify_.Configure(true, va_check_ == 2 ? 0 : first, va_check_ == 2 ? 0 : every);
        va_full_at_store_ = va_check_ == 2 || first || every;
        va_created_ = va_.usable();
        va_on_ = va_created_;
        if (!va_on_) VaDestroy();
      } else {
        REXLOG_WARN("[native] vertex arena: could not create the {} MB buffer; off", size >> 20);
        VaDestroy();
      }
    }
    if (va_measure_ || va_on_) {
      // Before the first stamp either side takes: no event it relies on can be missed.
      tc::EnableConsumer(tc::kConsumerVertices, true);
      REXLOG_INFO("[native] vertex arena: measurement {}; arena {}{}",
                  va_measure_ ? "ON (full XXH3 of every copied range, report every 10 s)" : "off",
                  va_on_ ? "ON" : "off",
                  va_on_ ? fmt::format(" ({} MB in {} segments of {} KB, memory type {}, {}; check {}; ranges of {} "
                                       "bytes or more; first {} hits and then 1 in {} verified with a full hash; "
                                       "off after {} undeclared changes of reused ranges over {} KB)",
                                       va_.capacity() >> 20, me::native::vertex_arena::Arena::kSegments,
                                       va_.segment_bytes() >> 10, va_type_,
                                       va_coherent_ ? "coherent" : "flushed before submitting",
                                       va_check_ == 0   ? "stamp only"
                                       : va_check_ == 1 ? "stamp + sampled fingerprint"
                                                        : "stamp + full XXH3",
                                       va_min_bytes_, va_verify_.first(),
                                       va_check_ == 2 ? 0 : REXCVAR_GET(masseffect_native_vertex_arena_verify_every),
                                       va_max_undeclared_, kVaWholeFingerprint >> 10)
                         : std::string());
    }
  }

  void VaDestroy() {
    if (va_data_) dfn_.vkUnmapMemory(device_, va_memory_);
    va_data_ = nullptr;
    if (va_buffer_ != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, va_buffer_, nullptr);
    if (va_memory_ != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, va_memory_, nullptr);
    va_buffer_ = VK_NULL_HANDLE;
    va_memory_ = VK_NULL_HANDLE;
    va_on_ = false;
    va_created_ = false;
  }

  // The arena stops serving and storing for the rest of the session. Draws already recorded from it keep reading it
  // (the buffer lives until shutdown and nothing is written into it any more).
  void VaOff(const char* reason) {
    if (!va_on_) return;
    va_on_ = false;
    va_off_reason_ = reason;
    va_.DropAll();
    if (!va_measure_) me::native::texture_coherency::EnableConsumer(me::native::texture_coherency::kConsumerVertices, false);
    REXLOG_ERROR("[native] vertex arena: OFF for the rest of the session ({}); every range is copied as before", reason);
  }

  // --- Measurement ---------------------------------------------------------------------------------------------------
  // Every range the upload loop is about to copy (after the in-frame dedupe missed), whether or not the arena serves it.
  void VaMeasure(const uint8_t* data, uint64_t address, uint32_t bytes, uint32_t order, uint32_t stride) {
    namespace tc = me::native::texture_coherency;
    namespace va = me::native::vertex_arena;
    const uint32_t stamp = tc::Current();  // before the bytes are read (me_texture_coherency.h)
    const uint64_t hash = XXH3_64bits(data, bytes);
    const va::RangeKey key{uint32_t(address), bytes, stride, order};
    const va::Measure::Result r = va_measure_tab_->Note(
        key, hash, stamp, [](uint64_t start, uint64_t length, uint32_t s) { return tc::Clean(start, length, s); });
    // With the arena on as well, a write no event declared makes the range's pages exempt at once.
    if (r == va::Measure::kCleanChanged && va_on_ && va_exempt_.Mark(address, bytes)) ++va_c_.exempt_from_measure;
  }

  // --- Arena ---------------------------------------------------------------------------------------------------------
  /*
   * One binding whose range the in-frame dedupe did not have. kVaHit: `offset` is an arena copy of exactly these
   * bytes. kVaStored: the range was copied into the arena (instead of the upload buffer) at `offset`. kVaCopy: the
   * caller copies into the upload buffer as before. `fingerprint` is the dedupe's (0 = not taken yet); it is filled
   * when the arena needs it, with the same function, so the caller does not hash again.
   *
   * The rule for a hit: the entry's segment was not recycled, no page of the range has a coherency stamp newer than the
   * one taken right before the arena copy read the guest bytes, the pages were never exempted, and the content check
   * (masseffect_native_vertex_arena_check) agrees.
   */
  template <class FingerprintFn>
  int VaBinding(const uint8_t* data, uint64_t address, uint32_t bytes, xenos::Endian order, uint32_t stride,
                uint64_t& fingerprint, FingerprintFn&& fingerprint_of, VkDeviceSize& offset) {
    namespace tc = me::native::texture_coherency;
    namespace va = me::native::vertex_arena;
    ++va_c_.lookups;
    if (bytes < va_min_bytes_ || bytes > va_.max_bytes() || (bytes & 3)) {
      ++va_c_.skip_size;
      return kVaCopy;
    }
    if (va_exempt_.Any(address, bytes)) {
      ++va_c_.skip_exempt;
      return kVaCopy;
    }
    const auto content = [&]() -> uint64_t {
      if (!fingerprint) fingerprint = fingerprint_of();
      return fingerprint;
    };
    const va::RangeKey key{uint32_t(address), bytes, stride, uint32_t(order)};
    va::Arena::Entry& e = va_.Slot(key);
    if (!va::Arena::Same(e, key)) {
      // First sighting (or the slot held another range): remember the content, copy as before.
      e = va::Arena::Entry{};
      e.key = key;
      e.state = va::Arena::kCandidate;
      e.fingerprint = content();
      ++va_c_.first_seen;
      return kVaCopy;
    }
    if (va_.Live(e)) {
      if (tc::Clean(address, bytes, e.stamp)) {
        bool equal = true;
        if (va_check_ == 1) {
          equal = content() == e.fingerprint;
        } else if (va_check_ == 2) {
          equal = XXH3_64bits(data, bytes) == e.full;
        }
        if (equal) {
          if (va_verify_.Due()) {
            const uint64_t full = XXH3_64bits(data, bytes);
            if (!va_verify_.Result(full == e.full)) {
              REXLOG_ERROR("[native] DIFFERENCE: vertex arena: range {:08X} ({} bytes, stride {}, order {}) has no "
                           "coherency event since its arena copy and passed the {} check, but its full hash is "
                           "{:016X}, the copy was made from {:016X} ({} hits before)",
                           key.address, bytes, stride, key.order,
                           va_check_ == 0 ? "(no content)" : "sampled fingerprint", full, e.full, e.hits);
              VaOff("a verified hit differed from guest memory");
              return kVaCopy;
            }
            if (va_verify_.FirstChecksDone()) {
              REXLOG_INFO("[native] vertex arena: the first {} hits verified with a full hash, all equal",
                          va_verify_.first());
            }
          }
          va_.Use(e.segment);
          ++e.hits;
          ++va_c_.hits;
          va_c_.bytes_hit += bytes;
          offset = e.offset;
          return kVaHit;
        }
        VaUndeclared(e, key);
        return kVaCopy;
      }
      // An event touched the range since the copy. Same content: copied again (fresh stamp); otherwise it waits for a
      // repeat like a new range.
      if (content() != e.fingerprint) {
        ++va_c_.dirty_changed;
        e.state = va::Arena::kCandidate;
        e.fingerprint = fingerprint;
        e.hits = 0;
        return kVaCopy;
      }
      ++va_c_.dirty_same;
    } else if (content() != e.fingerprint) {
      // A candidate (or an entry whose segment was recycled) seen again with other bytes: not stable (yet).
      ++va_c_.content_changed;
      e.state = va::Arena::kCandidate;
      e.fingerprint = fingerprint;
      e.hits = 0;
      return kVaCopy;
    }
    return VaStore(e, data, bytes, order, offset);
  }

  // Copies the range into the arena. The stamp is taken before the copy reads the guest bytes, so a later "clean"
  // means no event after a moment that precedes every byte of the copy.
  int VaStore(me::native::vertex_arena::Arena::Entry& e, const uint8_t* data, uint32_t bytes, xenos::Endian order,
              VkDeviceSize& offset) {
    namespace tc = me::native::texture_coherency;
    namespace va = me::native::vertex_arena;
    uint32_t at = 0, segment = 0, epoch = 0;
    if (!va_.Allocate(bytes, 4, at, segment, epoch)) {
      ++va_c_.no_space;
      e.state = va::Arena::kCandidate;
      e.hits = 0;
      return kVaCopy;
    }
    const uint32_t stamp = tc::Current();
    e.full = va_full_at_store_ ? XXH3_64bits(data, bytes) : 0;
    CopyVertices({data, va_data_ + at, bytes / 4, order});
    e.state = va::Arena::kStored;
    e.stamp = stamp;
    e.offset = at;
    e.segment = segment;
    e.epoch = epoch;
    e.hits = 0;
    ++va_c_.stored;
    va_c_.bytes_stored += bytes;
    offset = at;
    return kVaStored;
  }

  // A live entry whose pages saw no event but whose content check failed: the game wrote it without declaring it.
  void VaUndeclared(me::native::vertex_arena::Arena::Entry& e, const me::native::vertex_arena::RangeKey& key) {
    ++va_c_.undeclared;
    const uint32_t pages = va_exempt_.Mark(key.address, key.bytes);
    // Only where the sampled check may have let an earlier reuse through with stale bytes: a range larger than what
    // the fingerprint reads whole, that had been reused. Small ranges were compared in full on every hit.
    const bool counted = va_check_ == 1 && key.bytes > kVaWholeFingerprint && e.hits > 0;
    const uint64_t n = counted ? ++va_c_.undeclared_counted : va_c_.undeclared_counted;
    if (va_c_.undeclared <= 32 || (va_c_.undeclared & 255) == 0 || counted) {
      REXLOG_INFO("[native] vertex arena: range {:08X} ({} bytes, stride {}, order {}) changed with no coherency "
                  "event after {} reuses; {} new exempt pages ({} undeclared changes so far, {} of reused ranges over "
                  "{} KB, {} allowed)",
                  key.address, key.bytes, key.stride, key.order, e.hits, pages, va_c_.undeclared, n,
                  kVaWholeFingerprint >> 10, va_max_undeclared_);
    }
    e.state = me::native::vertex_arena::Arena::kEmpty;
    if (counted && n > va_max_undeclared_) VaOff("too many undeclared changes of large reused ranges");
  }

  // UseSlot: the slot's previous submission has completed (BeginRecording waited its fence).
  void VaSlotStarted(uint32_t slot) {
    if (va_created_) va_.SlotStarted(slot, frame_);
  }

  // BeforeSend: publish what this submission wrote into the arena (non-coherent memory only).
  void VaFlush() {
    uint64_t lo = 0, hi = 0;
    if (!va_created_ || !va_.TakeDirty(lo, hi) || va_coherent_) return;
    rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, va_memory_, va_type_, lo, va_real_size_, hi - lo);
  }

  // --- Report, every 10 s (UseSlot) ----------------------------------------------------------------------------------
  void VaReport() {
    if (!va_measure_ && !va_created_) return;
    const auto now = std::chrono::steady_clock::now();
    if (va_report_ == std::chrono::steady_clock::time_point{}) {
      va_report_ = now;
      return;
    }
    if (now - va_report_ < std::chrono::seconds(10)) return;
    const double seconds = std::chrono::duration<double>(now - va_report_).count();
    va_report_ = now;
    namespace tc = me::native::texture_coherency;
    namespace va = me::native::vertex_arena;
    const uint64_t events = tc::g.events.load(std::memory_order_relaxed);
    const auto mb = [](uint64_t b) { return double(b) / 1048576.0; };
    if (va_measure_) {
      const va::Measure::Counts& c = va_measure_tab_->counts();
      const va::Measure::Counts& o = va_measure_reported_;
      std::array<uint64_t, va::Measure::kResults> n{}, b{};
      uint64_t total_n = 0, total_b = 0;
      for (uint32_t i = 0; i < va::Measure::kResults; ++i) {
        n[i] = c.n[i] - o.n[i];
        b[i] = c.bytes[i] - o.bytes[i];
        total_n += n[i];
        total_b += b[i];
      }
      MASSEFFECT_REPORT_RING(
          "[native] vertex arena measure (last {:.1f} s): ranges {} ({:.1f} MB copied) | first seen {} ({:.1f} MB) | "
          "clean+same {} ({:.1f} MB, what the arena would not copy) | clean+CHANGED {} ({:.1f} MB, must be 0 outside "
          "exempt ranges) | dirty+same {} ({:.1f} MB) | dirty+changed {} ({:.1f} MB) | coherency events {}, ranges "
          "tracked {}, table resets {}",
          seconds, total_n, mb(total_b), n[va::Measure::kFirst], mb(b[va::Measure::kFirst]),
          n[va::Measure::kCleanSame], mb(b[va::Measure::kCleanSame]), n[va::Measure::kCleanChanged],
          mb(b[va::Measure::kCleanChanged]), n[va::Measure::kDirtySame], mb(b[va::Measure::kDirtySame]),
          n[va::Measure::kDirtyChanged], mb(b[va::Measure::kDirtyChanged]), events - va_events_reported_,
          va_measure_tab_->tracked(), c.table_resets - o.table_resets);
      size_t distinct = 0;
      const std::vector<va::Measure::Offender> top = va_measure_tab_->TakeTop(5, distinct);
      if (!top.empty()) {
        std::string list;
        for (const va::Measure::Offender& t : top) {
          list += fmt::format("{}{:08X}+{} (stride {}, order {}) x{}", list.empty() ? "" : "; ", t.key.address,
                              t.key.bytes, t.key.stride, t.key.order, t.count);
        }
        MASSEFFECT_REPORT_RING("[native] vertex arena measure: top clean+CHANGED ranges ({} distinct): {}", distinct,
                               list);
      }
      va_measure_reported_ = c;
    }
    if (va_created_) {
      const VaCounters& c = va_c_;
      const VaCounters& o = va_reported_;
      MASSEFFECT_REPORT_RING(
          "[native] vertex arena (last {:.1f} s, {}): lookups {}, hits {} ({:.1f} MB not copied), stored {} ({:.1f} "
          "MB) | misses: first seen {}, content changed {}, dirty+same {} (stored again), dirty+changed {}, "
          "undeclared changes {} ({} counted, {} exempted by the measurement), too small/large {}, exempt pages {}, "
          "no space {} | verified {} (cumulative {}, {} differences) | segments recycled {}, waits for the GPU {}; "
          "{:.1f} of {} MB used, {} pages exempt",
          seconds, va_on_ ? "on" : (va_off_reason_ ? va_off_reason_ : "off"), c.lookups - o.lookups, c.hits - o.hits,
          mb(c.bytes_hit - o.bytes_hit), c.stored - o.stored, mb(c.bytes_stored - o.bytes_stored),
          c.first_seen - o.first_seen, c.content_changed - o.content_changed, c.dirty_same - o.dirty_same,
          c.dirty_changed - o.dirty_changed, c.undeclared - o.undeclared, c.undeclared_counted - o.undeclared_counted,
          c.exempt_from_measure - o.exempt_from_measure, c.skip_size - o.skip_size, c.skip_exempt - o.skip_exempt,
          c.no_space - o.no_space, va_verify_.checked() - va_verified_reported_, va_verify_.checked(),
          va_verify_.differences(), va_.recycles() - va_recycles_reported_, va_.waits() - va_waits_reported_,
          mb(std::min(va_.used(), va_.capacity())), va_.capacity() >> 20, va_exempt_.marked());
      va_reported_ = c;
      va_verified_reported_ = va_verify_.checked();
      va_recycles_reported_ = va_.recycles();
      va_waits_reported_ = va_.waits();
    }
    va_events_reported_ = events;
  }
