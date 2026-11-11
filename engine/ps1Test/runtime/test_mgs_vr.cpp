#include "runtime/cpu_context.h"
#include "runtime/games/mgs_vr_input.h"
#include "runtime/games/mgs_mesh_replay.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/memory.h"
#include "runtime/input/host_input.h"
#include "../../ps1Runtime/src/vr/scene_view.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
using namespace ps1;
using namespace ps1::gpu::vr;
namespace replay = ps1::gpu::replay;

TEST(MgsVrView, WorldPointSurvivesChannelPanYawAndScale) {
  const Vec3 point{800, 1500, -2300};
  for (float yaw : {0.0f, 1.5707963f, 3.1415926f}) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    Matrix channel{c*0.999f, 0, -s, 0, 0, 1.001f, 0, 0, s, 0, c, 0, 12000, -3200, 7000, 1};
    Vec3 captured{};
    for (unsigned r = 0; r < 3; ++r) {
      captured[r] = channel[12+r];
      for (unsigned c = 0; c < 3; ++c) captured[r] += channel[c*4+r]*point[c];
    }
    auto recovered = worldFromView(channel, captured);
    ASSERT_TRUE(recovered);
    for (unsigned a = 0; a < 3; ++a) EXPECT_NEAR((*recovered)[a],point[a],0.002f);
  }
  EXPECT_FALSE(worldFromView(Matrix{}, point));
}
TEST(MgsVrView, ToyboxPlacementIgnoresMovementUntilSceneChanges) {
  Toybox stage;
  stage.place(7, {1000,0,2000});
  const auto before = stage.transform({2000,1000,3000});
  stage.place(7, {30000,500,10000});
  EXPECT_EQ(before, stage.transform({2000,1000,3000}));
  stage.place(8, {30000,500,10000});
  EXPECT_NE(before, stage.transform({2000,1000,3000}));
}

TEST(MgsVrView, GameplayHudFollowsHeadForwardAndRemovesRoll) {
  std::array<EyeView, 2> eyes{};
  const Quaternion orientation{0.2f, 0.3f, 0.4f, std::sqrt(0.71f)};
  for (unsigned i = 0; i < 2; ++i) {
    eyes[i].orientation = orientation;
    eyes[i].position = rotate(orientation, {i ? 0.032f : -0.032f, 0, 0});
  }
  const auto hud = hudAnchor(eyes);
  const auto trackedForward = rotate(orientation, {0, 0, -1});
  const auto hudForward = rotate(hud.heading, {0, 0, -1});
  for (unsigned a = 0; a < 3; ++a) {
    EXPECT_NEAR(hudForward[a], trackedForward[a], 1e-6f);
    EXPECT_NEAR(hud.origin[a], 0, 1e-6f);
  }
  EXPECT_NEAR(rotate(hud.heading, {1, 0, 0})[1], 0, 1e-6f);
  // A 90-degree look keeps the status panel in front; the cinema stays fixed.
  for (auto &eye : eyes) eye.orientation = yaw(1.57079632679f);
  EXPECT_NEAR(rotate(hudAnchor(eyes).heading, {0, 0, -1})[0], -1, 1e-6f);
  EXPECT_EQ(rotate(Anchor{}.heading, {0, 0, -1})[0], 0);
}
TEST(MgsVrView, HorizontalHudPagesKeepRadarInItsNativeSubrectangle) {
  EXPECT_EQ(mgsHudPage({0,0,319,223}), 0u);
  EXPECT_EQ(mgsHudPage({320,0,639,223}), 320u);
  EXPECT_EQ(mgsHudPage({235,16,303,67}), 0u);
  EXPECT_EQ(mgsHudPage({555,16,623,67}), 320u);
  EXPECT_EQ(mgsHudPage({625,16,693,67},320), 320u); // sliding radar clipped at page edge
  EXPECT_FALSE(mgsHudPage({0,256,319,479})); // texture area, not second framebuffer
  EXPECT_FALSE(mgsHudPage({768,0,1023,223}));
}

