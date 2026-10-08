// Per-component draw cache (docs/frame-coherence.md, "Per-component cache"): the renderer side. Included inside
// DrawsVulkanImpl by masseffect_native_draws.cpp, after CacheSampler and PipelineKey. The logic is in me_draw_cache.h,
// the settings in me_draw_cache_cvars.inc. Hooks in Draw(): DcTexturesBefore/After around the sampler loop,
// DcPipelineFor instead of PipelineFor(SearchKey(...)), DcIndicesLookup/Store/Hit around the 16-bit index cache, the
// index buffer bind; and Initialize (DcInit), UseSlot (DcSlotStarted, DcReport), BeforeSend (DcFlush), the destructor
// (DcDestroy).

  // --- 1. Texture bindings -------------------------------------------------------------------------------------------
  /*
   * Key: the pixel shader (library entry), its sampler registers (< 16) in order and the 6 fetch words of each.
   * Value: the per-register cache entry (CacheSampler) each register had after the sampler loop, i.e. the slot, heap
   * and sampler indices and host size the draw used, with the invalidation fields of where they came from.
   *
   * A hit does not write the shared constants itself: it primes cache_samplers_ with the stored entries and lets the
   * sampler loop run, where each sampler then takes the per-register cache hit. So the bindings go through exactly the
   * code that writes them today (and through any later change of it, e.g. a combined image+sampler heap), and the hit
   * is only taken when every binding is valid by the rule of that cache: frame horizon, GenerationValid (global,
   * image and address-bucket generations, resolved entries) and VerifyDue (the copy / address / frames checks that
   * still want PrepareTexture). What it skips: the per-fetch table (XXH3 + a cold line) and the PrepareTexture of
   * fetches whose per-fetch entry was evicted by another fetch.
   */
  static constexpr size_t kDcTexMax = 8;
  using DcTexSets = me::native::draw_cache::TextureSets<CacheSampler, kDcTexMax, 512>;
  struct DcTexState {
    bool active = false;     // key made: After() stores (or checks)
    bool hit = false;        // cache_samplers_ primed from the entry
    bool verifying = false;  // a hit checked against the loop: dc_tex_ref_ holds the cached values
    DcTexSets::Key key;
  };
  // Not in DcTexState: 8 CacheSampler are ~800 bytes to construct on every draw.
  std::array<CacheSampler, kDcTexMax> dc_tex_ref_{}, dc_tex_used_{};
  std::unique_ptr<DcTexSets> dc_tex_;
  me::native::draw_cache::Verifier dc_tex_verify_;
  me::native::draw_cache::Timing dc_tex_time_;
  // Misses by cause: no entry, slot clash, a binding no longer valid, a binding a texture-cache check wants, too many
  // samplers.
  std::array<uint64_t, 5> dc_tex_misses_{};
  uint64_t dc_tex_bindings_ = 0;  // bindings primed

  // Before the sampler loop: on a hit, primes cache_samplers_ (st.hit); the loop runs in every case.
  void DcTexturesBefore(const ShaderEntry* ps, const uint32_t* r, DcTexState& st) {
    if (!dc_tex_ || !dc_tex_verify_.on() || !ps || ps->samplers.empty()) return;
    uint8_t registers[kDcTexMax];
    uint32_t n = 0;
    for (const SamplerShader& sampler : ps->samplers) {
      if (sampler.register_value >= 16) continue;  // the loop skips it too
      if (n == kDcTexMax) {
        ++dc_tex_misses_[4];
        return;
      }
      registers[n++] = uint8_t(sampler.register_value);
    }
    if (!n || !DcTexSets::MakeKey(ps->number + 1, registers, n, r + kRegFetch, st.key)) return;
    st.active = true;
    bool clash = false;
    const DcTexSets::Entry* e = dc_tex_->Find(st.key, clash);
    if (!e) {
      ++dc_tex_misses_[clash ? 1 : 0];
      return;
    }
    for (uint32_t i = 0; i < n; ++i) {
      const CacheSampler& b = e->bindings[i];
      if (!(cache_between_frames_ ? frame_ <= b.valid_until : b.frame == frame_) || !GenerationValid(b)) {
        ++dc_tex_misses_[2];
        return;
      }
      if (VerifyDue(b)) {
        ++dc_tex_misses_[3];
        return;
      }
    }
    if (dc_tex_verify_.Due()) {
      st.verifying = true;
      std::copy_n(e->bindings.begin(), n, dc_tex_ref_.begin());
      return;
    }
    for (uint32_t i = 0; i < n; ++i) cache_samplers_[st.key.registers[i]] = e->bindings[i];
    st.hit = true;
    dc_tex_bindings_ += n;
  }

  // After the sampler loop finished (not reached when it rejected the draw).
  void DcTexturesAfter(DcTexState& st) {
    if (!st.active || st.hit) return;
    std::array<CacheSampler, kDcTexMax>& used = dc_tex_used_;
    for (uint32_t i = 0; i < st.key.n; ++i) used[i] = cache_samplers_[st.key.registers[i]];
    if (st.verifying) {
      bool equal = true;
      uint32_t at = 0;
      for (uint32_t i = 0; i < st.key.n && equal; ++i) {
        const CacheSampler& a = dc_tex_ref_[i];
        const CacheSampler& b = used[i];
        equal = a.slot == b.slot && a.heap == b.heap && a.sampler == b.sampler && a.width == b.width &&
                a.height == b.height;
        at = i;
      }
      if (!dc_tex_verify_.Result(equal)) {
        const CacheSampler& a = dc_tex_ref_[at];
        const CacheSampler& b = used[at];
        REXLOG_ERROR("[native] DIFFERENCE: draw cache textures: register {} (fetch {:08X} {:08X} {:08X}) cached slot "
                     "{:08X} heap {} sampler {} {}x{}, the sampler loop gives slot {:08X} heap {} sampler {} {}x{}; "
                     "the texture component is off from now on",
                     st.key.registers[at], b.fetch[0], b.fetch[1], b.fetch[2], a.slot, a.heap, a.sampler, a.width,
                     a.height, b.slot, b.heap, b.sampler, b.width, b.height);
        dc_tex_->Clear();
        return;
      }
      if (dc_tex_verify_.FirstChecksDone())
        REXLOG_INFO("[native] C6 draw cache: textures: the first {} hits checked against the sampler loop, all equal",
                    dc_tex_verify_.first());
    }
    if (dc_tex_verify_.on()) dc_tex_->Store(st.key, used.data());
  }

  // --- 2. Pipelines --------------------------------------------------------------------------------------------------
  /*
   * Key: the raw PipelineKey the draw built and the lookup context (dynamic-state mode, canonical key on/off), which
   * are everything SearchKey reads. Value: the pipeline, stored only when PipelineFor found it in the pipeline map or
   * its caches (last_key_ equal to the lookup key afterwards): never a generic pipeline standing in for a specialized
   * one still compiling, never a failed creation. Map entries are never replaced or destroyed during the session.
   */
  using DcPipeMemo = me::native::draw_cache::PipelineMemo<PipelineKey, VkPipeline, 256>;
  std::unique_ptr<DcPipeMemo> dc_pipe_;
  me::native::draw_cache::Verifier dc_pipe_verify_;
  me::native::draw_cache::Timing dc_pipe_time_;
  std::array<uint64_t, 3> dc_pipe_misses_{};  // empty slot, clash, result not final (not stored)

  uint32_t DcPipelineContext() const { return eds_mode_ | (canonical_key_ ? 0x100u : 0u); }

  VkPipeline DcPipelineFor(const PipelineKey& key, const VerticesEntry& entry, const SubmissionDraw& p) {
    if (!dc_pipe_ || !dc_pipe_verify_.on()) return PipelineFor(SearchKey(key), entry, p);
    const auto t0 = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    bool clash = false;
    const VkPipeline* cached = dc_pipe_->Find(key, DcPipelineContext(), clash);
    VkPipeline pipeline = VK_NULL_HANDLE;
    const bool hit = cached && !dc_pipe_verify_.Due();
    if (hit) {
      pipeline = *cached;
    } else {
      const VkPipeline want = cached ? *cached : VK_NULL_HANDLE;
      if (!cached) ++dc_pipe_misses_[clash ? 1 : 0];
      const PipelineKey search = SearchKey(key);
      pipeline = PipelineFor(search, entry, p);
      if (cached) {
        if (!dc_pipe_verify_.Result(want == pipeline)) {
          REXLOG_ERROR("[native] DIFFERENCE: draw cache pipelines: VS {} PS {} specialization {:08X}: cached pipeline "
                       "{:016X}, the lookup gives {:016X}; the pipeline component is off from now on",
                       key.vs, key.ps, key.specialization, std::bit_cast<uint64_t>(want),
                       std::bit_cast<uint64_t>(pipeline));
          dc_pipe_->Clear();
        } else if (dc_pipe_verify_.FirstChecksDone()) {
          REXLOG_INFO("[native] C6 draw cache: pipelines: the first {} hits checked against the lookup, all equal",
                      dc_pipe_verify_.first());
        }
      }
      // The context after SearchKey: a failed canonical-key check switches it off during this very call.
      if (dc_pipe_verify_.on() && pipeline != VK_NULL_HANDLE && last_valid_key_ && last_pipeline_ == pipeline &&
          Equal(&last_key_, &search, sizeof(search))) {
        dc_pipe_->Store(key, DcPipelineContext(), pipeline);
      } else {
        ++dc_pipe_misses_[2];
      }
    }
    if (time_) {
      dc_pipe_time_.Add(hit, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - t0).count()));
    }
    return pipeline;
  }

  // --- 4. Persistent index arena -------------------------------------------------------------------------------------
  /*
   * 16-bit index ranges of the index cache's fast path (no restart, no offset, no quads/fans, 16-bit, up to 16 KB so
   * they carry a full RangeFingerprint) are converted once into a persistent host-visible buffer. A later draw (any
   * frame) whose range has the same address, byte order, count and fingerprint binds that buffer at the stored offset
   * and uses the stored min/max: no conversion, no copy into the per-frame upload buffer. The fingerprint is computed
   * on every draw as before (the guest may rewrite indices at any time); only the conversion and the copy are skipped.
   * The arena is never written where a frame still on the GPU may read (me_draw_cache.h, IndexArena).
   */
  struct DcIndexState {
    bool hit = false;    // offset is valid: bind the arena
    bool store = false;  // a miss the arena can take: converted indices go there instead of the upload buffer
    uint32_t offset = 0;
    uint64_t key = 0, fingerprint = 0;
    uint32_t count = 0;
  };
  me::native::draw_cache::IndexArena dc_idx_;
  me::native::draw_cache::Verifier dc_idx_verify_;
  bool dc_idx_on_ = false;
  VkBuffer dc_idx_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory dc_idx_memory_ = VK_NULL_HANDLE;
  uint32_t dc_idx_type_ = 0;
  VkDeviceSize dc_idx_real_size_ = 0;
  bool dc_idx_coherent_ = true;
  uint8_t* dc_idx_data_ = nullptr;
  uint64_t dc_idx_hits_ = 0, dc_idx_bytes_hit_ = 0, dc_idx_stored_ = 0, dc_idx_bytes_stored_ = 0;
  uint64_t dc_idx_lookups_ = 0, dc_idx_no_space_ = 0, dc_idx_waiting_ = 0, dc_idx_first_seen_ = 0;
  VkBuffer indices_buffer_recorded_ = VK_NULL_HANDLE;  // the buffer of the last vkCmdBindIndexBuffer (with its type)

  // Stage 1, inside the index cache's fast path. True = the caller must not convert (the arena answered with min/max,
  // or a checked hit already converted into `output`).
  bool DcIndicesLookup(uint64_t key, uint32_t count, uint64_t fingerprint, const uint8_t* data, bool rotate,
                       uint16_t* output, uint32_t& min, uint32_t& max, DcIndexState& st) {
    st = {};
    if (!dc_idx_on_ || postponed_sky_ || !fingerprint || !dc_idx_verify_.on()) return false;
    ++dc_idx_lookups_;
    if (!dc_idx_.usable()) {
      ++dc_idx_waiting_;
      return false;
    }
    st.key = key;
    st.count = count;
    st.fingerprint = fingerprint;
    const me::native::draw_cache::IndexArena::Entry* e = dc_idx_.Find(key, count, fingerprint);
    if (!e) {
      st.store = dc_idx_.SeenBefore(key, count, fingerprint);  // promotion: the second sighting is stored
      dc_idx_first_seen_ += st.store ? 0 : 1;
      return false;
    }
    if (!dc_idx_verify_.Due()) {
      min = e->vmin;
      max = e->vmax;
      st.hit = true;
      st.offset = e->offset;
      return true;
    }
    uint32_t mn = 0xFFFF, mx = 0;
    IndicesFrom16(data, count, output, rotate, mn, mx);
    const bool equal = mn == e->vmin && mx == e->vmax &&
                       std::memcmp(dc_idx_data_ + e->offset, output, size_t(count) * 2) == 0;
    min = mn;
    max = mx;
    if (!dc_idx_verify_.Result(equal)) {
      REXLOG_ERROR("[native] DIFFERENCE: draw cache indices: range {:08X} ({} indices, fingerprint {:016X}): arena "
                   "min/max {}/{} at offset {}, conversion gives {}/{}{}; the index component is off from now on",
                   uint32_t(key >> 2), count, fingerprint, e->vmin, e->vmax, e->offset, mn, mx,
                   mn == e->vmin && mx == e->vmax ? " and other indices" : "");
      dc_idx_.Reset();
      return true;  // converted: the draw takes the upload buffer
    }
    if (dc_idx_verify_.FirstChecksDone())
      REXLOG_INFO("[native] C6 draw cache: indices: the first {} arena hits checked against the conversion, all equal",
                  dc_idx_verify_.first());
    st.hit = true;
    st.offset = e->offset;
    return true;
  }

  // Stage 4: the arena offset of a hit.
  VkDeviceSize DcIndicesHit(const DcIndexState& st) {
    ++dc_idx_hits_;
    dc_idx_bytes_hit_ += uint64_t(st.count) * 2;
    return st.offset;
  }

  // Stage 4: a miss the arena can take. False = no space (full: the arena starts waiting for the GPU) or switched
  // off: the caller uploads as before.
  bool DcIndicesStore(const DcIndexState& st, const uint16_t* indices, VkDeviceSize bytes, uint32_t vmin,
                      uint32_t vmax, VkDeviceSize& offset) {
    if (!st.store || !dc_idx_verify_.on() || bytes != VkDeviceSize(st.count) * 2) return false;
    uint32_t at = 0;
    if (!dc_idx_.Allocate(uint32_t(bytes), 4, at)) {
      ++dc_idx_no_space_;
      return false;
    }
    std::memcpy(dc_idx_data_ + at, indices, size_t(bytes));
    dc_idx_.Insert(st.key, st.count, st.fingerprint, vmin, vmax, at);
    ++dc_idx_stored_;
    dc_idx_bytes_stored_ += bytes;
    offset = at;
    return true;
  }

  // UseSlot: the slot's previous submission has completed (BeginRecording waited its fence).
  void DcSlotStarted(uint32_t slot) {
    if (dc_idx_on_) dc_idx_.SlotStarted(slot, frame_);
  }

  // BeforeSend: publish what this submission wrote into the arena (non-coherent memory only).
  void DcFlush() {
    uint64_t lo = 0, hi = 0;
    if (!dc_idx_on_ || !dc_idx_.TakeDirty(lo, hi) || dc_idx_coherent_) return;
    rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, dc_idx_memory_, dc_idx_type_, lo, dc_idx_real_size_,
                                                  hi - lo);
  }

  // --- Set-up, report, shutdown --------------------------------------------------------------------------------------
  void DcInit() {
    const uint64_t first = uint64_t(std::max<int32_t>(0, REXCVAR_GET(masseffect_native_draw_cache_verify_n)));
    const uint64_t every = uint64_t(std::max<int32_t>(0, REXCVAR_GET(masseffect_native_draw_cache_verify_every)));
    if (REXCVAR_GET(masseffect_native_draw_cache_textures)) {
      dc_tex_ = std::make_unique<DcTexSets>();
      dc_tex_verify_.Configure(true, first, every);
    }
    if (REXCVAR_GET(masseffect_native_draw_cache_pipelines)) {
      dc_pipe_ = std::make_unique<DcPipeMemo>();
      dc_pipe_verify_.Configure(true, first, every);
    }
    if (REXCVAR_GET(masseffect_native_draw_cache_indices)) {
      if (!REXCVAR_GET(masseffect_native_indices_cache)) {
        REXLOG_WARN("[native] C6 draw cache: the index arena needs masseffect_native_indices_cache = true; off");
      } else {
        const VkDeviceSize size = VkDeviceSize(REXCVAR_GET(masseffect_native_draw_cache_indices_mb)) << 20;
        void* mapped = nullptr;
        if (rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
                vulkan_device_, size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, rex::ui::vulkan::util::MemoryPurpose::kUpload,
                dc_idx_buffer_, dc_idx_memory_, &dc_idx_type_, &dc_idx_real_size_) &&
            dfn_.vkMapMemory(device_, dc_idx_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
          dc_idx_data_ = static_cast<uint8_t*>(mapped);
          dc_idx_coherent_ = (vulkan_device_->memory_types().host_coherent >> dc_idx_type_) & 0x1;
          dc_idx_.Configure(size, 4096);
          dc_idx_verify_.Configure(true, first, every);
          dc_idx_on_ = true;
        } else {
          REXLOG_WARN("[native] C6 draw cache: could not create the {} MB index arena; off", size >> 20);
          DcDestroy();
        }
      }
    }
    if (dc_tex_ || dc_pipe_ || dc_idx_on_)
      REXLOG_INFO("[native] C6 draw cache: textures {}, pipelines {}, index arena {}{}; the first {} hits of each and "
                  "then 1 in {} are checked against the path they replace",
                  dc_tex_ ? "ON" : "off", dc_pipe_ ? "ON" : "off", dc_idx_on_ ? "ON" : "off",
                  dc_idx_on_ ? fmt::format(" ({} MB, memory type {}, {})", dc_idx_.capacity() >> 20, dc_idx_type_,
                                           dc_idx_coherent_ ? "coherent" : "flushed before submitting")
                             : std::string(),
                  first, every);
  }

  void DcDestroy() {
    if (dc_idx_data_) dfn_.vkUnmapMemory(device_, dc_idx_memory_);
    dc_idx_data_ = nullptr;
    if (dc_idx_buffer_ != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, dc_idx_buffer_, nullptr);
    if (dc_idx_memory_ != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, dc_idx_memory_, nullptr);
    dc_idx_buffer_ = VK_NULL_HANDLE;
    dc_idx_memory_ = VK_NULL_HANDLE;
    dc_idx_on_ = false;
  }

  struct DcReported {
    me::native::draw_cache::Timing tex, pipe;
    uint64_t tex_hits = 0, tex_checked = 0, tex_bindings = 0;
    std::array<uint64_t, 5> tex_misses{};
    uint64_t pipe_hits = 0, pipe_checked = 0, pipe_last = 0;
    std::array<uint64_t, 3> pipe_misses{};
    uint64_t idx_lookups = 0, idx_hits = 0, idx_bytes_hit = 0, idx_stored = 0, idx_bytes_stored = 0, idx_no_space = 0,
             idx_waiting = 0, idx_checked = 0, idx_resets = 0, idx_first_seen = 0;
  };
  DcReported dc_reported_;
  std::chrono::steady_clock::time_point dc_report_{};

  static std::string DcTiming(const me::native::draw_cache::Timing& c, const me::native::draw_cache::Timing& o) {
    const uint64_t nh = c.n_hit - o.n_hit, ns = c.n_slow - o.n_slow;
    return fmt::format("us per timed call: hit {:.2f} ({}), slow {:.2f} ({})",
                       nh ? double(c.ns_hit - o.ns_hit) / 1e3 / double(nh) : 0.0, nh,
                       ns ? double(c.ns_slow - o.ns_slow) / 1e3 / double(ns) : 0.0, ns);
  }

  // Every 10 s (from UseSlot), only with a component on.
  void DcReport() {
    if (!dc_tex_ && !dc_pipe_ && dc_idx_buffer_ == VK_NULL_HANDLE) return;
    const auto now = std::chrono::steady_clock::now();
    if (dc_report_ == std::chrono::steady_clock::time_point{}) {
      dc_report_ = now;
      return;
    }
    if (now - dc_report_ < std::chrono::seconds(10)) return;
    const double seconds = std::chrono::duration<double>(now - dc_report_).count();
    dc_report_ = now;
    DcReported c;
    c.tex = dc_tex_time_;
    c.pipe = dc_pipe_time_;
    c.tex_hits = dc_tex_verify_.hits();
    c.tex_checked = dc_tex_verify_.checked();
    c.tex_bindings = dc_tex_bindings_;
    c.tex_misses = dc_tex_misses_;
    c.pipe_hits = dc_pipe_verify_.hits();
    c.pipe_checked = dc_pipe_verify_.checked();
    c.pipe_last = dc_pipe_ ? dc_pipe_->last_hits() : 0;
    c.pipe_misses = dc_pipe_misses_;
    c.idx_lookups = dc_idx_lookups_;
    c.idx_hits = dc_idx_hits_;
    c.idx_bytes_hit = dc_idx_bytes_hit_;
    c.idx_stored = dc_idx_stored_;
    c.idx_bytes_stored = dc_idx_bytes_stored_;
    c.idx_no_space = dc_idx_no_space_;
    c.idx_waiting = dc_idx_waiting_;
    c.idx_checked = dc_idx_verify_.checked();
    c.idx_resets = dc_idx_.resets();
    c.idx_first_seen = dc_idx_first_seen_;
    const DcReported& o = dc_reported_;
    MASSEFFECT_REPORT_RING(
        "[native] C6 draw cache (last {:.1f} s) | textures {}: {} set hits ({} bindings primed), misses: {} no entry, "
        "{} clash, {} not valid, {} wanted by a texture check, {} too many samplers; {} checked, {} differences; {} | "
        "pipelines {}: {} hits ({} by the last-key shortcut), misses: {} empty, {} clash, {} not stored (not final); "
        "{} checked, {} differences; {} | index arena {}: {} lookups, {} hits ({:.1f} MB neither converted nor "
        "copied), {} seen for the first time (not stored), {} stored ({:.1f} MB), {} without space, {} while "
        "waiting for the GPU, {} resets, {:.1f} of {} MB used; {} checked, {} differences",
        seconds, dc_tex_ ? (dc_tex_verify_.on() ? "on" : "OFF (difference)") : "off", c.tex_hits - o.tex_hits,
        c.tex_bindings - o.tex_bindings, c.tex_misses[0] - o.tex_misses[0], c.tex_misses[1] - o.tex_misses[1],
        c.tex_misses[2] - o.tex_misses[2], c.tex_misses[3] - o.tex_misses[3], c.tex_misses[4] - o.tex_misses[4],
        c.tex_checked - o.tex_checked, dc_tex_verify_.differences(), DcTiming(c.tex, o.tex),
        dc_pipe_ ? (dc_pipe_verify_.on() ? "on" : "OFF (difference)") : "off", c.pipe_hits - o.pipe_hits,
        c.pipe_last - o.pipe_last, c.pipe_misses[0] - o.pipe_misses[0], c.pipe_misses[1] - o.pipe_misses[1],
        c.pipe_misses[2] - o.pipe_misses[2], c.pipe_checked - o.pipe_checked, dc_pipe_verify_.differences(),
        DcTiming(c.pipe, o.pipe),
        dc_idx_on_ ? (dc_idx_verify_.on() ? "on" : "OFF (difference)") : "off", c.idx_lookups - o.idx_lookups,
        c.idx_hits - o.idx_hits, double(c.idx_bytes_hit - o.idx_bytes_hit) / 1048576.0, c.idx_first_seen - o.idx_first_seen,
        c.idx_stored - o.idx_stored,
        double(c.idx_bytes_stored - o.idx_bytes_stored) / 1048576.0, c.idx_no_space - o.idx_no_space,
        c.idx_waiting - o.idx_waiting, c.idx_resets - o.idx_resets, double(dc_idx_.used()) / 1048576.0,
        dc_idx_.capacity() >> 20, c.idx_checked - o.idx_checked, dc_idx_verify_.differences());
    dc_reported_ = c;
  }
