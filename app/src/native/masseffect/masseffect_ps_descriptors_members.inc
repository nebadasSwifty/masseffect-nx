// Exact pixel shader rewrites around descriptors and clamps (docs/scene-shader-cost.md, "Descriptor and clamp
// rewrites"): members of DrawsVulkanImpl, included inside the class by masseffect_native_draws.cpp. The settings are
// in masseffect_ps_descriptors_cvars.inc, the SPIR-V transforms in me_ps_descriptors_spirv.h.
//
//   A  masseffect_native_ps_flt_bounds_ubo  clamp bounds from shared words 122/123   (kKeyPsFltBounds)
//   B  masseffect_native_ps_combined_heap   combined image+sampler heap at set 5     (kKeyPsCombined)
//   C  masseffect_native_ps_no_index_mask   sign byte cleared, `& 0xFFFFFF` removed  (kKeyPsNoMask)
//
// Ring thread: the per-frame settings, the per-draw plan (PlanPsDescriptors) and the combined heap. Ring and prewarm
// threads: the module transform (ModulePsDescriptors, under modules_depth_mutex_).

  static constexpr uint32_t kCombinedHeapRequested = 16384;

  // --- Settings, once per frame ------------------------------------------------------------------------------------
  bool psd_flt_ = false;     // A
  bool psd_comb_ = false;    // B (only with comb_layout_)
  bool psd_nomask_ = false;  // C

  void PsDescriptorsPerFrame() {
    const bool flt = REXCVAR_GET(masseffect_native_ps_flt_bounds_ubo);
    const bool comb = REXCVAR_GET(masseffect_native_ps_combined_heap) && comb_layout_;
    const bool nomask = REXCVAR_GET(masseffect_native_ps_no_index_mask);
    if (flt != psd_flt_ || comb != psd_comb_ || nomask != psd_nomask_) {
      psd_flt_ = flt;
      psd_comb_ = comb;
      psd_nomask_ = nomask;
      REXLOG_INFO("[native] PS descriptors: clamp bounds from the shared block (A) {}; combined image+sampler heap "
                  "(B) {}{}; index masks dropped (C) {}{}",
                  flt ? "ON" : "off", comb ? "ON" : "off",
                  REXCVAR_GET(masseffect_native_ps_combined_heap) && !comb_layout_ ? " (needs a restart: no set 5)" : "",
                  nomask ? "ON" : "off", nomask && !fold_signs_ ? " (inactive without masseffect_native_fold_texture_signs)" : "");
    }
  }

  // Prewarm: a record whose pixel shader rewrite this session would never ask for.
  bool PsDescriptorsRecordSkipped(const PipelineKey& key) const {
    const uint32_t bits = key.signs_heaps & kKeyPsRewrites;
    return ((bits & kKeyPsFltBounds) && !REXCVAR_GET(masseffect_native_ps_flt_bounds_ubo)) ||
           ((bits & kKeyPsCombined) && (!comb_layout_ || !REXCVAR_GET(masseffect_native_ps_combined_heap))) ||
           ((bits & kKeyPsNoMask) && !REXCVAR_GET(masseffect_native_ps_no_index_mask));
  }

  // --- B: the combined heap ----------------------------------------------------------------------------------------
  bool comb_layout_ = false;  // set 5 is part of layout_pipeline_ (decided at start-up)
  VkDescriptorSetLayout comb_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool comb_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet comb_set_ = VK_NULL_HANDLE;
  uint32_t comb_capacity_ = 0;
  uint32_t comb_next_ = 0;
  bool comb_full_logged_ = false;
  std::vector<VkImageView> comb_views_;     // per 2D slot: the view WriteImage last put there
  std::vector<VkSampler> comb_samplers_;    // per sampler slot
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> comb_pairs_;  // per 2D slot: (sampler slot, combined slot)

  // CreateDescriptors, after the four heaps and before the pipeline layout. false only on a Vulkan error with the
  // setting on; with the setting off nothing is created and the layout is the usual one.
  bool CreateCombinedHeap() {
    comb_layout_ = false;
    if (!REXCVAR_GET(masseffect_native_ps_combined_heap)) return true;
    const auto get_layout_support = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSupport>(deferred::Proc(
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr, device_, "vkGetDescriptorSetLayoutSupport"));
    if (!get_layout_support) return true;
    const VkDescriptorBindingFlags flags_binding = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                   VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                   VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = kCombinedHeapRequested;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags.bindingCount = 1;
    flags.pBindingFlags = &flags_binding;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.pNext = &flags;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    info.bindingCount = 1;
    info.pBindings = &binding;
    VkDescriptorSetLayoutSupport support{};
    support.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
    for (;;) {
      get_layout_support(device_, &info, &support);
      if (support.supported) break;
      if (binding.descriptorCount <= 256) {
        REXLOG_WARN("[native] PS descriptors: combined heap unsupported; masseffect_native_ps_combined_heap stays off");
        return true;
      }
      binding.descriptorCount /= 2;
    }
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info, nullptr, &comb_set_layout_) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, binding.descriptorCount};
    VkDescriptorPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    info_pool.maxSets = 1;
    info_pool.poolSizeCount = 1;
    info_pool.pPoolSizes = &size;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool, nullptr, &comb_pool_) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve.descriptorPool = comb_pool_;
    reserve.descriptorSetCount = 1;
    reserve.pSetLayouts = &comb_set_layout_;
    if (dfn_.vkAllocateDescriptorSets(device_, &reserve, &comb_set_) != VK_SUCCESS) return false;
    comb_capacity_ = binding.descriptorCount;
    comb_next_ = 0;
    comb_views_.assign(heaps_[0].capacity, VK_NULL_HANDLE);
    comb_samplers_.assign(heaps_[3].capacity, VK_NULL_HANDLE);
    comb_pairs_.assign(heaps_[0].capacity, {});
    comb_layout_ = true;
    REXLOG_INFO("[native] PS descriptors: combined image+sampler heap at set 5, {} entries (requested {})",
                comb_capacity_, kCombinedHeapRequested);
    return true;
  }

  void DestroyCombinedHeap() {
    if (comb_pool_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, comb_pool_, nullptr);
    if (comb_set_layout_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, comb_set_layout_, nullptr);
    comb_pool_ = VK_NULL_HANDLE;
    comb_set_layout_ = VK_NULL_HANDLE;
    comb_set_ = VK_NULL_HANDLE;
  }

  void WriteCombined(uint32_t slot, VkImageView view, VkSampler sampler) {
    VkDescriptorImageInfo image{};
    image.sampler = sampler;
    image.imageView = view;
    image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = comb_set_;
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  // WriteImage of the 2D heap: every combined descriptor made from that slot follows it, at the same moment, so a
  // combined index always means "the view in 2D slot t with the sampler in sampler slot s", like the two words did.
  void CombinedMirrorImage(uint32_t slot, VkImageView view) {
    if (!comb_layout_ || slot >= comb_views_.size()) return;
    comb_views_[slot] = view;
    for (const auto& [sampler_slot, combined] : comb_pairs_[slot]) {
      const VkSampler sampler = comb_samplers_[sampler_slot] ? comb_samplers_[sampler_slot] : sampler_empty_;
      WriteCombined(combined, view, sampler);
    }
  }

  void CombinedMirrorSampler(uint32_t slot, VkSampler sampler) {
    if (!comb_layout_ || slot >= comb_samplers_.size()) return;
    comb_samplers_[slot] = sampler;  // sampler slots are never rewritten (SlotSampler keeps them for the session)
  }

  // The combined index for (2D slot, sampler slot), created on first use; UINT32_MAX when the heap is full.
  uint32_t CombinedSlot(uint32_t texture_slot, uint32_t sampler_slot) {
    if (texture_slot >= comb_pairs_.size() || sampler_slot >= comb_samplers_.size()) return UINT32_MAX;
    auto& pairs = comb_pairs_[texture_slot];
    for (const auto& [s, combined] : pairs)
      if (s == sampler_slot) return combined;
    if (comb_next_ >= comb_capacity_) {
      if (!comb_full_logged_) {
        comb_full_logged_ = true;
        REXLOG_WARN("[native] PS descriptors: combined heap full ({} pairs); new (texture, sampler) pairs are drawn "
                    "with the separate heaps from now on (same image)", comb_capacity_);
      }
      return UINT32_MAX;
    }
    const uint32_t combined = comb_next_++;
    const VkImageView view = comb_views_[texture_slot] ? comb_views_[texture_slot] : empty_[0].view;
    const VkSampler sampler = comb_samplers_[sampler_slot] ? comb_samplers_[sampler_slot] : sampler_empty_;
    WriteCombined(combined, view, sampler);
    pairs.emplace_back(sampler_slot, combined);
    return combined;
  }

  // After the heap sets are bound (they are bound together, once per command buffer).
  void BindCombinedSet(VkCommandBuffer cmd) {
    if (!comb_layout_) return;
    dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, me::native::kCombinedHeapSet, 1,
                                 &comb_set_, 0, nullptr);
  }

  // --- Per-PS / per-VS analysis (ring thread, once per shader) -----------------------------------------------------
  struct PsDescriptorsPlan {
    bool flt = false;     // A replaces at least one clamp bound
    bool comb = false;    // B is sound for this PS and redirects at least one 2D fetch
    bool nomask = false;  // C is sound once the signs of all its sampled registers are folded
    uint32_t registers = 0;  // sampled registers below 16
  };
  std::unordered_map<uint32_t, PsDescriptorsPlan> psd_plans_;   // by PS number
  std::unordered_map<uint32_t, bool> psd_vs_reads_words_;        // by VS number: reads shared words 0-47
  uint32_t psd_plans_logged_ = 0;

  const PsDescriptorsPlan& PsDescriptorsPlanFor(const ShaderEntry& ps) {
    if (const auto it = psd_plans_.find(ps.number); it != psd_plans_.end()) return it->second;
    PsDescriptorsPlan plan;
    const std::vector<uint32_t>& code = ps.shader->Spirv();
    std::string reason_a, reason_b, reason_c;
    std::vector<uint32_t> out;
    me::native::FltBoundsStats flt;
    plan.flt = me::native::ReadFltBoundsFromShared(code, out, flt, reason_a) && flt.replaced;
    uint32_t registers[8];
    const uint32_t n = SignedRegistersPS(ps, registers);
    uint32_t mask = 0;
    for (const SamplerShader& sampler : ps.samplers)
      if (sampler.register_value < 16) mask |= uint32_t(1) << sampler.register_value;
    plan.registers = uint32_t(std::popcount(mask));
    uint64_t known = 0;
    for (uint32_t k = 0; k < n; ++k)
      for (uint32_t h = 0; h < 3; ++h) known |= uint64_t(1) << (h * 16 + registers[k]);
    // B on the plain module and on the signs-folded + C one (the two extremes a key can ask for).
    me::native::CombinedFetchStats comb;
    plan.comb = me::native::CombineTexture2DFetches(code, out, comb, reason_b) && comb.chains;
    std::vector<uint32_t> folded, nomask;
    uint8_t signs[48] = {};
    me::native::TextureSignsFold fold;
    std::string reason_s;
    me::native::IndexMaskStats masks;
    if (n == plan.registers && known && me::native::FoldTextureSigns(code, signs, known, folded, fold, reason_s)) {
      plan.nomask = me::native::DropIndexMasks(folded, known, nomask, masks, reason_c) && masks.removed;
      if (plan.comb && plan.nomask)
        plan.comb = me::native::CombineTexture2DFetches(nomask, out, comb, reason_b) && comb.chains;
    } else {
      reason_c = n != plan.registers ? "more than eight sampled registers" : reason_s;
    }
    if (++psd_plans_logged_ <= 48 || (psd_plans_logged_ & 255) == 0)
      REXLOG_INFO("[native] PS descriptors: PS n{} ({} analyzed): A {} ({} bounds), B {} ({} 2D fetches{}{}), C {}{}{}",
                  ps.number, psd_plans_logged_, plan.flt ? "yes" : "no", flt.replaced, plan.comb ? "yes" : "no",
                  comb.chains, reason_b.empty() ? "" : "; ", reason_b, plan.nomask ? "yes" : "no",
                  reason_c.empty() ? "" : ": ", reason_c);
    return psd_plans_.emplace(ps.number, plan).first->second;
  }

  bool VsReadsDescriptorWords(const ShaderEntry& vs) {
    if (!vs.shader) return true;
    if (const auto it = psd_vs_reads_words_.find(vs.number); it != psd_vs_reads_words_.end()) return it->second;
    const bool reads = me::native::ReadsSharedWords(vs.shader->Spirv(), 0, 48);
    if (reads) REXLOG_INFO("[native] PS descriptors: VS n{} reads descriptor words; B and C off for its draws", vs.number);
    return psd_vs_reads_words_.emplace(vs.number, reads).first->second;
  }

  /*
   * Per draw, BEFORE the shared block is uploaded (B and C change its descriptor words) and after the texture signs
   * of the key were taken from it. Returns the kKeyPs* bits for PipelineKey::signs_heaps.
   *   B: every sampled register below 16 gets, in its 2D word, the combined index of (2D slot, sampler slot) of
   *      that draw, sign byte kept. All or nothing: a full heap leaves the words as they were and the bit clear.
   *   C: only when the signs of every sampled register are folded in the key (signs_folded == registers): the 2D,
   *      3D and cube words of those registers lose their sign byte.
   */
  uint32_t PlanPsDescriptors(const ShaderEntry& ps, const ShaderEntry& vs, uint32_t signs_folded, uint32_t* shared) {
    if (!use_ubo_ || !ps.shader || !(psd_flt_ || psd_comb_ || psd_nomask_)) return 0;
    const PsDescriptorsPlan& plan = PsDescriptorsPlanFor(ps);
    uint32_t bits = 0;
    if (psd_flt_ && plan.flt) bits |= kKeyPsFltBounds;
    const bool comb = psd_comb_ && plan.comb;
    const bool nomask = psd_nomask_ && plan.nomask && signs_folded && signs_folded == plan.registers;
    if ((!comb && !nomask) || VsReadsDescriptorWords(vs)) return bits;
    uint32_t mask = 0;
    for (const SamplerShader& sampler : ps.samplers)
      if (sampler.register_value < 16) mask |= uint32_t(1) << sampler.register_value;
    if (comb) {
      uint32_t combined[16];
      bool all = true;
      for (uint32_t reg = 0; reg < 16 && all; ++reg) {
        if (!((mask >> reg) & 1)) continue;
        combined[reg] = CombinedSlot(shared[reg] & 0x00FFFFFFu, shared[48 + reg]);
        all = combined[reg] != UINT32_MAX;
      }
      if (all) {
        for (uint32_t reg = 0; reg < 16; ++reg)
          if ((mask >> reg) & 1) shared[reg] = (shared[reg] & 0xFF000000u) | combined[reg];
        bits |= kKeyPsCombined;
      }
    }
    if (nomask) {
      for (uint32_t reg = 0; reg < 16; ++reg)
        if ((mask >> reg) & 1)
          for (uint32_t h = 0; h < 3; ++h) shared[h * 16 + reg] &= 0x00FFFFFFu;
      bits |= kKeyPsNoMask;
    }
    return bits;
  }

  // --- The module (ring and prewarm) -------------------------------------------------------------------------------
  struct ModulePsDescriptorsEntry {
    CodePtr source;  // interned: pointer equality is content equality (docs/memory-growth.md 4.1.1)
    uint32_t bits = 0;
    uint64_t cleared = 0;
    VkShaderModule module = VK_NULL_HANDLE;  // null: failed (B or C) or nothing changed (A only: selected is used)
    bool failed = false;
  };
  std::unordered_map<uint64_t, std::vector<ModulePsDescriptorsEntry>> modules_ps_descriptors_;
  uint64_t psd_modules_ = 0;
  uint64_t psd_failures_ = 0;

  /*
   * The kKeyPs* rewrites of the key applied to the selected pixel shader module, after the texture signs and before
   * the restore / FragCoord / depth transforms. Order: C, B, A. VK_NULL_HANDLE when B or C cannot be applied: the
   * draw has already written combined indices or cleared sign bytes, so the plain module would not be exact (the
   * per-PS plan checked the same transforms on the library module, so this is not expected). A failed A is skipped.
   */
  VkShaderModule ModulePsDescriptors(VkShaderModule selected, const PipelineKey& key, const ShaderEntry& ps) {
    const uint32_t bits = key.signs_heaps & kKeyPsRewrites;
    if (!selected || !bits) return selected;
    uint64_t cleared = 0;
    if (bits & kKeyPsNoMask) {
      uint32_t registers[8];
      const uint32_t n = std::min(SignedRegistersPS(ps, registers), (key.signs_heaps >> 16) & 0xFu);
      for (uint32_t k = 0; k < n; ++k)
        for (uint32_t h = 0; h < 3; ++h) cleared |= uint64_t(1) << (h * 16 + registers[k]);
    }
    std::lock_guard<std::mutex> lock(modules_depth_mutex_);
    const CodePtr source = TrackedCodeLocked(selected);
    if (!source) {
      if (++psd_failures_ <= 8)
        REXLOG_ERROR("[native] PS descriptors: PS n{} final module has no tracked SPIR-V", ps.number);
      return (bits & (kKeyPsCombined | kKeyPsNoMask)) ? VK_NULL_HANDLE : selected;
    }
    const uint64_t hash = XXH3_64bits_withSeed(&cleared, sizeof(cleared), source->hash ^ bits);
    auto& bucket = modules_ps_descriptors_[hash];
    for (const auto& entry : bucket) {
      if (entry.bits == bits && entry.cleared == cleared && entry.source == source)
        return entry.failed ? VK_NULL_HANDLE : (entry.module ? entry.module : selected);
    }
    const CodeWordsPtr words = CodeWordsLocked(source);
    if (!words) return (bits & (kKeyPsCombined | kKeyPsNoMask)) ? VK_NULL_HANDLE : selected;  // logged
    std::vector<uint32_t> current = *words, next;
    std::string reason;
    bool failed = false, changed = false;
    uint32_t removed = 0, combined = 0, separate = 0, bounds = 0;
    if (bits & kKeyPsNoMask) {
      me::native::IndexMaskStats stats;
      if (!me::native::DropIndexMasks(current, cleared, next, stats, reason)) {
        failed = true;
        reason = "C: " + reason;
      } else {
        current.swap(next);
        removed = stats.removed;
        changed |= stats.removed != 0;
      }
    }
    if (!failed && (bits & kKeyPsCombined)) {
      me::native::CombinedFetchStats stats;
      if (!me::native::CombineTexture2DFetches(current, next, stats, reason)) {
        failed = true;
        reason = "B: " + reason;
      } else {
        current.swap(next);
        combined = stats.combined;
        separate = stats.separate;
        changed |= stats.chains != 0;
      }
    }
    if (!failed && (bits & kKeyPsFltBounds)) {
      me::native::FltBoundsStats stats;
      std::string reason_a;
      if (me::native::ReadFltBoundsFromShared(current, next, stats, reason_a)) {
        current.swap(next);
        bounds = stats.replaced;
        changed |= stats.replaced != 0;
      } else if (++psd_failures_ <= 8) {
        REXLOG_WARN("[native] PS descriptors: PS n{}: clamp bounds left as immediates: {}", ps.number, reason_a);
      }
    }
    VkShaderModule module = VK_NULL_HANDLE;
    if (!failed && changed) {
      VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = current.size() * sizeof(uint32_t);
      info.pCode = current.data();
      if (CreateModuleCounted(info, &module) != VK_SUCCESS) {
        module = VK_NULL_HANDLE;
        failed = (bits & (kKeyPsCombined | kKeyPsNoMask)) != 0;
        reason = "vkCreateShaderModule failed";
      }
    }
    ModulePsDescriptorsEntry entry;
    entry.source = source;
    entry.bits = bits;
    entry.cleared = cleared;
    entry.module = module;
    entry.failed = failed;
    bucket.push_back(std::move(entry));
    if (failed) {
      if (++psd_failures_ <= 16)
        REXLOG_ERROR("[native] PS descriptors: PS n{} rewrite {:X} failed ({}); the pipeline is not created",
                     ps.number, bits >> 24, reason);
      return VK_NULL_HANDLE;
    }
    if (!module) return selected;
    if (++psd_modules_ <= 32 || (psd_modules_ & 255) == 0)
      REXLOG_INFO("[native] PS descriptors: PS n{} rewritten ({} modules so far): bits {:X}, {} masks removed, {} "
                  "fetches combined ({} kept separate), {} clamp bounds from the shared block",
                  ps.number, psd_modules_, bits >> 24, removed, combined, separate, bounds);
    // The same steps as above, without the statistics: what makes the words again once they were dropped.
    const auto remake = [bits, cleared](const std::vector<uint32_t>& input, std::vector<uint32_t>& output) {
      std::vector<uint32_t> words_now = input, words_next;
      std::string why;
      if (bits & kKeyPsNoMask) {
        me::native::IndexMaskStats masks;
        if (!me::native::DropIndexMasks(words_now, cleared, words_next, masks, why)) return false;
        words_now.swap(words_next);
      }
      if (bits & kKeyPsCombined) {
        me::native::CombinedFetchStats fetches;
        if (!me::native::CombineTexture2DFetches(words_now, words_next, fetches, why)) return false;
        words_now.swap(words_next);
      }
      if (bits & kKeyPsFltBounds) {
        me::native::FltBoundsStats clamps;
        std::string why_a;
        if (me::native::ReadFltBoundsFromShared(words_now, words_next, clamps, why_a)) words_now.swap(words_next);
      }
      output = std::move(words_now);
      return true;
    };
    codes_modules_depth_.insert_or_assign(module, OwnedCodeLocked(std::move(current), source, remake));
    return module;
  }
