// Cold-start pipeline hitches (docs/cold-start-hitches.md): members of DrawsVulkanImpl, included inside the class
// by masseffect_native_draws.cpp. The settings are in masseffect_pipelines_cold_cvars.inc.
//
// 1. Specialized pipelines compiled in the background (masseffect_native_pipelines_async_specialized).
// 2. The shipped prewarm list (masseffect_native_pipelines_shipped_list), merged by LoadPipelinesList.
// 3. Helpers of the chunked pipelines file write (masseffect_native_pipelines_save_*).

  // --- 1. Specialized pipelines compiled in the background --------------------------------------------------
  static constexpr uint32_t kSpecFoldedBits = kSpecVsFolded | kSpecSignsFolded;

  // The key without its folded values: exactly the key the draw builds with both foldings off. Its pipeline reads
  // the same values from the constants the draw uploads anyway, so it draws the same image. The pixel shader
  // rewrites A and B (kKeyPsFltBounds, kKeyPsCombined) stay: the draw wrote its shared block for them. Keys with
  // kKeyPsNoMask are never deferred (the generic module would read the cleared sign bytes).
  static PipelineKey GenericKey(const PipelineKey& key) {
    PipelineKey g = key;
    g.specialization &= ~kSpecFoldedBits;
    g.signs_low = 0;
    g.signs_high = 0;
    g.signs_heaps = key.signs_heaps & (kKeyPsFltBounds | kKeyPsCombined);
    std::fill(std::begin(g.vs_values), std::end(g.vs_values), 0u);
    g.vs_fold = 0;
    return g;
  }

  struct AsyncPipelineJob {
    PipelineKey key;
    uint64_t fingerprint = 0;
    VerticesEntry entry;  // a copy: CreatePipelineVulkan reads its attributes and bindings
    VkShaderModule vs = VK_NULL_HANDLE;  // modules live until the renderer is destroyed
    VkShaderModule ps = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;  // pass_rp_ of the draw (render passes live until the end, too)
    const ShaderEntry* vs_entry = nullptr;  // library entries, for the prewarm list
    const ShaderEntry* ps_entry = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    uint64_t ns = 0;
  };
  enum AsyncDecision : uint8_t { kAsyncNo = 0, kAsyncCreated = 1, kAsyncFallback = 2 };

  // PipelineFor, right after the map misses: a specialized key whose pipeline is still compiling (or failed to
  // compile in the background) is drawn with the generic pipeline. False = continue as usual.
  bool AsyncSpecializedPending(const PipelineKey& key, const VerticesEntry& entry, const SubmissionDraw& p,
                               uint64_t fingerprint, VkPipeline& out) {
    if (async_pending_.empty() && async_failed_.empty()) return false;
    if (!(key.specialization & kSpecFoldedBits) || (key.signs_heaps & kKeyPsNoMask)) return false;
    if (const auto it = async_pending_.find(fingerprint); it != async_pending_.end()) {
      if (!Equal(&it->second, &key, sizeof(key))) return false;
    } else if (!async_failed_.count(fingerprint)) {
      return false;
    }
    const VkPipeline generic = PipelineFor(GenericKey(key), entry, p);
    if (generic == VK_NULL_HANDLE) return false;  // the usual path compiles it (the late result is discarded)
    ++async_fallback_draws_;
    out = generic;
    return true;
  }

  // PipelineFor, with the modules ready and before compiling: decides whether this specialized pipeline is compiled
  // now (kAsyncNo), was found in the Vulkan cache (kAsyncCreated, `pipeline` set: the usual path keeps it) or goes to
  // the background thread (kAsyncFallback, `pipeline` = the generic one, not stored under this key).
  AsyncDecision AsyncSpecializedDefer(const PipelineKey& key, const VerticesEntry& entry, const SubmissionDraw& p,
                                      VkShaderModule vs, VkShaderModule ps, uint64_t fingerprint, VkPipeline& pipeline,
                                      uint32_t& n_colors) {
    const int32_t mode = REXCVAR_GET(masseffect_native_pipelines_async_specialized);
    if (mode <= 0 || !(key.specialization & kSpecFoldedBits) || (key.signs_heaps & kKeyPsNoMask) || async_no_thread_ ||
        async_pending_.count(fingerprint)) {
      return kAsyncNo;
    }
    if (REXCVAR_GET(masseffect_native_pipelines_async_probe)) {
      VkPipeline probe = VK_NULL_HANDLE;
      const auto t0 = std::chrono::steady_clock::now();
      const VkResult result = CreatePipelineVulkan(key, entry, vs, ps, pass_rp_, true, probe, n_colors,
                                                   VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
      ns_async_probe_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - t0).count());
      ++async_probes_;
      if (result == VK_SUCCESS && probe != VK_NULL_HANDLE) {
        ++async_probe_hits_;
        pipeline = probe;
        return kAsyncCreated;  // in the Vulkan cache: used at once, exactly as before
      }
      if (probe != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, probe, nullptr);
      if (result != VK_PIPELINE_COMPILE_REQUIRED) {
        ++async_probe_errors_;
        return kAsyncNo;
      }
    }
    const PipelineKey generic = GenericKey(key);
    if (mode == 1) {
      const auto it = pipelines_.find(XXH3_64bits(&generic, sizeof(generic)));
      if (it == pipelines_.end() || it->second.second == VK_NULL_HANDLE ||
          !Equal(&it->second.first, &generic, sizeof(generic))) {
        ++async_no_generic_;
        return kAsyncNo;
      }
    }
    const auto t0 = std::chrono::steady_clock::now();
    const VkPipeline fallback = PipelineFor(generic, entry, p);  // never deferred itself: it has no folded bits
    ns_async_generic_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - t0).count());
    if (fallback == VK_NULL_HANDLE || !EnqueueAsyncSpecialized(key, fingerprint, entry, p, vs, ps)) {
      return kAsyncNo;
    }
    async_pending_.emplace(fingerprint, key);
    ++async_deferred_;
    pipeline = fallback;
    return kAsyncFallback;
  }

  bool EnqueueAsyncSpecialized(const PipelineKey& key, uint64_t fingerprint, const VerticesEntry& entry,
                               const SubmissionDraw& p, VkShaderModule vs, VkShaderModule ps) {
    if (!async_thread_.joinable()) {
      const int ring_core = RexSwitchCurrentCore();
      try {
        async_thread_ = std::thread([this, ring_core] { AsyncSpecializedLoop(ring_core); });
      } catch (const std::system_error& error) {
        async_no_thread_ = true;
        REXLOG_WARN("[native] C6 async pipelines: could not create the thread ({}); compiled on the ring",
                    error.what());
        return false;
      }
    }
    AsyncPipelineJob job;
    job.key = key;
    job.fingerprint = fingerprint;
    job.entry.attributes = entry.attributes;
    job.entry.bindings = entry.bindings;
    job.vs = vs;
    job.ps = ps;
    job.pass = pass_rp_;
    job.vs_entry = p.vs;
    job.ps_entry = key.ps ? p.ps : nullptr;
    {
      std::lock_guard<std::mutex> lock(async_mutex_);
      async_queue_.push_back(std::move(job));
    }
    async_wake_.notify_one();
    return true;
  }

  // The background thread: compiles the queued pipelines one by one with the same function as the ring.
  void AsyncSpecializedLoop(int ring_core) {
    rex::thread::set_current_thread_name("MASSEFFECT pipeline async");
    int32_t priority = -1;
    const int32_t wanted = REXCVAR_GET(masseffect_native_pipelines_async_priority);
    if (wanted >= 0 && RexSwitchSetCurrentThreadPriorityOk(int(wanted))) {
      priority = wanted;
    } else {
      for (const int32_t candidate : {0x3F, 0x3E, 0x3D, 0x3C, 0x3B}) {
        if (RexSwitchSetCurrentThreadPriorityOk(int(candidate))) {
          priority = candidate;
          break;
        }
      }
    }
    const int32_t request = REXCVAR_GET(masseffect_native_pipelines_async_core);
    const int core = request == -2 ? (ring_core == 2 ? 1 : 2) : int(request);
    const bool core_ok = core < 0 || RexSwitchSetCurrentThreadCore(core);
    REXLOG_INFO("[native] C6 async pipelines: thread at priority {:#x}{}; preferred core {}{} (the ring runs on "
                "core {}), now on core {}",
                uint32_t(priority), priority < 0 ? " (none accepted: keeps the one from its creation)" : "", core,
                core_ok ? "" : " (NOT accepted)", ring_core, RexSwitchCurrentCore());
    for (;;) {
      AsyncPipelineJob job;
      {
        std::unique_lock<std::mutex> lock(async_mutex_);
        async_wake_.wait(lock, [this] { return async_stop_ || !async_queue_.empty(); });
        if (async_stop_) return;
        job = std::move(async_queue_.front());
        async_queue_.pop_front();
      }
      uint32_t n_colors = 0;
      const auto t0 = std::chrono::steady_clock::now();
      job.result = CreatePipelineVulkan(job.key, job.entry, job.vs, job.ps, job.pass, false, job.pipeline, n_colors);
      job.ns = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
      {
        std::lock_guard<std::mutex> lock(async_mutex_);
        async_done_.push_back(std::move(job));
      }
      async_done_count_.fetch_add(1, std::memory_order_release);
    }
  }

  // Ring, once per frame: the finished pipelines go into the map under their specialized key, so the next draw
  // uses them; and the 10 s report.
  void AsyncSpecializedPerFrame(std::chrono::steady_clock::time_point now) {
    if (async_done_count_.load(std::memory_order_acquire) != async_collected_) {
      std::deque<AsyncPipelineJob> done;
      {
        std::lock_guard<std::mutex> lock(async_mutex_);
        done.swap(async_done_);
      }
      for (AsyncPipelineJob& job : done) {
        ++async_collected_;
        async_pending_.erase(job.fingerprint);
        ns_async_worker_ += job.ns;
        if (job.result != VK_SUCCESS || job.pipeline == VK_NULL_HANDLE) {
          if (job.pipeline != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, job.pipeline, nullptr);
          async_failed_.insert(job.fingerprint);  // drawn with the generic pipeline from now on
          ++async_failures_;
          continue;
        }
        if (pipelines_.count(job.fingerprint)) {  // the ring had to create it meanwhile
          dfn_.vkDestroyPipeline(device_, job.pipeline, nullptr);
          ++async_duplicates_;
          continue;
        }
        pipelines_.emplace(job.fingerprint, std::make_pair(job.key, job.pipeline));
        ++pipelines_per_mode_[job.key.fill2 & 3];
        ++pipelines_no_save_;
        ++async_installed_;
        SubmissionDraw p;
        p.vs = job.vs_entry;
        p.ps = job.ps_entry;
        NotePipelineCreated(job.key, job.entry, p, true, 0);  // the prewarm list (0 ms: not the ring's time)
      }
    }
    if (now - async_report_ < std::chrono::seconds(10)) return;
    async_report_ = now;
    const uint64_t changes = async_deferred_ + async_collected_ + async_probes_ + async_fallback_draws_;
    if (changes == async_report_changes_) return;
    async_report_changes_ = changes;
    const auto average = [](uint64_t ns, uint64_t n) { return n ? double(ns) / double(n) / 1e6 : 0.0; };
    MASSEFFECT_REPORT_RING(
        "[native] C6 async pipelines (mode {}): {} specialized deferred, {} pending, {} installed, {} failed, {} "
        "duplicates; background compile {:.1f} ms average | {} draws with the generic pipeline | cache probe: {} "
        "asked, {} hits, {} errors ({:.2f} ms average) | generic pipelines asked on the ring {:.0f} ms in total; "
        "{} not deferred for lack of a generic one (mode 1)",
        REXCVAR_GET(masseffect_native_pipelines_async_specialized), async_deferred_, async_pending_.size(),
        async_installed_, async_failures_, async_duplicates_, average(ns_async_worker_, async_collected_),
        async_fallback_draws_, async_probes_, async_probe_hits_, async_probe_errors_,
        average(ns_async_probe_, async_probes_), double(ns_async_generic_) / 1e6, async_no_generic_);
  }

  // Destructor, before pipelines, modules, render passes and the cache are destroyed.
  void StopAsyncSpecialized() {
    {
      std::lock_guard<std::mutex> lock(async_mutex_);
      async_stop_ = true;
    }
    async_wake_.notify_all();
    if (async_thread_.joinable()) async_thread_.join();  // at most the pipeline being compiled
    for (AsyncPipelineJob& job : async_done_) {
      if (job.pipeline != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, job.pipeline, nullptr);
    }
    async_done_.clear();
    async_queue_.clear();
  }

  std::thread async_thread_;
  std::mutex async_mutex_;
  std::condition_variable async_wake_;
  std::deque<AsyncPipelineJob> async_queue_;  // guarded by async_mutex_
  std::deque<AsyncPipelineJob> async_done_;   // guarded by async_mutex_
  bool async_stop_ = false;                   // guarded by async_mutex_
  std::atomic<uint64_t> async_done_count_{0};
  // Ring only from here.
  bool async_no_thread_ = false;
  std::unordered_map<uint64_t, PipelineKey> async_pending_;  // XXH3 of the specialized key -> key
  std::unordered_set<uint64_t> async_failed_;
  uint64_t async_collected_ = 0, async_installed_ = 0, async_failures_ = 0, async_duplicates_ = 0;
  uint64_t async_deferred_ = 0, async_fallback_draws_ = 0, async_no_generic_ = 0;
  uint64_t async_probes_ = 0, async_probe_hits_ = 0, async_probe_errors_ = 0;
  uint64_t ns_async_probe_ = 0, ns_async_generic_ = 0, ns_async_worker_ = 0;
  uint64_t async_report_changes_ = 0;
  std::chrono::steady_clock::time_point async_report_{};

  // --- 2. Shipped prewarm list --------------------------------------------------------------------------------
  // LoadPipelinesList, before the prewarm thread exists: appends the records of the shipped list
  // (masseffect_native_pipelines_shipped_list) that the normal list does not have. Keys are compared with the
  // numbers stored in each file, which may be stale; RenumberPipelinesList (once the library is loaded) rewrites
  // them from the fingerprints and drops what then turns out to be a duplicate.
  void MergeShippedList() {
    std::string name = REXCVAR_GET(masseffect_native_pipelines_shipped_list);
    std::erase(name, '"');
    if (name.empty()) return;
    // Relative: next to the NRO; installed NSP: the SD data folder first, then romfs:/ (me_packaged.h ShippedFile).
    const std::filesystem::path path = me::packaged::ShippedFile(std::filesystem::path(name));
    std::vector<uint8_t> data;
    ReadWholeFile(path, data);
    uint32_t header[4] = {};
    if (data.size() >= kHeaderList) std::memcpy(header, data.data(), sizeof(header));
    if (data.size() < kHeaderList || header[0] != kMagicPipelinesList || header[1] != kVersionPipelinesList ||
        header[2] != sizeof(RegisterPipeline) ||
        data.size() != kHeaderList + size_t(header[3]) * sizeof(RegisterPipeline)) {
      REXLOG_WARN("[native] C6 prewarm: shipped list {} {}", path.string(),
                  data.empty() ? "not found or empty" : "from another list version or damaged: ignored");
      return;
    }
    uint32_t added = 0, present = 0, rejected = 0;
    for (uint32_t i = 0; i < header[3]; ++i) {
      RegisterPipeline r;
      std::memcpy(&r, data.data() + kHeaderList + size_t(i) * sizeof(RegisterPipeline), sizeof(r));
      if (r.n_attributes > RegisterPipeline::kMaxAttributes || r.n_bindings > RegisterPipeline::kMaxBindings ||
          file_list_.size() >= kMaxRegistersList) {
        ++rejected;
        continue;
      }
      if (index_list_.emplace(XXH3_64bits(&r.key, sizeof(r.key)), file_list_.size()).second) {
        file_list_.push_back(r);
        ++added;
      } else {
        ++present;
      }
    }
    if (added) ++list_no_save_;  // the normal list keeps them from now on
    REXLOG_INFO("[native] C6 prewarm: shipped list {}: {} records, {} added, {} already in the list, {} rejected",
                path.string(), header[3], added, present, rejected);
  }

  // Multi-threaded prewarm (masseffect_native_pipelines_prewarm_threads): TryPrewarm, PrewarmedLoop, PrewarmPublish.
  std::vector<std::thread> prewarm_helpers_;
  std::unique_ptr<std::atomic<uint8_t>[]> prewarm_done_flags_;  // per record of file_list_, set after its state
  std::atomic<size_t> prewarm_next_{0};                           // the next record a prewarm thread claims
  std::atomic<uint32_t> prewarm_running_{0};
  std::mutex prewarm_publish_mutex_;

  // --- 3. Chunked write of the pipelines file -----------------------------------------------------------------
  // Writer thread. Pieces of masseffect_native_pipelines_save_chunk_kb with a pause between them, so the game's
  // own SD card requests are not queued behind one 16 MB write. No pause once the renderer is stopping.
  bool WriteChunked(std::ofstream& file, const uint8_t* data, size_t bytes, uint32_t& pieces) {
    const int32_t chunk_kb = REXCVAR_GET(masseffect_native_pipelines_save_chunk_kb);
    const size_t chunk = chunk_kb > 0 ? size_t(chunk_kb) << 10 : bytes;
    const int32_t pause_ms = REXCVAR_GET(masseffect_native_pipelines_save_pause_ms);
    size_t done = 0;
    while (done < bytes) {
      const size_t n = std::min(chunk, bytes - done);
      if (!file.write(reinterpret_cast<const char*>(data + done), std::streamsize(n))) return false;
      done += n;
      ++pieces;
      if (chunk_kb > 0) {
        if (!file.flush()) return false;
        bool stopping = false;
        {
          std::lock_guard<std::mutex> latch(writer_mutex_);
          stopping = writer_stop_;
        }
        if (pause_ms > 0 && done < bytes && !stopping) {
          std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms));
        }
      }
    }
    return true;
  }

  // Writer thread (masseffect_native_pipelines_save_serialize_off_ring): the cache data, empty if it did not
  // change size since the last save. bytes_cache_saved_ belongs to the writer thread in this mode.
  std::vector<uint8_t> SerializeCacheOnWriter(uint64_t& ns) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> data;
    size_t bytes = 0;
    if (cache_pipelines_ != VK_NULL_HANDLE &&
        data_cache_(device_, cache_pipelines_, &bytes, nullptr) == VK_SUCCESS && bytes &&
        bytes != bytes_cache_saved_) {
      data.resize(bytes);
      const VkResult result = data_cache_(device_, cache_pipelines_, &bytes, data.data());
      if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !bytes) {
        data.clear();
      } else {
        data.resize(bytes);
        bytes_cache_saved_ = bytes;
      }
    }
    ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    return data;
  }

  bool serialize_off_ring_ = false;  // masseffect_native_pipelines_save_serialize_off_ring, read at start-up
  bool writer_cache_wanted_ = false;  // guarded by writer_mutex_: the writer serializes the cache itself
  std::atomic<uint64_t> ns_ring_serialize_{0};  // last serialization on the ring (old mode), for the log
  uint64_t writer_ns_serialize_ = 0;             // writer thread: its last serialization
