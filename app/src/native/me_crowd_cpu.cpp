// Mass Effect - game-thread cost of characters: skeletal pose updates of meshes not on screen, and dynamic light
// environment (DLE) updates (docs/engine-settings-ab.md, part B). Both off by default.
//
// WHAT THE 2007 ENGINE ALREADY DOES (addresses of this edition; details and the
// addresses of the callees in docs/engine-settings-ab.md)
//   USkeletalMeshComponent::Tick (vtable slot 77, sub_824C1058): ticks the anim tree and the skel controls every
//   frame, then updates the pose (bone blending, space bases) unless bUpdateSkelWhenNotRendered (bit 0x80000000 of
//   the bitfield at +868) is FALSE and LastRenderTime (+608) is older than WorldInfo.TimeSeconds - 1.0. The class
//   default is TRUE; the Normandy maps set FALSE on about two thirds of the placed BioPawn mesh components
//   (BIOA_NOR10_* packages), the rest inherit TRUE.
//   UDynamicLightEnvironmentComponent::Tick (slot 77, sub_82558D30): if bEnabled (+88 bit 31) ticks the state (+92).
//   That already throttles: an environment whose owner was not rendered for 1 s updates only every
//   InvisibleUpdateTime (+112, BioPawn 10 s) with a 0.8-1.2 jitter, a full (static) update happens every
//   MinTimeBetweenFullUpdates (+120, BioPawn 1.0 s); visible ones run the dynamic update and the proxy update every
//   frame. bForceFullUpdate (+88 bit 30) forces an update.
//
// WHAT THE CVARS ADD
//   masseffect_skel_skip_unrendered = true: treat bUpdateSkelWhenNotRendered as FALSE for every skeletal mesh
//     component except those owned by the local player's pawn (cleared around the original Tick and restored). The
//     engine's own rule then applies: no pose update while not rendered for 1 s. Anim nodes still tick (notifies,
//     FinishAnim latent script functions keep working).
//   masseffect_dle_visible_every = N (2..8): run the light environment tick only every N-th tick of each component
//     (with N times the delta time), unless bForceFullUpdate is set. Character lighting reacts up to N-1 frames late.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <unordered_map>

REXCVAR_DEFINE_BOOL(masseffect_skel_skip_unrendered, false, "Mass Effect",
                    "Game thread: no skeletal pose update for meshes not rendered for 1 s, also where the data says "
                    "bUpdateSkelWhenNotRendered=True (the local player's pawn excluded); docs/engine-settings-ab.md")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_dle_visible_every, 0, "Mass Effect",
                     "Game thread: run each dynamic light environment update only every N-th frame (N times the delta "
                     "time), forced updates still immediate; 0/1 = every frame (off); docs/engine-settings-ab.md")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824C1058);  // USkeletalMeshComponent::Tick(FLOAT DeltaTime)
REX_EXTERN(__imp__sub_82558D30);  // UDynamicLightEnvironmentComponent::Tick(FLOAT DeltaTime)