TEST(MgsVrView, NativePageBackgroundIsNotHudButRadarAndFadesRemain) {
  // GPU::executeRectangle lowers a native TILE to these post-offset corners.
  // Exercise both horizontal draw pages through the renderer's predicate.
  for (const int page : {0, 320}) {
    replay::DrawPacket clear;
    clear.opcode = 0x60;
    clear.route = replay::PacketRoute::Screen2D;
    clear.vertexCount = 4;
    clear.state.drawArea = {page, 0, page + 319, 223};
    clear.state.drawOffset = {page, 0};
    for (unsigned i = 0; i < 4; ++i) {
      clear.vertices[i].nativeX = page + ((i & 1) ? 320 : 0);
      clear.vertices[i].nativeY = (i & 2) ? 224 : 0;
    }
    ASSERT_TRUE(isMgsHudCandidate(clear));
    EXPECT_TRUE(isMgsHudBackground(clear, page, 320, 224));
    auto polygon = clear;
    polygon.opcode = 0x28;
    polygon.route = replay::PacketRoute::Projected3D; // unproven native 2D polygon
    EXPECT_TRUE(isMgsHudBackground(polygon, page, 320, 224));
    polygon.vertices[3].nativeX -= 1; // not a complete rectangle
    EXPECT_FALSE(isMgsHudBackground(polygon, page, 320, 224));

    auto radar = clear;
    radar.state.drawArea = {page + 235, 16, page + 303, 67};
    radar.state.drawOffset = {page + 269, 42};
    EXPECT_FALSE(isMgsHudBackground(radar, page, 320, 224));
    auto small = clear;
    small.vertices[1].nativeX = small.vertices[3].nativeX = page + 100;
    EXPECT_FALSE(isMgsHudBackground(small, page, 320, 224)); // life/status background
    auto sprite = clear;
    sprite.opcode = 0x64;
    sprite.textured = true;
    EXPECT_FALSE(isMgsHudBackground(sprite, page, 320, 224));
    auto fade = clear;
    fade.opcode = 0x62;
    fade.state.blend.enabled = true;
    fade.state.blend.mode = replay::BlendMode::BackgroundMinusForeground;
    EXPECT_FALSE(isMgsHudBackground(fade, page, 320, 224));
    auto world = polygon;
    world.validMask = 15;
    EXPECT_FALSE(isMgsHudCandidate(world));
    EXPECT_FALSE(isMgsHudBackground(world, page, 320, 224));
  }
}

