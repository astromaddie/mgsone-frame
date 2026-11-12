#include "runtime/games/mgs_vr_input.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <chrono>
#include <memory>
#include "../vr/arm_ik.h"

extern void recomp_dispatch(uint8_t *, recomp_context *, uint32_t);

namespace {
std::atomic<uint64_t> vrShotDirection{0}, vrMovement{0};
struct WeaponSample {
  ps1::games::mgs::VrWeaponPose pose;
  std::chrono::steady_clock::time_point time;
  uint64_t generation = 0;
};
std::shared_ptr<const WeaponSample> vrWeapon;
struct ConstraintSample { ps1::games::mgs::VrWeaponConstraint value; uint64_t generation; };
std::shared_ptr<const ConstraintSample> weaponConstraint;
std::atomic<uint64_t> weaponGeneration{1};
struct WeaponBinding {
  uint32_t owner = 0;
  uint64_t generation = 0;
  int16_t weapon = -1;
  std::array<float,16> localShot{};
};
WeaponBinding weaponBinding; // guest-thread only
constexpr uint64_t movementActive = uint64_t(1) << 63, moveValid = uint64_t(1) << 62, pitchValid = uint64_t(1) << 61;

using ShotVector = std::array<float, 3>;
float shotDot(const ShotVector &a, const ShotVector &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
ShotVector shotCross(const ShotVector &a, const ShotVector &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}
}

namespace ps1::games::mgs {
bool isVrFirearmAction(const Memory &memory, uint32_t handler,
                       int16_t weapon) noexcept {
  if (handler != 0x800599ACu || (weapon != 0 && weapon != 1)) return false;
  // ATTACK clears before this same callback runs its normal exit to idle.
  // Keep head-facing continuity through that normal exit as well.
  // Authored status/pause/context guards remain mandatory at both callers.
  // USA normal shoot helper: SOCOM and FAMAS share this action. Keep a live
  // entry witness in both capture and guest paths, not an address-only gate.
  constexpr std::array<uint32_t,8> entry{0x27bdffd8,0xafb00018,0x00808021,0x24040800,
      0xafbf0020,0x0c0142c1,0xafb1001c,0x1440000b};
  for (unsigned i=0;i<entry.size();++i)
    if (memory.read32(handler+i*4) != entry[i]) return false;
  return true;
}
void setVrWeaponPose(const VrWeaponPose &pose) noexcept {
  if (!pose.wristOwner) {
    weaponGeneration.fetch_add(1,std::memory_order_acq_rel);
    std::atomic_store_explicit(&vrWeapon,std::shared_ptr<const WeaponSample>{},std::memory_order_release);
    std::atomic_store_explicit(&weaponConstraint,std::shared_ptr<const ConstraintSample>{},std::memory_order_release);
    return;
  }
  std::atomic_store_explicit(&vrWeapon,std::make_shared<const WeaponSample>(WeaponSample{pose,std::chrono::steady_clock::now(),weaponGeneration.load(std::memory_order_acquire)}),std::memory_order_release);
}
VrWeaponConstraint vrWeaponConstraint() noexcept {
  const auto value=std::atomic_load_explicit(&weaponConstraint,std::memory_order_acquire);
  return value && value->generation==weaponGeneration.load(std::memory_order_acquire)?value->value:VrWeaponConstraint{};
}

void setVrMovement(const input::VrInputSample &sample) noexcept {
  uint64_t packed = 0;
  if (sample.facing) {
    packed = movementActive | (*sample.facing & 4095u);
    if (sample.movementHeading) packed |= moveValid | (uint64_t(*sample.movementHeading & 4095u) << 12);
    // HMD pitch is view-only; CONTROL X tilts the complete native body root.
  }
  vrMovement.store(packed, std::memory_order_release);
}

void setVrShotDirection(const std::array<float, 3> &direction) noexcept {
  const float length = std::sqrt(shotDot(direction, direction));
  uint64_t packed = 0;
  if (std::isfinite(length) && length > 0.01f) {
    for (unsigned axis = 0; axis < 3; ++axis) {
      const auto value = int16_t(std::lround(direction[axis] * (4096.0f / length)));
      packed |= uint64_t(uint16_t(value)) << (axis * 16);
    }
  }
  vrShotDirection.store(packed, std::memory_order_relaxed);
}
}

namespace {
bool vrSnake(ps1::Memory &memory, uint32_t actor, uint64_t sample, bool weaponAction = false) {
  if (!(sample & movementActive) || actor < 0x80010000u || actor > 0x801FF500u ||
      memory.read32(0x800AE16Cu) != actor + 0x20 || memory.read32(0x800AE0A0u) != 0 ||
      (memory.read32(0x800ADB3Cu) & 0xdc080050u) ||
      (memory.read32(0x800AE1C8u) & ((sample & pitchValid) ? 0x20007282u : 0x20007283u))) return false;
  const auto name = memory.read32(actor + 20);
  if (name < 0x80010000u || name > 0x801FFFF8u ||
      std::memcmp(memory.ramPtr() + (name & 0x1fffffffu), "snake.c", sizeof("snake.c"))) return false;
  // USA idle/crouch/prone/run and run-start handlers. Damage, wall presses,
  // ladders, dragging and scripted motion stay native. Weapon callers may
  // additionally admit the authenticated firearm action without moving Snake.
  const uint32_t handler = memory.read32(actor + 0x9c4);
  return handler == 0x80054854u || handler == 0x800549D8u || handler == 0x80054B50u ||
         handler == 0x80054C6Cu || handler == 0x80054E80u || handler == 0x80055B6Cu ||
         (weaponAction && ps1::games::mgs::isVrFirearmAction(memory, handler,
             int16_t(memory.read16(0x800B752Cu))));
}
void vrFacing(ps1::Memory &memory, uint32_t control, uint64_t sample) {
  const uint16_t heading = sample & 4095u;
  memory.write16(control + 10, heading);
  memory.write16(control + 0x4e, heading);
}
}

namespace {
using ps1::gpu::vr::Matrix;
namespace arm = ps1::gpu::vr::arm;
Matrix readShotMatrix(ps1::Memory &memory,uint32_t address) {
  Matrix result=arm::identity();
  for(unsigned r=0;r<3;++r) {
    for(unsigned c=0;c<3;++c)result[c*4+r]=int16_t(memory.read16(address+(r*3+c)*2))/4096.0f;
    result[12+r]=int32_t(memory.read32(address+20+r*4));
  }
  return result;
}
Matrix compose(const Matrix &a,const Matrix &b) {
  Matrix out=arm::identity();
  for(unsigned c=0;c<4;++c) {
    const auto v=c==3?arm::point(a,arm::position(b)):arm::vector(a,{b[c*4],b[c*4+1],b[c*4+2]});
    for(unsigned r=0;r<3;++r)out[c*4+r]=v[r];
  }
  return out;
}
// HZD_OnlineHazardCheck has native scratch/GTE side effects. Save exactly its
// work regions, including precision metadata, and call on an isolated CPU
// context. No simulation tick, callback drain or actor update is permitted.
struct QueryMemory {
  ps1::Memory &memory;
  uint32_t stack, cursor;
  uint32_t savedCursor;
  ps1::PGXPValue cursorPrecision{};
  bool cursorPrecise = false, restored = false;
  struct Word { uint32_t value; ps1::PGXPValue precision{}; bool precise=false; };
  std::array<Word,768> words{};
  uint32_t address(size_t i) const { return i<512?stack+uint32_t(i)*4:0x1f800000u+uint32_t(i-512)*4; }
  QueryMemory(ps1::Memory &m,uint32_t base,uint32_t iterator):memory(m),stack(base),cursor(iterator) {
    savedCursor=memory.read32(cursor);
    cursorPrecise=memory.lookupPrecision(cursor,savedCursor,cursorPrecision);
    for(size_t i=0;i<words.size();++i) {
      auto &w=words[i];w.value=memory.read32(address(i));
      w.precise=memory.lookupPrecision(address(i),w.value,w.precision);
    }
  }
  bool restore() {
    if (restored) return true;
    restored = true;
    memory.write32Tracked(cursor,savedCursor,cursorPrecise?&cursorPrecision:nullptr);
    for(size_t i=0;i<words.size();++i) {
      const auto &w=words[i];memory.write32Tracked(address(i),w.value,w.precise?&w.precision:nullptr);
    }
    bool equal=memory.read32(cursor)==savedCursor;
    for(size_t i=0;i<words.size();++i) equal &= memory.read32(address(i))==words[i].value;
    return equal;
  }
  ~QueryMemory() { if(!restored) restore(); }
};
struct MuzzleSegment {
  bool valid=false, hit=false, wall=false;
  ShotVector point{}, wallPoint{}, wallNormal{};
};
MuzzleSegment muzzleSegment(recomp_context *ctx,uint32_t control,ShotVector from,ShotVector to) {
  auto &memory=*ctx->mem;
  // USA entry, authenticated from SOCOM's laser helper 80067728 against the
  // retail EXE and its HZD_ALL / NO_PLAYER call. Use all segment attributes
  // here (mask zero) so even walls omitted by player movement block a muzzle.
  constexpr std::array<uint32_t,8> entry{0x27bdffc0,0xafbf003c,0xafbe0038,0xafb70034,
      0xafb60030,0xafb5002c,0xafb40028,0xafb30024};
  for(unsigned i=0;i<entry.size();++i)if(memory.read32(0x80029974u+i*4)!=entry[i])return {};
  const uint32_t map=memory.read32(control+0x2c);
  if(map<0x80010000u||map>0x801fffe0u)return {};
  const uint32_t hzd=memory.read32(map+8);
  if(hzd<0x80010000u||hzd>0x801fff80u||ctx->r29<0x80011000u||ctx->r29>0x801fffc0u)return {};
  for(float v:from)if(!std::isfinite(v)||v < -32760 ||v > 32760)return {};
  for(float v:to)if(!std::isfinite(v)||v < -32760 ||v > 32760)return {};
  const uint32_t base=(ctx->r29-2048)&~15u;
  if (ctx->r28 < 0x80010000u || ctx->r28 > 0x801ff000u) return {};
  QueryMemory restore(memory,base,ctx->r28+0x81c);
  const recomp_context original=*ctx;
  recomp_context query=*ctx;
  query.r29=base+1984;query.r4=hzd;query.r5=query.r29+24;query.r6=query.r29+32;query.r7=15;
  query.nativeResumePc=query.nativeEntryPc=0;
  query.callbackDrainBlockDepth=ctx->callbackDrainBlockDepth+1;
  memory.write32(query.r29+16,0);
  for(unsigned i=0;i<4;++i) {
    memory.write16(query.r5+i*2,i<3?uint16_t(int16_t(std::lround(from[i]))):0);
    memory.write16(query.r6+i*2,i<3?uint16_t(int16_t(std::lround(to[i]))):0);
  }
  query.r2=1; // a missing dispatcher cannot admit an unchecked muzzle
  recomp_dispatch(memory.ramPtr(),&query,0x80029974u);
  MuzzleSegment result;result.valid=true;result.hit=query.r2!=0;
  if(result.hit) result.point={float(int16_t(memory.read16(0x1f800054u))),
      float(int16_t(memory.read16(0x1f800058u))),float(int16_t(memory.read16(0x1f800056u)))};
  if(result.hit) {
    // USA TestSegment80028F9C retains the KSEG0 wall pointer at scratch+64.
    // TestFloor80029428 clears bit31 there, so floors cannot enter this path.
    // HZD_SEG has two {x,z,y,height} endpoints; its plane is always vertical.
    const uint32_t wall=memory.read32(0x1f800064u);
    if(wall>=0x80010000u && wall<=0x801ffff0u && !(wall&3u)) {
      const float x1=int16_t(memory.read16(wall)),z1=int16_t(memory.read16(wall+2));
      const float dx=int16_t(memory.read16(wall+8))-x1,dz=int16_t(memory.read16(wall+10))-z1;
      const float length=std::hypot(dx,dz);
      if(length>=1) {
        result.wallPoint={x1,0,z1};result.wallNormal={dz/length,0,-dx/length};
        float side=arm::dot(arm::sub(from,result.wallPoint),result.wallNormal);
        if(side<0) { result.wallNormal=arm::scale(result.wallNormal,-1);side=-side; }
        // A source on the wall or inconsistent hit keeps the segment fallback.
        result.wall=side>=1 && arm::dot(arm::sub(to,result.wallPoint),result.wallNormal)<=1;
      }
    }
  }
  const bool restored=restore.restore() && std::memcmp(&original,ctx,sizeof(original))==0;
  ps1::metrics::trace("mgs.vr.muzzle_segment",{{"owner",memory.read32(control+0x7c)+72+4*92},
      {"weapon",int16_t(memory.read16(0x800B752Cu))},{"hits",query.r2},{"restore_ok",restored},{"wall",result.wall},
      {"normal_x",int64_t(std::lround(result.wallNormal[0]*4096))},{"normal_z",int64_t(std::lround(result.wallNormal[2]*4096))},
      {"from_x",int64_t(std::lround(from[0]))},{"from_y",int64_t(std::lround(from[1]))},{"from_z",int64_t(std::lround(from[2]))},
      {"target_x",int64_t(std::lround(to[0]))},{"target_y",int64_t(std::lround(to[1]))},{"target_z",int64_t(std::lround(to[2]))},
      {"hit_x",int64_t(std::lround(result.point[0]))},{"hit_y",int64_t(std::lround(result.point[1]))},{"hit_z",int64_t(std::lround(result.point[2]))}});
  result.valid &= restored;
  return result;
}
void publishConstraint(const ps1::games::mgs::VrWeaponConstraint &value,uint64_t generation) {
  std::atomic_store_explicit(&weaponConstraint,
      std::make_shared<const ConstraintSample>(ConstraintSample{value,generation}),std::memory_order_release);
}
struct ResolvedWeapon { Matrix wrist{},shot{}; bool valid=false; };
ResolvedWeapon resolveWeapon(recomp_context *ctx,const WeaponSample &sample) {
  auto &memory=*ctx->mem;
  const auto reject=[&]{publishConstraint({sample.pose,true},sample.generation);return ResolvedWeapon{};};
  if(std::chrono::steady_clock::now()-sample.time>std::chrono::milliseconds(150))return reject();
  const uint32_t control=memory.read32(0x800AE16Cu);
  if(!vrSnake(memory,control-0x20,vrMovement.load(std::memory_order_acquire),true))return reject();
  const uint32_t body=memory.read32(control+0x7c);
  if(body<0x80010000u||body>0x801ff800u||memory.read16(body+46)!=16||
      sample.pose.wristOwner!=body+72+4*92||weaponBinding.owner!=sample.pose.wristOwner||
      sample.generation!=weaponGeneration.load(std::memory_order_acquire)||weaponBinding.generation!=sample.generation||
      weaponBinding.weapon!=int16_t(memory.read16(0x800B752Cu)))return reject();
  const auto hand=readShotMatrix(memory,sample.pose.wristOwner);
  if(arm::length(arm::sub(arm::position(sample.pose.wristWorld),arm::position(hand)))>1500)return reject();
  const auto native=compose(hand,weaponBinding.localShot);
  Matrix wrist=sample.pose.wristWorld;
  auto shot=compose(wrist,weaponBinding.localShot);
  const auto from=arm::point(native,{0,400,0}),to=arm::point(shot,{0,400,0});
  const auto segment=muzzleSegment(ctx,control,from,to);
  if(!segment.valid)return reject();
  if(segment.hit) {
    // Keep a vertical wall's tangent coordinates: clipping along the native
    // animated muzzle ray otherwise drags the tracked hand sideways/upward.
    // Floors and unverified/degenerate walls retain the native segment clamp.
    // The rigid muzzle offset and orientation stay identical in both paths.
    const auto ray=arm::sub(to,from);
    const float length=arm::length(ray);
    if(length<1)return reject();
    const float hitDistance=arm::dot(arm::sub(segment.point,from),arm::scale(ray,1/length));
    if(hitDistance<0||hitDistance>length+10)return reject();
    auto safe=arm::add(from,arm::scale(ray,std::max(0.0f,hitDistance-20)/length));
    if(segment.wall) {
      const float distance=arm::dot(arm::sub(to,segment.wallPoint),segment.wallNormal);
      // Ramp the existing 20-unit inset with penetration. A one-unit minimum
      // matches the native integer-coordinate resolution; contact cannot add
      // a sudden full inset. The final native query still decides acceptance.
      const float margin=std::clamp(-distance,1.0f,20.0f);
      safe=arm::add(to,arm::scale(segment.wallNormal,margin-distance));
    }
    const auto correction=arm::sub(safe,to);
    for(unsigned axis=0;axis<3;++axis)wrist[12+axis]+=correction[axis];
    // Re-solve reach before accepting the shortened muzzle. If the arm
    // limits would move it back through the wall, retain the native pose.
    ps1::gpu::replay::MgsArmPose skeleton;
    skeleton.owners={1,2,3};skeleton.world=sample.pose.armWorld;
    arm::State state;state.weight=1;
    arm::Target target;target.tracked=true;target.wrist=wrist;
    const auto solved=arm::solve(skeleton,target,{-1,-1.5f,0},0,state);
    if(!solved.active)return reject();
    wrist=solved.world[2];
    shot=compose(wrist,weaponBinding.localShot);
    const auto verify=muzzleSegment(ctx,control,from,arm::point(shot,{0,400,0}));
    if(!verify.valid||verify.hit)return reject();
    publishConstraint({{sample.pose.wristOwner,wrist,sample.pose.armWorld},false},sample.generation);
    ps1::metrics::count("mgs.vr.blocked_muzzle");
  } else publishConstraint({},sample.generation);
  for(unsigned r=0;r<3;++r) {
    if(!std::isfinite(shot[12+r])||std::abs(shot[12+r])>32760)return reject();
    for(unsigned c=0;c<3;++c)if(!std::isfinite(shot[c*4+r])||std::abs(shot[c*4+r])>2)return reject();
  }
  const auto accepted=arm::point(shot,{0,400,0});
  ps1::metrics::trace("mgs.vr.muzzle_resolved",{{"owner",sample.pose.wristOwner},{"weapon",weaponBinding.weapon},{"clamped",segment.hit},
      {"x",int64_t(std::lround(accepted[0]))},{"y",int64_t(std::lround(accepted[1]))},{"z",int64_t(std::lround(accepted[2]))}});
  return {wrist,shot,true};
}
void refreshWeaponConstraint(recomp_context *ctx) {
  const auto sample=std::atomic_load_explicit(&vrWeapon,std::memory_order_acquire);
  if(!sample) {weaponBinding={};return;}
  // A local muzzle offset is learned only at an authenticated retail shot.
  // Selection/actor changes cannot reuse another gun's offset.
  if(weaponBinding.owner!=sample->pose.wristOwner||weaponBinding.generation!=sample->generation||
      weaponBinding.weapon!=int16_t(ctx->mem->read16(0x800B752Cu))) {
    weaponBinding={};publishConstraint({},sample->generation);return;
  }
  resolveWeapon(ctx,*sample);
}
// One record per authenticated player NewBullet call, before any tracked-shot
// guards. Read-only and dormant when tracing is off; no per-frame actor logging.
void traceBulletBoundary(recomp_context *ctx) {
  if (!ps1::metrics::traceEnabled()) return;
  auto &memory = *ctx->mem;
  const auto sample = std::atomic_load_explicit(&vrWeapon, std::memory_order_acquire);
  const uint64_t movement = vrMovement.load(std::memory_order_acquire);
  const uint32_t control = memory.read32(0x800AE16Cu), actor = control - 0x20;
  const bool actorValid = actor >= 0x80010000u && actor <= 0x801ff500u;
  const uint32_t body = actorValid ? memory.read32(control + 0x7c) : 0;
  const bool bodyValid = body >= 0x80010000u && body <= 0x801ff800u;
  const uint32_t map = actorValid ? memory.read32(control + 0x2c) : 0;
  const bool mapValid = map >= 0x80010000u && map <= 0x801fffe0u;
  const uint32_t name = actorValid ? memory.read32(actor + 20) : 0;
  const bool namedSnake = name >= 0x80010000u && name <= 0x801ffff8u &&
      std::memcmp(memory.ramPtr() + (name & 0x1fffffffu), "snake.c", sizeof("snake.c")) == 0;
  const bool ownerMatches = sample && bodyValid && sample->pose.wristOwner == body + 72 + 4*92;
  bool handInvertible = false;
  int64_t wristDistance = -1;
  if (ownerMatches) {
    const auto hand = readShotMatrix(memory, sample->pose.wristOwner);
    handInvertible = bool(arm::delta(hand, arm::identity()));
    const float distance = arm::length(arm::sub(arm::position(sample->pose.wristWorld), arm::position(hand)));
    if (std::isfinite(distance) && distance < 1e9f) wristDistance = int64_t(std::lround(distance));
  }
  const bool matrixValid = ctx->r4 == ctx->r29 + 16 && ctx->r4 >= 0x80010000u && ctx->r4 <= 0x801fffe0u;
  ps1::metrics::trace("mgs.vr.bullet_boundary", {
      {"call",ctx->r31},{"side",ctx->r5},{"matrix",ctx->r4},{"matrix_ok",matrixValid},
      {"sample",bool(sample)},{"sample_age_us",sample ? std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-sample->time).count() : -1},
      {"sample_generation",sample ? int64_t(sample->generation) : -1},
      {"generation",int64_t(weaponGeneration.load(std::memory_order_acquire))},
      {"movement_active",bool(movement & movementActive)},{"pitch_valid",bool(movement & pitchValid)},
      {"snake_gate",vrSnake(memory,actor,movement)},{"weapon_gate",vrSnake(memory,actor,movement,true)},{"control",control},{"actor_valid",actorValid},
      {"snake_name",namedSnake},{"handler",actorValid ? memory.read32(actor+0x9c4) : 0},
      {"pause",memory.read32(0x800AE0A0u)},{"game_status",memory.read32(0x800ADB3Cu)},
      {"player_status",memory.read32(0x800AE1C8u)},
      {"body",body},{"model_count",bodyValid ? memory.read16(body+46) : 0},
      {"owner",sample ? sample->pose.wristOwner : 0},{"owner_matches",ownerMatches},
      {"hand_inverse",handInvertible},{"wrist_distance",wristDistance},
      {"weapon",int16_t(memory.read16(0x800B752Cu))},{"map",map},
      {"hzd",mapValid ? memory.read32(map+8) : 0},{"hzd_entry",memory.read32(0x80029974u)},
      {"sp",ctx->r29},{"gp",ctx->r28}});
}
bool trackedShot(recomp_context *ctx,uint32_t matrix) {
  const auto sample=std::atomic_load_explicit(&vrWeapon,std::memory_order_acquire);
  if(!sample)return false;
  auto &memory=*ctx->mem;
  const uint32_t control=memory.read32(0x800AE16Cu);
  const auto reject=[&]{publishConstraint({sample->pose,true},sample->generation);return true;};
  if(!vrSnake(memory,control-0x20,vrMovement.load(std::memory_order_acquire),true))return reject();
  const uint32_t body=memory.read32(control+0x7c);
  if(body<0x80010000u||body>0x801ff800u||memory.read16(body+46)!=16||
      sample->pose.wristOwner!=body+72+4*92)return reject();
  const auto hand=readShotMatrix(memory,sample->pose.wristOwner);
  const auto inverse=arm::delta(hand,arm::identity());
  if(!inverse)return reject();
  weaponBinding={sample->pose.wristOwner,sample->generation,int16_t(memory.read16(0x800B752Cu)),
      compose(*inverse,readShotMatrix(memory,matrix))};
  const auto resolved=resolveWeapon(ctx,*sample);
  if(!resolved.valid||sample->generation!=weaponGeneration.load(std::memory_order_acquire))return true; // native shot and native rendered weapon
  const auto &shot=resolved.shot;
  for(unsigned r=0;r<3;++r) {
    memory.write32(matrix+20+r*4,uint32_t(int32_t(std::lround(shot[12+r]))));
    for(unsigned c=0;c<3;++c)memory.write16(matrix+(r*3+c)*2,uint16_t(int16_t(std::lround(shot[c*4+r]*4096))));
  }
  const auto muzzle=arm::point(shot,{0,400,0});
  ps1::metrics::count("mgs.vr.tracked_muzzle");
  ps1::metrics::trace("mgs.vr.tracked_shot",{{"call",ctx->r31},
      {"muzzle_x",int64_t(std::lround(muzzle[0]))},{"muzzle_y",int64_t(std::lround(muzzle[1]))},{"muzzle_z",int64_t(std::lround(muzzle[2]))},
      {"x",int64_t(std::lround(-shot[4]*4096))},{"y",int64_t(std::lround(-shot[5]*4096))},{"z",int64_t(std::lround(-shot[6]*4096))}});
  return true;
}
}

