// Mass Effect - guest addresses and UE3 object layout used by the tour (me_tour.cpp) and the UnrealScript profiler
// (me_script_prof.cpp). RUSSIAN edition (XEX 0.0.0.5; overlay copy of app/src/native/me_tour_guest.h, the English one,
// same layout, other addresses; found with the same method and the EN->RU function map). docs/tour.md lists both.
//
// How each value was found (static analysis of the decrypted xex and of the generated code, 2026-10-08):
//   GNames        FName::ToString (found from the "Accessed None '%s'" message) indexes the TArray data pointer at
//                 0x82EC2628; an FNameEntry keeps its UTF-16BE text at +16.
//   GNatives      every exec thunk dispatches the next bytecode through the table at 0x82E9C618.
//   FarMoveActor  AActor::execSetLocation (native table entry "intAActorexecSetLocation") calls sub_823C10F8(GWorld,
//                 Actor, &Dest, bTest, bNoCheck, bAttachedMove) and returns its UBOOL. It stores Dest at Actor+260,
//                 then calls the touch update when bCollideActors (+136 bit 0x20000) is set.
//   CallFunction  sub_823E8EC8(this, FFrame& Stack, RESULT_DECL, UFunction* Function): iNative at Function+144, Func at
//                 +168, FunctionFlags +140 (0x400 native), builds the callee FFrame.
//   ProcessInternal sub_823E93B0(this, FFrame& Stack, RESULT_DECL): Stack.Node (+4) = the UFunction, runs the
//                 bytecode until EX_Return; CallFunction calls it at its end. Hooked for the tour's death guard.
//   ProcessEvent  sub_823E95C0(this, UFunction* Function, void* Parms): (Native|Defined) flag test, probe mask on
//                 names 300..363, ParmsSize at Function+158, PropertiesSize at +80.
//   Levels        FActorIterator::operator++: GWorld->Levels at +72 (data) / +76 (num), the persistent
//                 level at +84, ULevel->Actors at +60 / +64.
//   GameSequences AWorldInfo::execGetGameSequence: GWorld+88 (current level) -> ULevel+160 / +164.
//   Class chain   AActor::execAllActors (IsA): UObject::Class at +52, UStruct super chain at +60 (UField::SuperField).
//   DoTouch       ABioTriggerStream::execDoTouch calls vtable byte offset 840 (slot 210) of the trigger.
//   Rotation      AActor::SetRotation / FarMoveActor: Rotation (Pitch, Yaw, Roll ints) at Actor+272.
//   Properties    CallFunction's parameter loop: UStruct::Children +76, UField::Next +64, UProperty::Offset +100.
// Only the functions this file calls or hooks are named by address here: tools/direct_calls.py treats every 82xxxxxx
// in app/src as hooked (no direct inlining), so the other addresses are only in docs/tour.md.
// Outer +40 verified on the console (RU 2026-10-09, actor Outer == level for every actor); Name +44 / +48 static (the
// name number was decoded on the console). Layout comments: docs/tour.md.
#pragma once

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstdint>

namespace me::tour_guest {

inline constexpr const char* kEdition = "RU";

// Globals (the variable's address; Load32 gives the value).
inline constexpr uint32_t kGEngine = 0x82EAEA3C;
inline constexpr uint32_t kGWorld = 0x82EAEAB4;
inline constexpr uint32_t kGNamesData = 0x82EC2628;  // TArray<FNameEntry*>: data at +0, num at +4
inline constexpr uint32_t kGNatives = 0x82E9C618;    // informational (docs)

// Offsets (same in both editions).
inline constexpr uint32_t kObjOuter = 40, kObjName = 44, kObjNameNumber = 48, kObjClass = 52;
inline constexpr uint32_t kStructSuper = 60;  // UField::SuperField (super class for a UClass)
// Reflection (UObject::CallFunction's parameter loop: Children = Function+76, Property->PropertyFlags (QWORD) +76,
// Locals + Property->Offset with Offset at +100, Property = Property->Next at +64). Used by the tour's health keeper,
// checked at run time on Actor.Location / Rotation / Physics (+260 / +272 / +104).
inline constexpr uint32_t kFieldNext = 64, kStructChildren = 76, kPropertyOffset = 100;
inline constexpr uint32_t kNameEntryText = 16;
inline constexpr uint32_t kWorldLevelsData = 72, kWorldLevelsNum = 76, kWorldPersistentLevel = 84,
                          kWorldCurrentLevel = 88;
inline constexpr uint32_t kLevelActorsData = 60, kLevelActorsNum = 64;
inline constexpr uint32_t kLevelGameSequencesData = 160, kLevelGameSequencesNum = 164;
inline constexpr uint32_t kActorPhysics = 104, kActorFlags0 = 132, kActorFlags1 = 136;
inline constexpr uint32_t kActorDeleteMeBit = 0x10000000u;  // +132, skipped by AActor::execAllActors
inline constexpr uint32_t kActorLocation = 260, kActorRotation = 272;
inline constexpr uint32_t kGamePlayersData = 748, kGamePlayersNum = 752, kPlayerActor = 64, kControllerPawn = 492;
inline constexpr uint32_t kFunctionFlags = 140, kFunctionNative = 0x400u;
inline constexpr uint32_t kTriggerDoTouchVtableOffset = 840;
inline constexpr uint8_t kPhysNone = 0, kPhysRigidBody = 10;

}  // namespace me::tour_guest

REX_EXTERN(sub_823C10F8);  // UWorld::FarMoveActor(AActor*, const FVector&, UBOOL bTest, UBOOL bNoCheck, UBOOL bAttached)

namespace me::tour_guest {
inline void FarMoveActor(PPCContext& ctx, uint8_t* base) { sub_823C10F8(ctx, base); }
}  // namespace me::tour_guest

// The UnrealScript profiler's hooks (me_script_prof.cpp defines ME_TOUR_GUEST_DEFINE_HOOKS before the include, so the
// hooks exist once). me::sprof::Wrap is declared in me_tour.h.
#ifdef ME_TOUR_GUEST_DEFINE_HOOKS
REX_EXTERN(__imp__sub_823E8EC8);  // UObject::CallFunction(FFrame& Stack, RESULT_DECL, UFunction* Function)
REX_EXTERN(__imp__sub_823E95C0);  // UObject::ProcessEvent(UFunction* Function, void* Parms, void* Result)
REX_HOOK_RAW(sub_823E8EC8) {
  me::sprof::Wrap(ctx, base, ctx.r6.u32, me::sprof::kViaCallFunction, __imp__sub_823E8EC8);
}
REX_HOOK_RAW(sub_823E95C0) {
  me::sprof::Wrap(ctx, base, ctx.r4.u32, me::sprof::kViaProcessEvent, __imp__sub_823E95C0);
}
REX_EXTERN(__imp__sub_823E93B0);  // UObject::ProcessInternal(FFrame& Stack, RESULT_DECL): runs a script function's code
// Called by CallFunction (directly) and ProcessEvent (through UFunction::Func) after the parameters are set up, so not
// calling it skips the function's body without breaking the caller's bytecode stream. The tour's death guard.
REX_HOOK_RAW(sub_823E93B0) { me::tour::ScriptGuard(ctx, base, __imp__sub_823E93B0); }
#endif