TEST(MgsVrViewButton, ShortReleasePulsesSelectAndLongHoldConsumesIt) {
  input::VrViewButton view;
  EXPECT_FALSE(view.sample(false,true,0).select);
  EXPECT_FALSE(view.sample(true,true,100000000).select);
  EXPECT_FALSE(view.sample(true,true,200000000).select);
  EXPECT_TRUE(view.sample(false,true,250000000).select);
  EXPECT_TRUE(view.sample(false,true,299999999).select);
  EXPECT_FALSE(view.sample(false,true,300000000).select);
  EXPECT_FALSE(view.sample(true,true,400000000).recenter);
  const auto longHold = view.sample(true,true,1400000000);
  EXPECT_TRUE(longHold.recenter); EXPECT_FALSE(longHold.select);
  EXPECT_FALSE(view.sample(true,true,1700000000).recenter);
  const auto release = view.sample(false,true,1800000000);
  EXPECT_FALSE(release.select); EXPECT_FALSE(release.recenter);
  // A release at the threshold is long even if no frame sampled it earlier.
  view.sample(true,true,2000000000);
  EXPECT_TRUE(view.sample(false,true,3000000000).recenter);
}
TEST(MgsVrViewButton, FocusLossCancelsHoldAndPulseUntilRelease) {
  input::VrViewButton view;
  view.sample(false,true,0); view.sample(true,true,100000000);
  view.sample(false,false,900000000);
  EXPECT_FALSE(view.sample(true,true,1200000000).recenter);
  EXPECT_FALSE(view.sample(true,true,2400000000).recenter);
  EXPECT_FALSE(view.sample(false,true,2500000000).select);
  view.sample(true,true,2600000000);
  EXPECT_TRUE(view.sample(false,true,2700000000).select);
  view.sample(false,false,2710000000);
  EXPECT_FALSE(view.sample(false,true,2720000000).select);
}
TEST(MgsVrViewButton, SelectPulseReachesGuestLatchesAtEveryPresentationRate) {
  constexpr int64_t guestPeriod = 1000000000/60;
  for (int hz : {72,90,120}) for (int phase = 0; phase < 4; ++phase) {
    input::VrViewButton view;
    input::HostInput host;
    input::InputController pad;
    int64_t nextGuest = phase*guestPeriod/4;
    unsigned seen = 0, recentered = 0;
    for (int frame = 0; frame < hz; ++frame) {
      const int64_t time = int64_t(frame)*1000000000/hz;
      const auto result = view.sample(time>=100000000 && time<250000000,true,time);
      host.gamepad = result.select ? input::BTN_SELECT : 0;
      host.apply(pad);
      recentered += result.recenter;
      while (nextGuest <= time) {
        pad.latchVBlank();
        seen += (pad.latchedSample()[0].buttons & input::BTN_SELECT) == 0;
        nextGuest += guestPeriod;
      }
    }
    EXPECT_GE(seen,2u) << "Hz=" << hz << " phase=" << phase;
    EXPECT_EQ(recentered,0u);
  }
}
TEST(MgsVrView, FirstPersonRecenterPreservesWorldHeading) {
  std::array<EyeView,2> eyes{};
  for (auto &eye : eyes) eye.orientation = yaw(-0.8f);
  Anchor anchor; anchor.heading = yaw(0.3f);
  float base = 1.2f, stick = 0.4f, head = 0; // helper samples current head, not a stale delta
  const Vec3 before = rotate(yaw(0.3f+stick), rotate(yaw(-base), {0,0,-1}));
  recenterFirstPerson(anchor,eyes,base,stick,head);
  EXPECT_NEAR(base,-0.3f,1e-6f);
  EXPECT_EQ(stick,0); EXPECT_EQ(head,0);
  const Vec3 after = rotate(anchor.heading, rotate(yaw(-base), {0,0,-1}));
  for (unsigned axis = 0; axis < 3; ++axis) EXPECT_NEAR(after[axis],before[axis],1e-6f);
  // Looking straight up has no forward yaw; the fallback must still preserve
  // the world transform rather than applying a stale head-turn delta.
  const Vec3 beforeVertical = after;
  for (auto &eye : eyes) eye.orientation = {std::sqrt(0.5f),0,0,std::sqrt(0.5f)};
  head = 2.0f;
  recenterFirstPerson(anchor,eyes,base,stick,head);
  const Vec3 afterVertical = rotate(anchor.heading, rotate(yaw(-base), {0,0,-1}));
  for (unsigned axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(afterVertical[axis],beforeVertical[axis],1e-6f);
}

TEST(MgsVrView, MenuSourceGeometryRequiresAuthenticatedGameplayWitness) {
  replay::DrawPacket source;
  source.originalMesh.emplace();
  source.originalMesh->sourceTopology = true;
  source.originalMesh->temporalIdentity = {5,9,0,0};
  source.route = replay::PacketRoute::Projected3D;
  std::vector<replay::Operation> operations{source};
  EXPECT_EQ(mgsPresentation(operations,{}), MgsPresentation::Cinema);
  // Admission is independent of the number or coordinates of menu meshes.
  operations.assign(100,source);
  EXPECT_EQ(mgsPresentation(operations,{}), MgsPresentation::Cinema);
  operations.assign(1,source);
  replay::MgsPlayerView player; player.sceneEpoch = 5; player.bodyLifetime = 9;
  EXPECT_EQ(mgsPresentation(operations,player), MgsPresentation::Gameplay);
  player.sceneEpoch = 6;
  EXPECT_EQ(mgsPresentation(operations,player), MgsPresentation::Cinema);
  player.sceneEpoch = 5;
  replay::DrawPacket codec;
  codec.screenEffect.kind = replay::MgsScreenEffectKind::CodecScreen;
  operations.push_back(codec);
  EXPECT_EQ(mgsPresentation(operations,player), MgsPresentation::Cinema);
  operations.clear();
  EXPECT_EQ(mgsPresentation(operations,{}), MgsPresentation::Reuse);
}

class MgsVrMovement : public ::testing::Test {
protected:
  Memory memory;
  recomp_context ctx{};
  static constexpr uint32_t actor = 0x80100000, control = actor+0x20, name = actor+0x2000;
  void SetUp() override {
    ctx.mem = &memory; ctx.r4 = control; ctx.r18 = actor;
    ctx.r28 = 0x800A0000; ctx.r31 = 0x8005D0FC;
    memory.write32(0x800AE16C, control);
    memory.write32(actor+20, name);
    std::memcpy(memory.ramPtr()+(name & 0x1fffffff),"snake.c",8);
    memory.write32(actor+0x9c4, 0x80054C6C); // retail running handler
    games::mgs::setVrMovement({});
  }
  void TearDown() override { games::mgs::setVrMovement({}); }
};
TEST_F(MgsVrMovement, NativeStepMovesFourWaysWithoutChangingFacingOrPosition) {
  for (unsigned dir = 0; dir < 4096; dir += 1024) {
    input::VrInputSample sample; sample.facing = 0; sample.movementHeading = dir;
    games::mgs::setVrMovement(sample);
    memory.write32(ctx.r28+0x920,dir);
    memory.write16(control+0x44,0); memory.write16(control+0x48,100);
    memory.write16(control+0x46,37); // gravity stays native
    memory.write16(control,1234); memory.write16(control+4,5678);
    memory.write16(control+10,dir); memory.write16(control+0x4e,dir);
    mgsApplyVrLocomotion(&ctx);
    EXPECT_EQ(memory.read32(ctx.r28+0x920),0u);
    mgsApplyVrControl(&ctx);
    const float angle = dir*(2*3.14159265358979323846f/4096);
    EXPECT_EQ(int16_t(memory.read16(control+0x44)), std::lround(std::sin(angle)*100));
    EXPECT_EQ(int16_t(memory.read16(control+0x48)), std::lround(std::cos(angle)*100));
    EXPECT_EQ(memory.read16(control+10),0u); EXPECT_EQ(memory.read16(control+0x4e),0u);
    EXPECT_EQ(memory.read16(control),1234u); EXPECT_EQ(memory.read16(control+4),5678u);
    EXPECT_EQ(memory.read16(control+0x46),37u);
  }
}
TEST_F(MgsVrMovement, ThirdPersonAndAuthoredStatesAreByteExactNoOps) {
  input::VrInputSample first; first.facing = 1024; first.movementHeading = 2048;
  for (unsigned mode = 0; mode < 4; ++mode) {
    games::mgs::setVrMovement(mode ? first : input::VrInputSample{});
    memory.write32(0x800AE0A0,mode==1 ? 1 : 0); // pause
    memory.write32(actor+0x9c4,mode==2 ? 0x80055040 : 0x80054C6C); // wall stance
    ctx.r31 = mode==3 ? 0x80061234 : 0x8005D0FC; // another caller
    const std::vector<uint8_t> before(memory.ramPtr(), memory.ramPtr()+Memory::RAM_SIZE);
    if (mode != 3) mgsApplyVrLocomotion(&ctx);
    mgsApplyVrControl(&ctx);
    EXPECT_EQ(std::memcmp(before.data(),memory.ramPtr(),before.size()),0);
  }
}

TEST_F(MgsVrMovement, WallCameraAdmissionKeepsNativeActionsAndRejectsAuthoredViews) {
  games::mgs::SceneSnapshot scene;
  scene.epoch = 7;
  games::mgs::SceneInstance body;
  body.guestAddress = actor + 0x3000;
  body.lifetime = 12;
  body.visible = true;
  body.models.resize(7);
  body.models[6].world[12] = 100;
  body.models[6].world[13] = 1500;
  body.models[6].world[14] = 200;
  body.models[6].world[0] = body.models[6].world[5] = body.models[6].world[10] = body.models[6].world[15] = 1;
  body.models[6].presentationWorld = body.models[6].world;
  scene.instances.push_back(body);
  memory.write32(control + 0x7c, body.guestAddress);
  memory.write32(0x800AE1C8, 0x10000); // PLAYER_CAUTION
  memory.write32(0x800ADB3C, 0x10); // STATE_BEHIND_CAMERA
  for (uint32_t handler : {0x80055040u, 0x80055134u, 0x800552B8u}) {
    memory.write32(actor + 0x9c4, handler);
    const auto view = games::mgs::capturePlayerView(scene, memory);
    ASSERT_TRUE(view);
    EXPECT_EQ(view->bodyLifetime, body.lifetime);
    EXPECT_EQ(view->eye, (std::array<float, 3>{100,1600,260}));
    // Admission never turns wall handling into modern locomotion.
    input::VrInputSample movement; movement.facing = 1024; movement.movementHeading = 2048;
    games::mgs::setVrMovement(movement);
    const std::vector<uint8_t> before(memory.ramPtr(), memory.ramPtr() + Memory::RAM_SIZE);
    mgsApplyVrLocomotion(&ctx);
    mgsApplyVrControl(&ctx);
    EXPECT_EQ(std::memcmp(before.data(), memory.ramPtr(), before.size()), 0);
  }
  for (uint32_t flag : {0x40u, 0x80000u, 0x4000000u, 0x8000000u, 0x10000000u, 0x40000000u, 0x80000000u}) {
    memory.write32(0x800ADB3C, 0x10 | flag);
    EXPECT_FALSE(games::mgs::capturePlayerView(scene, memory));
  }
  memory.write32(0x800ADB3C, 0x10);
  memory.write32(0x800AE0A0, 1);
  EXPECT_FALSE(games::mgs::capturePlayerView(scene, memory));
  memory.write32(0x800AE0A0, 0);
  memory.write32(0x800AE1C8, 0x10000 | 0x80); // not playable
  EXPECT_FALSE(games::mgs::capturePlayerView(scene, memory));
  memory.write32(0x800AE1C8, 0); // behind camera without wall status
  EXPECT_FALSE(games::mgs::capturePlayerView(scene, memory));
  memory.write32(0x800AE1C8, 0x10000);
  memory.write32(actor + 0x9c4, 0x80054C6C); // normal run is not wall admission
  EXPECT_FALSE(games::mgs::capturePlayerView(scene, memory));
  memory.write32(actor + 0x9c4, 0x80055040);
  memory.write32(actor + 20, 0); // actor authentication is still required
  EXPECT_FALSE(games::mgs::capturePlayerView(scene, memory));
}

TEST_F(MgsVrMovement, EyeOffsetFollowsTheRenderedHeadPose) {
  games::mgs::SceneSnapshot scene;
  games::mgs::SceneInstance body;
  body.guestAddress = actor + 0x3000;
  body.visible = true;
  body.models.resize(7);
  // A 90-degree head yaw rotates local face-forward +Z to world +X.
  // Deliberately different native translation catches mixing the two poses.
  body.models[6].world[12] = -9000;
  body.models[6].presentationWorld = {0,0,-1,0, 0,1,0,0, 1,0,0,0, 100,1500,200,1};
  scene.instances.push_back(body);
  memory.write32(control + 0x7c, body.guestAddress);
  const auto view = games::mgs::capturePlayerView(scene, memory);
  ASSERT_TRUE(view);
  EXPECT_EQ(view->eye, (std::array<float, 3>{160,1600,200}));
  // Native crawl clearance remains an independent world-up adjustment.
  memory.write32(0x800AE1C8, 0x40);
  const auto prone = games::mgs::capturePlayerView(scene, memory);
  ASSERT_TRUE(prone);
  EXPECT_EQ(prone->eye, (std::array<float, 3>{160,1920,200}));
}