namespace {

constexpr uint32_t kGEngine = 0x82EAEA1C, kGWorld = 0x82EAEA94;
constexpr uint32_t kGamePlayersData = 748, kGamePlayersNum = 752, kPlayerActor = 64, kControllerPawn = 492;
constexpr uint32_t kComponentOwner = 76;       // UActorComponent::Owner
constexpr uint32_t kSkelFlags = 868;           // bUpdateSkelWhenNotRendered = bit 31
constexpr uint32_t kSkelLastRenderTime = 608;  // UPrimitiveComponent::LastRenderTime
constexpr uint32_t kUpdateSkelWhenNotRendered = 0x80000000u;
constexpr uint32_t kDleFlags = 88;             // bEnabled bit 31, bForceFullUpdate bit 30
constexpr uint32_t kDleForce = 0x40000000u;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t v = Load32(base, address);
  float f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

uint32_t LocalPlayerPawn(const uint8_t* base) {
  const uint32_t engine = Load32(base, kGEngine);
  if (!engine || int32_t(Load32(base, engine + kGamePlayersNum)) <= 0) return 0;
  const uint32_t players = Load32(base, engine + kGamePlayersData);
  const uint32_t player = players ? Load32(base, players) : 0;
  const uint32_t controller = player ? Load32(base, player + kPlayerActor) : 0;
  return controller ? Load32(base, controller + kControllerPawn) : 0;
}

// WorldInfo.TimeSeconds, read the way the skeletal Tick reads it: GWorld +84, +60, [0], +740.
float WorldTime(const uint8_t* base) {
  const uint32_t world = Load32(base, kGWorld);
  const uint32_t a = world ? Load32(base, world + 84) : 0;
  const uint32_t b = a ? Load32(base, a + 60) : 0;
  const uint32_t info = b ? Load32(base, b) : 0;
  return info ? LoadFloat(base, info + 740) : 0.0f;
}

struct SkelStats {
  uint64_t ticks = 0, cleared = 0, skipped = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};

}  // namespace

REX_HOOK_RAW(sub_824C1058) {
  static const bool enabled = REXCVAR_GET(masseffect_skel_skip_unrendered);
  if (!enabled) {
    __imp__sub_824C1058(ctx, base);
    return;
  }
  static SkelStats stats;  // game thread only
  const uint32_t component = ctx.r3.u32;
  ++stats.ticks;
  const uint32_t flags = component ? Load32(base, component + kSkelFlags) : 0;
  const bool clear = (flags & kUpdateSkelWhenNotRendered) &&
                     Load32(base, component + kComponentOwner) != LocalPlayerPawn(base);
  if (clear) {
    Store32(base, component + kSkelFlags, flags & ~kUpdateSkelWhenNotRendered);
    ++stats.cleared;
    if (LoadFloat(base, component + kSkelLastRenderTime) <= WorldTime(base) - 1.0f) ++stats.skipped;
  }
  __imp__sub_824C1058(ctx, base);
  if (clear)
    Store32(base, component + kSkelFlags, Load32(base, component + kSkelFlags) | kUpdateSkelWhenNotRendered);
  const auto now = std::chrono::steady_clock::now();
  if (now - stats.last >= std::chrono::seconds(10)) {
    REXLOG_INFO("[crowd] 10 s: {} skeletal ticks, {} with bUpdateSkelWhenNotRendered cleared, {} of them not rendered "
                "(pose update skipped)",
                stats.ticks, stats.cleared, stats.skipped);
    stats = SkelStats{};
    stats.last = now;
  }
}

REX_HOOK_RAW(sub_82558D30) {
  static const int every = REXCVAR_GET(masseffect_dle_visible_every);
  if (every <= 1) {
    __imp__sub_82558D30(ctx, base);
    return;
  }
  static std::unordered_map<uint32_t, uint32_t> counters;  // game thread only
  static uint64_t ran = 0, skipped = 0;
  static auto last = std::chrono::steady_clock::now();
  const uint32_t component = ctx.r3.u32;
  if (counters.size() > 8192) counters.clear();  // components come and go with levels
  uint32_t& counter = counters[component];
  const bool forced = component && (Load32(base, component + kDleFlags) & kDleForce);
  if (counter == 0) counter = 1 + (component >> 4) % uint32_t(every);  // spread the components over the frames
  if (!forced && --counter != 0) {
    ++skipped;
  } else {
    counter = uint32_t(every);
    ++ran;
    const double delta = ctx.f1.f64;
    if (!forced) ctx.f1.f64 = delta * every;
    __imp__sub_82558D30(ctx, base);
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - last >= std::chrono::seconds(10)) {
    REXLOG_INFO("[crowd] 10 s: light environment ticks {} run, {} skipped (every {})", ran, skipped, every);
    ran = skipped = 0;
    last = now;
  }
}