void mgsApplyVrLocomotion(recomp_context *ctx) {
  if (!ctx || !ctx->mem) return;
  const auto sample = vrMovement.load(std::memory_order_acquire);
  const uint32_t actor = ctx->r18; // USA main logic keeps Snake in s2.
  auto &memory = *ctx->mem;
  if (!vrSnake(memory, actor, sample) || ctx->r28 + 0x920 < 0x80010000u ||
      ctx->r28 + 0x920 > 0x801FFFFCu) return;
  // The original pad and wall-direction checks have already run. Let the
  // normal animation state machine see movement along the facing direction,
  // avoiding its turn-around/start delay for backwards and lateral input.
  const uint32_t direction = ctx->r28 + 0x920;
  if (int32_t(memory.read32(direction)) >= 0 && (sample & moveValid))
    memory.write32(direction, sample & 4095u);
  vrFacing(memory, actor + 0x20, sample);
}

void mgsApplyVrControl(recomp_context *ctx) {
  // This is the actual Snake call to GM_ActControl, after root animation has
  // produced step and before CheckCollide/CheckHeight consume it.
  if (!ctx || !ctx->mem || ctx->r31 != 0x8005D0FCu) return;
  const auto sample = vrMovement.load(std::memory_order_acquire);
  const auto control = ctx->r4;
  auto &memory = *ctx->mem;
  if (!vrSnake(memory, control - 0x20, sample, true)) return;
  refreshWeaponConstraint(ctx);
  if (!vrSnake(memory, control - 0x20, sample)) {
    // The authenticated SOCOM/FAMAS shoot helper consumes native movement,
    // not the published head heading. Follow head yaw through rotation only;
    // its animation step, position and normal action transitions stay native.
    vrFacing(memory, control, sample);
    ps1::metrics::trace("mgs.vr.firearm_facing", {{"facing",int64_t(sample & 4095u)},
        {"native_pitch",int16_t(memory.read16(control+8))}});
    return;
  }
  if (sample & moveValid) {
    const float x = int16_t(memory.read16(control + 0x44));
    const float z = int16_t(memory.read16(control + 0x48));
    const float speed = std::hypot(x, z);
    const float angle = float((sample >> 12) & 4095u) * (2 * 3.14159265358979323846f / 4096);
    memory.write16(control + 0x44, uint16_t(int16_t(std::lround(std::sin(angle) * speed))));
    memory.write16(control + 0x48, uint16_t(int16_t(std::lround(std::cos(angle) * speed))));
  }
  vrFacing(memory, control, sample);
  ps1::metrics::trace("mgs.vr.control", {{"facing", int64_t(sample & 4095u)},
      {"move", (sample & moveValid) ? int64_t((sample >> 12) & 4095u) : -1},
      {"step_x", int16_t(memory.read16(control + 0x44))},
      {"step_z", int16_t(memory.read16(control + 0x48))}});
}

void mgsApplyVrBulletAim(recomp_context *ctx) {
  // These USA call sites pass a 32-byte shot matrix at caller sp + 0x10.
  // Retain every ammo, trigger, actor and collision rule in NewBullet.
  if (!ctx || !ctx->mem ||
      (ctx->r31 != 0x80067B7Cu && ctx->r31 != 0x800682D8u && ctx->r31 != 0x8006A2D4u))
    return;
  traceBulletBoundary(ctx);
  if (ctx->r5 != 1) return;
  const uint64_t packed = vrShotDirection.load(std::memory_order_relaxed);
  const uint32_t matrix = ctx->r4;
  if (matrix != ctx->r29 + 16 || matrix < 0x80010000u || matrix > 0x801FFFE0u) return;
  if (trackedShot(ctx,matrix)) return;
  if (!packed) return;
  auto &memory = *ctx->mem;
  std::array<ShotVector, 3> oldColumns{};
  ShotVector aim{};
  for (unsigned row = 0; row < 3; ++row) {
    aim[row] = -float(int16_t(packed >> (row * 16))) / 4096.0f;
    for (unsigned col = 0; col < 3; ++col)
      oldColumns[col][row] = int16_t(memory.read16(matrix + (row * 3 + col) * 2));
  }
  ShotVector right = oldColumns[0];
  const float vertical = shotDot(right, aim);
  for (unsigned axis = 0; axis < 3; ++axis) right[axis] -= vertical * aim[axis];
  float rightLength = std::sqrt(shotDot(right, right));
  if (rightLength < 1) {
    right = shotCross(aim, std::abs(aim[1]) < 0.9f ? ShotVector{0, 1, 0} : ShotVector{1, 0, 0});
    rightLength = std::sqrt(shotDot(right, right));
  }
  for (auto &component : right) component /= rightLength;
  const std::array columns{right, aim, shotCross(right, aim)};
  std::array<ShotVector, 3> scaled = columns;
  for (unsigned col = 0; col < 3; ++col) {
    const float length = std::sqrt(shotDot(oldColumns[col], oldColumns[col]));
    if (length < 1 || length > 8192) return;
    for (auto &component : scaled[col]) component *= length;
  }
  for (unsigned row = 0; row < 3; ++row) {
    // NewBullet starts at local (0, 400, 0) and travels down its -Y axis.
    const uint32_t position = matrix + 20 + row * 4;
    const auto correction = int32_t(std::lround((oldColumns[1][row] - scaled[1][row]) * (400.0f / 4096)));
    memory.write32(position, memory.read32(position) + uint32_t(correction));
    for (unsigned col = 0; col < 3; ++col)
      memory.write16(matrix + (row * 3 + col) * 2, uint16_t(int16_t(std::lround(scaled[col][row]))));
  }
  ps1::metrics::count("mgs.vr.aimed_shots");
  ps1::metrics::trace("mgs.vr.aimed_shot", {{"call", ctx->r31},
      {"x", -int32_t(std::lround(scaled[1][0]))}, {"y", -int32_t(std::lround(scaled[1][1]))},
      {"z", -int32_t(std::lround(scaled[1][2]))}});
}
