#include "runtime/cpu_context.h"
#include "runtime/games/mgs_vr_input.h"
#include "runtime/games/mgs_mesh_replay.h"
#include "runtime/games/mgs_scene.h"
#include "runtime/memory.h"
#include "runtime/gpu/gpu.h"
#include "runtime/gpu/modern_replay_recorder.h"
#include "runtime/games/mgs_profile.h"
#include "runtime/input/host_input.h"
#include "runtime/input/tas_route.h"
#include "../../ps1Runtime/src/vr/scene_view.h"
#include "../../ps1Runtime/src/vr/cinema_view.h"
#include <gtest/gtest.h>
#include <cstring>
#include <iostream>
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

TEST(MgsVrView, DioramaInitialUpMatchesNativePadAndRouteConvention) {
  // Pinned libgv/pad.c GetPadDir maps Up to2048 before adding PadOrg.
  // These explicit world vectors also pass through production native MOVE.
  const std::array<Vec3,4> nativeUp{{{0,0,-1000},{-1000,0,0},{0,0,1000},{1000,0,0}}};
  for(unsigned quarter=0;quarter<4;++quarter) {
    const uint16_t padOrigin=quarter*1024;
    Toybox stage;stage.place(7,{},Toybox::nativeUpHeading(padOrigin));
    const auto origin=stage.transform({}),destination=stage.transform(nativeUp[quarter]);
    EXPECT_NEAR(destination[0],origin[0],1e-6f);
    EXPECT_GT(destination[1],origin[1]);EXPECT_LT(destination[2],origin[2]);
    const auto stick=input::headRelativeStick(0,1,stage.heading,padOrigin);
    EXPECT_EQ(stick,(std::array<uint8_t,2>{128,1})); // original analog Up
    Memory memory;memory.write32(0x800AE16C,0x80100000);memory.write32(0x800ADAA8,padOrigin);
    input::TasRoute route;std::string error;
    const std::string script="MGSROUTE 1\nSTEP native_up 100 1\nUNTIL story eq 1 4294967295\nMOVE "+
        std::to_string(int(nativeUp[quarter][0]))+" "+std::to_string(int(nativeUp[quarter][2]))+"\nEND\n";
    ASSERT_TRUE(route.parse(script,error))<<error;
    EXPECT_EQ(route.sample(memory.ramPtr(),Memory::RAM_SIZE,1)[0],input::BTN_UP);
  }
}

TEST(MgsVrView, DioramaUsesOneFixedViewBasisForStageAndNativePad) {
  constexpr float pi=3.14159265358979323846f;
  for(float heading:{0.0f,pi/2,pi,-pi/2}) {
    Toybox stage;stage.place(7,{1200,400,-600},heading);
    const auto origin=stage.transform(stage.centre);
    const auto up=stage.transform({1200,2200,-600});
    EXPECT_GT(up[1],origin[1]); // Snake stands visibly above the tilted table
    EXPECT_GT(up[2],origin[2]);
    EXPECT_NEAR(std::hypot(up[1]-origin[1],up[2]-origin[2]),0.216f,1e-5f);
    for(const auto stick:std::array<std::array<float,2>,4>{{{0,1},{1,0},{0,-1},{-1,0}}}) {
      const float angle=heading-std::atan2(stick[0],stick[1]);
      const Vec3 destination{1200+1000*std::sin(angle),400,-600+1000*std::cos(angle)};
      const auto shown=stage.transform(destination);
      EXPECT_NEAR(shown[0]-origin[0],stick[0]*Toybox::scale*1000,1e-5f);
      EXPECT_NEAR(shown[1]-origin[1],stick[1]*Toybox::scale*1000*std::sqrt(0.5f),1e-5f);
      for(uint16_t padOrigin:{0,1024,2048,3072}) {
        const auto pad=input::headRelativeStick(stick[0],stick[1],stage.heading,padOrigin);
        const float nativeAngle=std::atan2(float(pad[0])-128,float(pad[1])-128)+padOrigin*(2*pi/4096);
        EXPECT_NEAR(std::sin(nativeAngle),std::sin(angle),0.01f);
        EXPECT_NEAR(std::cos(nativeAngle),std::cos(angle),0.01f);
        // Same production merge as main_host: raw digital directions must not
        // override the compensated analog route, while actions stay native.
        input::HostInput host;input::InputController controller;
        input::VrInputSample vr;vr.analogMovement=true;vr.x=pad[0];vr.y=pad[1];
        vr.buttons=input::BTN_SQUARE | (stick[0]>0?input::BTN_RIGHT:stick[0]<0?input::BTN_LEFT:
            stick[1]>0?input::BTN_UP:input::BTN_DOWN);
        host.gamepad=vr.buttons;host.apply(controller,false,&vr);controller.latchVBlank();
        const auto routed=controller.latchedSample()[0];
        EXPECT_EQ(routed.padType,input::PadType::Analog);
        EXPECT_EQ(routed.analogLX,pad[0]);EXPECT_EQ(routed.analogLY,pad[1]);
        EXPECT_EQ(routed.buttons&0xf0,0xf0); // active-low, no unrotated D-pad
        EXPECT_EQ(routed.buttons&input::BTN_SQUARE,0);
        EXPECT_FALSE(vr.facing);EXPECT_FALSE(vr.movementHeading);
        vr.analogMovement=false;host.apply(controller,false,&vr);controller.latchVBlank();
        EXPECT_EQ(controller.latchedSample()[0].padType,input::PadType::Digital);
        EXPECT_EQ(controller.latchedSample()[0].buttons&vr.buttons,0); // menus stay native
      }
    }
    const auto before=stage.transform({1400,600,900});
    stage.place(7,{5000,300,-4000},heading+pi); // movement/native camera cut
    EXPECT_EQ(stage.heading,heading);EXPECT_EQ(stage.transform({1400,600,900}),before);
  }
}

TEST(MgsVrView, NativeRadarTile1ReplaysAtTheSamePixelOnBothPages) {
  struct Cleanup {~Cleanup(){games::mgs::configure(false,false);}} cleanup;
  games::mgs::configure(true,false);
  for(uint32_t page:{0u,320u}) for(bool blinkOn:{false,true}) {
    gpu::GPU native,captured;
    captured.configureReplay({true,4,true,true,replay::ReplayDepthMode::Hybrid});
    for(auto *gpu:{&native,&captured}) {
      gpu->writeGP0(0xE3000000u|(16u<<10)|(page+235));
      gpu->writeGP0(0xE4000000u|(67u<<10)|(page+303));
      gpu->writeGP0(0xE5000000u|(42u<<11)|(page+269));
      // Native radar.c TILE1 source: local0,0 and C8C8C8, only on blink frames.
      if(blinkOn) { gpu->writeGP0(0x68C8C8C8);gpu->writeGP0(0); }
    }
    EXPECT_EQ(native.getVRAM()[42*1024+page+269].raw,captured.getVRAM()[42*1024+page+269].raw);
    EXPECT_EQ(native.getVRAM()[42*1024+page+269].raw!=0,blinkOn);
    captured.snapshotDisplayBuffer(1);
    const auto frame=captured.tryConsumeReplay();ASSERT_TRUE(frame);
    EXPECT_EQ(mgsHasPanelContent(frame->operations(),320,224),blinkOn);
    unsigned points=0;
    for(const auto &op:frame->operations()) if(const auto *paint=std::get_if<replay::PaintRect>(&op)) {
      ++points;
      const auto packet=mgsHudPaintRect(*paint,320,224);ASSERT_TRUE(packet);
      EXPECT_EQ(packet->vertices[0].nativeX,int(page+269));
      EXPECT_EQ(packet->vertices[0].nativeY,42);
      EXPECT_EQ(packet->vertices[3].nativeX,int(page+270));
      EXPECT_EQ(packet->vertices[3].nativeY,43);
      EXPECT_EQ(packet->vertices[0].color,(replay::Rgb8{200,200,200}));
      EXPECT_EQ(mgsHudPage(packet->state.drawArea),page);
      EXPECT_EQ(packet->vertices[0].x-page,269); // no second drawOffset subtraction
    }
    EXPECT_EQ(points,blinkOn?1u:0u);
  }
}

TEST(MgsVrView, RecordedPaintRectBackgroundIsNotAWorldHudPlate) {
  for(uint32_t page:{0u,320u}) {
    replay::CrashReplayRecorder recorder({true,4,true,true,replay::ReplayDepthMode::Hybrid});
    std::vector<gpu::Color16> vram(replay::CrashReplayRecorder::NATIVE_PIXEL_COUNT);
    recorder.recordSurfaceSeed(vram);
    for(const replay::NativeRect rect: {replay::NativeRect{int32_t(page),0,320,224},
        replay::NativeRect{int32_t(page+235),16,69,52}}) {
      replay::PaintRect paint;paint.destination=rect;
      paint.depth.invalidateOnAccepted=true;paint.coalescedCommandCount=uint64_t(rect.width)*rect.height;
      recorder.observeOpcode(0x68);recorder.recordPaintRect(0x68,paint);
    }
    const auto frame=recorder.sealAndPublish({});ASSERT_TRUE(frame);
    EXPECT_TRUE(mgsHasPanelContent(frame->operations(),320,224));
    unsigned seen=0;
    for(const auto &op:frame->operations()) if(const auto *paint=std::get_if<replay::PaintRect>(&op)) {
      const auto packet=mgsHudPaintRect(*paint,320,224);ASSERT_TRUE(packet);
      EXPECT_EQ(isMgsHudBackground(*packet,page,320,224),seen==0);
      EXPECT_FALSE(isMgsEyeOverlay(*packet,page,320,224));
      ++seen;
    }
    EXPECT_EQ(seen,2u);
  }
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

TEST(MgsVrView, CapturedHeliportFadeCoversEyesButRadarStaysOnHud) {
  // s01a/story18, native Slot 1, latch1625: fadeio.c actor0x16e8e0.
  // The other TILE at235,16..304,68 is the radar background, not this fade.
  for (const int page : {0, 320}) {
    replay::DrawPacket fade;
    fade.opcode = 0x62;
    fade.route = replay::PacketRoute::Screen2D;
    fade.vertexCount = 4;
    fade.state.drawArea = {page, 0, page + 319, 223};
    fade.state.drawOffset = {page, 0};
    fade.state.blend.enabled = true;
    fade.state.blend.mode = replay::BlendMode::BackgroundMinusForeground;
    fade.screenEffect.kind = replay::MgsScreenEffectKind::FadeBlack;
    for (unsigned i = 0; i < 4; ++i) {
      auto &v = fade.vertices[i];
      v.x = v.nativeX = page + ((i & 1) ? 320 : 0);
      v.y = v.nativeY = (i & 2) ? 224 : 0;
      v.color = {12,12,12};
      const auto clip = mgsEyeOverlayPosition(v.x, v.y, page, 320, 224);
      EXPECT_EQ(clip, (Vec3{(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, 0}));
    }
    EXPECT_TRUE(isMgsHudCandidate(fade));
    EXPECT_FALSE(isMgsHudBackground(fade, page, 320, 224));
    EXPECT_TRUE(isMgsEyeOverlay(fade, page, 320, 224));
    auto partial = fade;
    partial.vertices[1].nativeX = partial.vertices[3].nativeX = page + 319;
    EXPECT_FALSE(isMgsEyeOverlay(partial, page, 320, 224));
    partial = fade;
    partial.vertices[2].nativeY = partial.vertices[3].nativeY = 223;
    EXPECT_FALSE(isMgsEyeOverlay(partial, page, 320, 224));
    auto radar = fade;
    radar.screenEffect = {};
    radar.state.drawArea = {page + 235, 16, page + 303, 67};
    radar.state.blend.mode = replay::BlendMode::HalfBackgroundPlusHalfForeground;
    for (unsigned i = 0; i < 4; ++i) {
      radar.vertices[i].nativeX = page + ((i & 1) ? 304 : 235);
      radar.vertices[i].nativeY = (i & 2) ? 68 : 16;
      radar.vertices[i].color = {0,24,24};
    }
    EXPECT_TRUE(isMgsHudCandidate(radar));
    EXPECT_FALSE(isMgsEyeOverlay(radar, page, 320, 224));
    auto texture = fade;
    texture.opcode = 0x2e;
    texture.textured = true;
    EXPECT_TRUE(isMgsHudCandidate(texture));
    EXPECT_FALSE(isMgsEyeOverlay(texture, page, 320, 224));
    auto masked = fade;
    masked.state.mask.checkMaskBit = true;
    EXPECT_FALSE(isMgsEyeOverlay(masked, page, 320, 224));
    auto panorama = texture;
    panorama.surfaceEffect.kind = replay::MgsSurfaceKind::Panorama;
    EXPECT_FALSE(isMgsHudCandidate(panorama));
  }
}

TEST(MgsVrView, EyeOverlayPipelineRetainsNativeBlendOperators) {
  const std::array<MgsEyeOverlayBlend,4> expected{{{0.5f,0.5f,false}, {1,1,false}, {1,1,true}, {0.25f,1,false}}};
  for (unsigned i = 0; i < expected.size(); ++i) {
    const auto blend = mgsEyeOverlayBlend(replay::BlendMode(i));
    EXPECT_EQ(blend.foreground, expected[i].foreground);
    EXPECT_EQ(blend.background, expected[i].background);
    EXPECT_EQ(blend.subtract, expected[i].subtract);
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
    EXPECT_EQ(view->eye, (std::array<float, 3>{100,1550,260}));
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
  memory.write32(actor + 0x9c4, 0x80055044); // unknown adjacent callback is not wall admission
  EXPECT_FALSE(games::mgs::capturePlayerView(scene, memory));
  memory.write32(actor + 0x9c4, 0x80055040);
  memory.write32(actor + 20, 0); // actor authentication is still required
  EXPECT_FALSE(games::mgs::capturePlayerView(scene, memory));
}

TEST_F(MgsVrMovement, GameplayPauseMenusKeepWorldWhileCodecAndScriptsRemainCinema) {
  games::mgs::SceneSnapshot scene;scene.epoch=5;
  games::mgs::SceneInstance body;
  body.guestAddress=actor+0x3000;body.lifetime=9;body.visible=true;body.models.resize(7);
  body.models[6].presentationWorld={1,0,0,0,0,1,0,0,0,0,1,0,100,1500,200,1};
  scene.instances.push_back(body);memory.write32(control+0x7c,body.guestAddress);
  replay::DrawPacket world;world.route=replay::PacketRoute::Projected3D;
  world.originalMesh.emplace();world.originalMesh->sourceTopology=true;
  world.originalMesh->temporalIdentity[0]=scene.epoch;
  std::vector<replay::Operation> operations{world};
  for(uint32_t pause:{0u,2u,4u,6u}) {
    memory.write32(0x800AE0A0,pause);
    const auto player=games::mgs::capturePlayerView(scene,memory);ASSERT_TRUE(player);
    EXPECT_EQ(player->paused,pause!=0);
    EXPECT_EQ(mgsPresentation(operations,player),MgsPresentation::Gameplay);
    replay::DrawPacket codec;codec.screenEffect.kind=replay::MgsScreenEffectKind::CodecScreen;
    auto call=operations;call.push_back(codec);
    EXPECT_EQ(mgsPresentation(call,player),MgsPresentation::Cinema);
  }
  for(uint32_t pause:{1u,8u,12u}) {
    memory.write32(0x800AE0A0,pause);
    const auto player=games::mgs::capturePlayerView(scene,memory);
    EXPECT_FALSE(player);EXPECT_EQ(mgsPresentation(operations,player),MgsPresentation::Cinema);
  }
  memory.write32(0x800AE0A0,4);
  for(uint32_t scripted:{0x4000000u,0x8000000u,0x10000000u,0x40000000u,0x80000000u}) {
    memory.write32(0x800ADB3C,scripted);
    const auto player=games::mgs::capturePlayerView(scene,memory);
    EXPECT_FALSE(player);EXPECT_EQ(mgsPresentation(operations,player),MgsPresentation::Cinema);
  }
  EXPECT_EQ(mgsPresentation(operations,{}),MgsPresentation::Cinema); // title source models, no Snake
}

TEST_F(MgsVrMovement, OrdinaryCameraHintsKeepWorldWithoutChangingNativeInput) {
  games::mgs::SceneSnapshot scene;scene.epoch=5;
  games::mgs::SceneInstance body;body.guestAddress=actor+0x3000;body.lifetime=9;
  body.visible=true;body.models.resize(7);
  body.models[6].presentationWorld={1,0,0,0,0,1,0,0,0,0,1,0,100,1500,200,1};
  scene.instances.push_back(body);memory.write32(control+0x7c,body.guestAddress);
  constexpr std::array<uint32_t,8> entry{0x27bdffd8,0xafb00018,0x00808021,0x24040800,
      0xafbf0020,0x0c0142c1,0xafb1001c,0x1440000b};
  for(unsigned i=0;i<entry.size();++i)memory.write32(0x800599AC+i*4,entry[i]);
  replay::DrawPacket world;world.originalMesh.emplace();world.originalMesh->sourceTopology=true;
  world.originalMesh->temporalIdentity[0]=scene.epoch;
  std::vector<replay::Operation> operations{world};
  input::VrInputSample movement;movement.facing=1024;movement.movementHeading=2048;
  games::mgs::setVrMovement(movement);games::mgs::setVrWeaponPose({});
  memory.write32(ctx.r28+0x920,3072);memory.write16(control+10,123);
  memory.write16(control+0x44,60);memory.write16(control+0x48,80);
  for(uint32_t handler:{0x80054854u,0x800549D8u,0x80054B50u,0x80054C6Cu,
      0x80054E80u,0x80055B6Cu,0x800599ACu}) for(uint32_t pause:{0u,2u,4u})
      for(uint32_t hints:{0x10u,0x40u,0x80000u,0x80050u}) {
    SCOPED_TRACE(handler);
    SCOPED_TRACE(pause);
    SCOPED_TRACE(hints);
    memory.write32(actor+0x9c4,handler);memory.write32(0x800AE0A0,pause);
    memory.write32(0x800ADB3C,hints);memory.write32(0x800AE1C8,handler==0x800599AC?0x800:0x10);
    const auto player=games::mgs::capturePlayerView(scene,memory);ASSERT_TRUE(player);
    EXPECT_EQ(mgsPresentation(operations,player),MgsPresentation::Gameplay);
    EXPECT_EQ(player->paused,pause!=0);
    const std::vector<uint8_t> before(memory.ramPtr(),memory.ramPtr()+Memory::RAM_SIZE);
    mgsApplyVrLocomotion(&ctx);mgsApplyVrControl(&ctx);
    EXPECT_EQ(std::memcmp(before.data(),memory.ramPtr(),before.size()),0);
    replay::DrawPacket codec;codec.screenEffect.kind=replay::MgsScreenEffectKind::CodecScreen;
    auto call=operations;call.push_back(codec);
    EXPECT_EQ(mgsPresentation(call,player),MgsPresentation::Cinema);
  }
  // Same hints plus a real authored/game-over flag still reject ordinary Snake.
  memory.write32(actor+0x9c4,0x80054C6C);memory.write32(0x800AE0A0,0);
  for(uint32_t flag:{0x4000000u,0x8000000u,0x10000000u,0x40000000u,0x80000000u}) {
    memory.write32(0x800ADB3C,0x80050|flag);
    EXPECT_FALSE(games::mgs::capturePlayerView(scene,memory));
  }
  memory.write32(0x800ADB3C,0x80050);
  for(uint32_t status:{0x80u,0x200u,0x2000u,0x20000000u}) {
    memory.write32(0x800AE1C8,status);
    EXPECT_FALSE(games::mgs::capturePlayerView(scene,memory));
  }
  memory.write32(0x800AE1C8,0);
  for(uint32_t handler:{0x800599A8u,0x80055040u,0x800599ACu}) {
    memory.write32(actor+0x9c4,handler);memory.write16(0x800B752C,2); // not SOCOM/FAMAS
    const auto unknown=games::mgs::capturePlayerView(scene,memory);
    EXPECT_FALSE(unknown);EXPECT_EQ(mgsPresentation(operations,unknown),MgsPresentation::Cinema);
  }
  memory.write16(0x800B752C,0);memory.write32(0x800599AC,0); // wrong retail entry
  EXPECT_FALSE(games::mgs::capturePlayerView(scene,memory));
  // Authentic continuation's first rejected state was death, not alert onset.
  memory.write32(actor+0x9c4,0x80054C6C);
  memory.write32(0x800ADB3C,0x10086000);memory.write32(0x800AE1C8,0x20c08100);
  const auto dead=games::mgs::capturePlayerView(scene,memory);
  EXPECT_FALSE(dead);EXPECT_EQ(mgsPresentation(operations,dead),MgsPresentation::Cinema);
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
  EXPECT_EQ(view->eye, (std::array<float, 3>{160,1550,200}));
  // Native crawl clearance remains an independent world-up adjustment.
  memory.write32(0x800AE1C8, 0x40);
  const auto prone = games::mgs::capturePlayerView(scene, memory);
  ASSERT_TRUE(prone);
  EXPECT_EQ(prone->eye, (std::array<float, 3>{160,1870,200}));
}

#include "../../ps1Runtime/src/vr/arm_ik.h"
namespace {
replay::MgsArmPose testArm() {
  replay::MgsArmPose a;
  a.owners={100,200,300};
  for(auto &m:a.world)m=arm::identity();
  a.world[0][12]=200; a.world[0][13]=1400;
  a.world[1][12]=200; a.world[1][13]=1100;
  a.world[2][12]=200; a.world[2][13]=800;
  return a;
}
}
TEST(MgsVrArms, ReachAndRollPreserveSegmentLengthsAndRigidAttachments) {
  const auto native=testArm();
  for(const Vec3 target : {Vec3{200,1400,1200},Vec3{-250,1300,200},
      Vec3{200,2100,30},Vec3{200,500,20},Vec3{200,1400,0}}) {
    arm::State state;
    arm::Target hand; hand.tracked=true;hand.wrist=arm::identity();
    for(unsigned axis=0;axis<3;++axis)hand.wrist[12+axis]=target[axis];
    arm::Solution solution;
    for(unsigned tick=0;tick<20;++tick)solution=arm::solve(native,hand,{-1,-1.5f,0},1.0f/120,state);
    ASSERT_TRUE(solution.active);
    EXPECT_NEAR(arm::length(arm::sub(solution.elbow,arm::position(native.world[0]))),300,0.001);
    EXPECT_NEAR(arm::length(arm::sub(solution.wrist,solution.elbow)),300,0.001);
    EXPECT_LE(arm::length(arm::sub(solution.wrist,arm::position(native.world[0]))),597.001f);
    const Vec3 before=solution.elbow;
    // Rolling the wrist cannot choose a different elbow plane.
    hand.wrist[0]=-1;hand.wrist[5]=-1;
    solution=arm::solve(native,hand,{-1,-1.5f,0},1.0f/120,state);
    for(unsigned axis=0;axis<3;++axis)EXPECT_NEAR(solution.elbow[axis],before[axis],0.001);
    const Vec3 localMuzzle{20,-370,60};
    const auto rendered=arm::point(solution.changes[2],arm::point(native.world[2],localMuzzle));
    const auto attached=arm::point(solution.world[2],localMuzzle);
    for(unsigned axis=0;axis<3;++axis)EXPECT_NEAR(rendered[axis],attached[axis],0.001);
  }
}
TEST(MgsVrArms, ContinuousTargetsDoNotFlipElbowsAndLossReturnsToNative) {
  const auto native=testArm();arm::State state;arm::Target hand;hand.tracked=true;
  Vec3 previous{};bool havePrevious=false;
  for(unsigned tick=0;tick<720;++tick) {
    const float angle=float(tick)*0.00872664626f;
    hand.wrist=arm::identity();hand.wrist[12]=200+450*std::sin(angle);
    hand.wrist[13]=1400+450*std::cos(angle);hand.wrist[14]=100;
    const auto solution=arm::solve(native,hand,{-1,-1.5f,0},1.0f/120,state);
    ASSERT_TRUE(solution.active);
    if(havePrevious && tick>20)EXPECT_LT(arm::length(arm::sub(previous,solution.elbow)),15);
    previous=solution.elbow;havePrevious=true;
  }
  hand.tracked=false;
  for(unsigned tick=0;tick<16;++tick)arm::solve(native,hand,{-1,-1.5f,0},1.0f/120,state);
  EXPECT_EQ(state.weight,0);EXPECT_FALSE(state.hasPole);
  EXPECT_FALSE(arm::solve(native,hand,{-1,-1.5f,0},1.0f/120,state).active);
  hand.tracked=true;hand.wrist[12]=INFINITY;
  EXPECT_FALSE(arm::solve(native,hand,{-1,-1.5f,0},1.0f/120,state).active);
  hand.wrist=arm::identity();hand.wrist[12]=100000;
  EXPECT_FALSE(arm::solve(native,hand,{-1,-1.5f,0},1.0f/120,state).active);
  hand.wrist=native.world[2];arm::solve(native,hand,{-1,-1.5f,0},1.0f/120,state);
  EXPECT_FALSE(arm::solve({},hand,{-1,-1.5f,0},1.0f/120,state).active);
  EXPECT_EQ(state.weight,0); // authored context is an immediate native reset
}
TEST(MgsVrArms, GripTransformUsesTheRenderedCameraAndRecenterBasis) {
  Anchor anchor;anchor.origin={0.2f,1.3f,-0.5f};anchor.heading=yaw(0.4f);
  const Vec3 eye{1200,1600,-700};const float base=0.7f,turn=-0.2f;
  const Vec3 grip{0.4f,1.1f,-0.8f};const Quaternion q=yaw(0.8f);
  const auto target=arm::controllerTarget(grip,q,anchor,base,turn,eye,true);
  Vec3 relative=arm::scale(arm::sub(arm::position(target.wrist),eye),0.001f);
  const float c=std::cos(base),s=std::sin(base);
  const Vec3 camera{-c*relative[0]+s*relative[2],relative[1],-s*relative[0]-c*relative[2]};
  const auto room=arm::add(rotate(yaw(0.4f+turn),camera),anchor.origin);
  for(unsigned axis=0;axis<3;++axis)EXPECT_NEAR(room[axis],grip[axis],1e-6);
  EXPECT_NEAR(arm::length(arm::vector(target.wrist,{0,-1,0})),1,1e-6);
}
TEST(MgsVrArms, ProductionCornerTransformKeepsSeamsAndAttachmentsTogether) {
  std::array<float,3> source{20,30,40};
  const auto bytes=std::span(reinterpret_cast<const uint8_t *>(source.data()),sizeof(source));
  replay::OriginalMeshGeometry mesh;
  mesh.temporalCamera=arm::identity();
  for(unsigned corner=0;corner<4;++corner)for(unsigned row=0;row<3;++row)
    mesh.viewRows[corner*3+row][row]=row==1?58.0f/64:1;
  mesh.vrPoseOwners={100,200,300,300};
  std::map<uint32_t,Matrix> changes;
  changes[100]=arm::identity();changes[100][12]=10;
  changes[200]=arm::identity();changes[200][12]=-20;
  changes[300]=arm::identity();changes[300][13]=100;
  Vec3 result{};
  ASSERT_TRUE(sourceMeshPosition(bytes,mesh,2,result,false,&changes));
  EXPECT_EQ(result,(Vec3{20,130,40}));
  // A subdivided/seam lane blends independently transformed source corners.
  mesh.weights[0]={0.5f,0.5f,0,0};
  ASSERT_TRUE(sourceMeshPosition(bytes,mesh,0,result,false,&changes));
  EXPECT_EQ(result,(Vec3{15,30,40}));
  changes.clear(); // focus/context reset restores the original geometry
  ASSERT_TRUE(sourceMeshPosition(bytes,mesh,0,result,false,&changes));
  EXPECT_EQ(result,source);
  mesh.vrPoseOwners[2]=0; // unrelated instance never takes the wrist delta
  changes[300]=arm::identity();changes[300][13]=100;
  ASSERT_TRUE(sourceMeshPosition(bytes,mesh,2,result,false,&changes));
  EXPECT_EQ(result,source);
}
TEST_F(MgsVrMovement, ArmCaptureAuthenticatesHierarchyAndLeavesGuestMemoryAlone) {
  games::mgs::SceneSnapshot scene;
  games::mgs::SceneInstance body;
  body.guestAddress=actor+0x3000;body.definition=actor+0x4000;
  body.visible=true;body.primaryModels=16;body.models.resize(16);
  body.pose.source=games::mgs::ScenePoseSource::Rotations;
  for(unsigned joint=0;joint<16;++joint) {
    body.models[joint].guestAddress=body.guestAddress+72+joint*92;
    body.models[joint].presentationWorld=arm::identity();
    memory.write32(body.definition+76+joint*88,joint?joint-1:~0u);
  }
  scene.instances.push_back(body);memory.write32(control+0x7c,body.guestAddress);
  const std::vector<uint8_t> before(memory.ramPtr(),memory.ramPtr()+Memory::RAM_SIZE);
  games::mgs::setMeshReplayStereo(true);
  const auto view=games::mgs::capturePlayerView(scene,memory);
  ASSERT_TRUE(view);
  EXPECT_EQ(view->arms[0].owners[0],body.models[2].guestAddress);
  EXPECT_EQ(view->arms[0].owners[2],body.models[4].guestAddress);
  EXPECT_EQ(view->arms[1].owners[0],body.models[7].guestAddress);
  EXPECT_EQ(view->arms[1].owners[2],body.models[9].guestAddress);
  EXPECT_EQ(std::memcmp(before.data(),memory.ramPtr(),before.size()),0);
  memory.write32(actor+0x9c4,0x80055040); // authored wall action: camera may remain, arms don't
  const auto authored=games::mgs::capturePlayerView(scene,memory);
  ASSERT_TRUE(authored);EXPECT_EQ(authored->arms[0].owners[0],0u);
  memory.write32(actor+0x9c4,0x80054C6C);
  memory.write32(body.definition+76+4*88,4); // cyclic hand parent
  const auto invalid=games::mgs::capturePlayerView(scene,memory);
  ASSERT_TRUE(invalid);EXPECT_EQ(invalid->arms[0].owners[0],0u);
  games::mgs::setMeshReplayStereo(false);
  const auto flat=games::mgs::capturePlayerView(scene,memory);
  ASSERT_TRUE(flat);EXPECT_EQ(flat->arms[1].owners[0],0u);
}

extern void (*g_testRecompDispatchHook)(recomp_context *,uint32_t);
namespace {
unsigned hazardCalls=0;
int hazardMode=0;
void testHazardDispatch(recomp_context *query,uint32_t address) {
  EXPECT_EQ(address,0x80029974u);
  EXPECT_EQ(query->r7,15u);
  EXPECT_EQ(query->mem->read32(query->r29+16),0u); // no wall attribute is skipped
  EXPECT_GT(query->callbackDrainBlockDepth,0u);
  EXPECT_EQ(query->nativeEntryPc,0u);EXPECT_EQ(query->nativeResumePc,0u);
  ++hazardCalls;
  const bool hit=hazardMode==2||(hazardMode==1&&hazardCalls==1);
  // Exercise all audited native work regions and copied CPU/GTE state.
  query->mem->write32(query->r29-120,0x12345678);
  query->mem->write32(query->r28+0x81c,0x55555555);
  query->mem->write32(0x1f8003fc,0x87654321);
  if(hit) {
    for(unsigned axis=0;axis<3;++axis) {
      const int from=int16_t(query->mem->read16(query->r5+axis*2));
      const int to=int16_t(query->mem->read16(query->r6+axis*2));
      const uint32_t offset=axis==1?0x58:(axis==2?0x56:0x54);
      query->mem->write16(0x1f800000+offset,uint16_t(int16_t((from+to)/2)));
    }
  }
  query->r16=0xabababab;query->cop2c[7]=0xbcbcbcbc;query->r2=hit;
}
bool wallHitIsFloor=false;
constexpr uint32_t testWallAddress=0x801e8000;
void testWallDispatch(recomp_context *query,uint32_t address) {
  EXPECT_EQ(address,0x80029974u);
  ++hazardCalls;
  auto &memory=*query->mem;
  Vec3 from{},to{};
  for(unsigned axis=0;axis<3;++axis) {
    from[axis]=int16_t(memory.read16(query->r5+axis*2));
    to[axis]=int16_t(memory.read16(query->r6+axis*2));
  }
  const float plane=int16_t(memory.read16(testWallAddress+2));
  query->r2=from[2]<plane && to[2]>=plane;
  if(query->r2) {
    const auto hit=arm::add(from,arm::scale(arm::sub(to,from),(plane-from[2])/(to[2]-from[2])));
    for(unsigned axis=0;axis<3;++axis) {
      const uint32_t offset=axis==1?0x58:(axis==2?0x56:0x54);
      memory.write16(0x1f800000+offset,uint16_t(int16_t(std::lround(hit[axis]))));
    }
    memory.write32(0x1f800064,wallHitIsFloor?testWallAddress&0x7fffffff:testWallAddress);
  }
}
void writeTestMatrix(Memory &memory,uint32_t address,const Matrix &m) {
  for(unsigned r=0;r<3;++r) {
    for(unsigned c=0;c<3;++c)memory.write16(address+(r*3+c)*2,uint16_t(int16_t(std::lround(m[c*4+r]*4096))));
    memory.write32(address+20+r*4,uint32_t(int32_t(std::lround(m[12+r]))));
  }
}
}
TEST_F(MgsVrMovement, RetailFirearmHeadYawChangesOnlyTwoRotationFields) {
  constexpr uint32_t liveControl=0x8016ca80,liveActor=liveControl-0x20;
  constexpr std::array<uint32_t,8> entry{0x27bdffd8,0xafb00018,0x00808021,0x24040800,
      0xafbf0020,0x0c0142c1,0xafb1001c,0x1440000b};
  memory.write32(0x800AE16C,liveControl);memory.write32(liveActor+20,name);
  for(unsigned i=0;i<entry.size();++i)memory.write32(0x800599AC+i*4,entry[i]);
  // Distinct native position, step, roll and pad direction witnesses.
  for(unsigned offset=0;offset<0x70;offset+=2)memory.write16(liveControl+offset,0x1200+offset);
  ctx.r18=liveActor;ctx.r4=liveControl;ctx.r28=0x800ad9fc;ctx.r29=0x800aeac0;
  memory.write32(ctx.r28+0x920,2048);
  games::mgs::setVrWeaponPose({});games::mgs::setVrShotDirection({});
  for(unsigned mode=0;mode<10;++mode) {
    SCOPED_TRACE(mode);
    const bool admitted=mode<3; // SOCOM, FAMAS, normal shoot-helper exit
    memory.write32(liveActor+0x9c4,mode==4?0x80055040u:0x800599ACu);
    memory.write16(0x800B752C,mode==1?1:mode==3?2:0);
    memory.write32(0x800AE1C8,mode==2?0:mode==5?0x880u:0x800u);
    memory.write32(0x800ADB3C,mode==6?0x40u:0);
    memory.write32(0x800AE0A0,mode==7?4:0);
    memory.write32(0x800599AC,mode==8?0:entry[0]);
    ctx.r31=mode==9?0x8005D100u:0x8005D0FCu;
    for(const auto aim:std::array<std::array<int,2>,5>{{{1024,256},{1024,-384},{1024,0},{3072,-384},{0,0}}}) {
      SCOPED_TRACE(aim[0]);
      input::VrInputSample sample;sample.facing=aim[0];sample.aimPitch=aim[1];sample.movementHeading=2048;
      games::mgs::setVrMovement(sample);
      const std::vector<uint8_t> before(memory.ramPtr(),memory.ramPtr()+Memory::RAM_SIZE);
      mgsApplyVrLocomotion(&ctx);
      ASSERT_EQ(std::memcmp(before.data(),memory.ramPtr(),before.size()),0);
      auto expected=before;
      if(admitted) for(const auto field:std::array<std::array<unsigned,2>,2>{{
          {10,uint16_t(aim[0])},{0x4e,uint16_t(aim[0])}}}) {
        const auto offset=(liveControl&0x1fffffff)+field[0];
        expected[offset]=uint8_t(field[1]);expected[offset+1]=uint8_t(field[1]>>8);
      }
      mgsApplyVrControl(&ctx);
      EXPECT_EQ(std::memcmp(expected.data(),memory.ramPtr(),expected.size()),0);
      // Production supplies no shot override. Native gun orientation and
      // translation survive the actual authenticated NewBullet hook exactly.
      const auto returnPc=ctx.r31;
      ctx.r4=ctx.r29+16;ctx.r5=1;ctx.r31=mode==1?0x800682D8:0x80067B7C;
      auto shot=arm::identity();shot[12]=321;shot[13]=654;shot[14]=-987;
      writeTestMatrix(memory,ctx.r4,shot);
      const std::vector<uint8_t> beforeShot(memory.ramPtr(),memory.ramPtr()+Memory::RAM_SIZE);
      mgsApplyVrBulletAim(&ctx);
      EXPECT_EQ(std::memcmp(beforeShot.data(),memory.ramPtr(),beforeShot.size()),0);
      ctx.r4=liveControl;ctx.r31=returnPc;
    }
  }
}

TEST_F(MgsVrMovement, RetailFirearmFiringKeepsTrackedArmsAndShotWithoutOverridingMovement) {
  // Actual USA candidate5 shot16082 identity/handler/status, not a locomotion
  // callback substituted to make the weapon bridge pass.
  constexpr uint32_t liveControl=0x8016ca80, liveActor=liveControl-0x20,
      liveBody=0x8016d4d0, liveMap=0x800ba078, liveHzd=0x801575a0;
  const uint32_t hand=liveBody+72+4*92;
  memory.write32(0x800AE16C,liveControl);memory.write32(liveActor+20,name);
  memory.write32(liveControl+0x7c,liveBody);memory.write16(liveBody+46,16);
  memory.write32(liveControl+0x2c,liveMap);memory.write32(liveMap+8,liveHzd);
  constexpr std::array<uint32_t,8> entry{0x27bdffc0,0xafbf003c,0xafbe0038,0xafb70034,0xafb60030,0xafb5002c,0xafb40028,0xafb30024};
  for(unsigned i=0;i<entry.size();++i)memory.write32(0x80029974+i*4,entry[i]);
  constexpr std::array<uint32_t,8> fireEntry{0x27bdffd8,0xafb00018,0x00808021,0x24040800,0xafbf0020,0x0c0142c1,0xafb1001c,0x1440000b};
  for(unsigned i=0;i<fireEntry.size();++i)memory.write32(0x800599AC+i*4,fireEntry[i]);
  const auto native=testArm();
  writeTestMatrix(memory,hand,native.world[2]);
  games::mgs::SceneSnapshot scene;
  games::mgs::SceneInstance body;
  body.guestAddress=liveBody;body.definition=0x80180000;body.visible=true;
  body.primaryModels=16;body.models.resize(16);body.pose.source=games::mgs::ScenePoseSource::Rotations;
  for(unsigned joint=0;joint<16;++joint) {
    body.models[joint].guestAddress=liveBody+72+joint*92;
    body.models[joint].presentationWorld=joint>=2&&joint<=4?native.world[joint-2]:arm::identity();
    memory.write32(body.definition+76+joint*88,joint?joint-1:~0u);
  }
  scene.instances.push_back(body);
  input::VrInputSample movement;movement.facing=1024;movement.aimPitch=0;movement.movementHeading=2048;
  games::mgs::setVrMovement(movement);
  games::mgs::setMeshReplayStereo(true);
  g_testRecompDispatchHook=testHazardDispatch;
  struct Cleanup {
    ~Cleanup() { games::mgs::setVrWeaponPose({});games::mgs::setMeshReplayStereo(false);g_testRecompDispatchHook=nullptr; }
  } cleanup;
  ctx.r28=0x800ad9fc;ctx.r29=0x800aeac0;ctx.r18=liveActor;
  Matrix desired=native.world[2];desired[12]+=100;
  for(unsigned mode=0;mode<7;++mode) {
    SCOPED_TRACE(mode);
    const bool admitted=mode<2;
    games::mgs::setVrWeaponPose({});
    memory.write32(liveActor+0x9c4,mode==2?0x80055040u:mode==3?0x800599A8u:0x800599ACu);
    memory.write32(0x800AE1C8,mode==4?0x880u:0x800u);
    memory.write16(0x800B752C,mode==1?1:mode==5?2:0);
    memory.write32(0x800599AC,mode==6?0:fireEntry[0]);
    const auto view=games::mgs::capturePlayerView(scene,memory);
    if(admitted) { ASSERT_TRUE(view);EXPECT_EQ(view->arms[0].owners[2],hand); }
    else EXPECT_TRUE(!view || view->arms[0].owners[2]==0);
    // The firing hook follows head rotation without redirecting native step.
    ctx.r4=liveControl;ctx.r31=0x8005D0FC;
    const std::vector<uint8_t> beforeMove(memory.ramPtr(),memory.ramPtr()+Memory::RAM_SIZE);
    mgsApplyVrLocomotion(&ctx);
    EXPECT_EQ(std::memcmp(beforeMove.data(),memory.ramPtr(),beforeMove.size()),0);
    auto expectedMove=beforeMove;
    if(admitted) for(unsigned offset:{10u,0x4eu}) {
      const uint16_t value=1024;
      expectedMove[(liveControl&0x1fffffff)+offset]=uint8_t(value);
      expectedMove[(liveControl&0x1fffffff)+offset+1]=uint8_t(value>>8);
    }
    mgsApplyVrControl(&ctx);
    EXPECT_EQ(std::memcmp(expectedMove.data(),memory.ramPtr(),expectedMove.size()),0);
    // Publish even on rejected cases to prove the guest boundary fails closed
    // if an old renderer sample races an authored-action transition.
    games::mgs::setVrWeaponPose({hand,desired,native.world});
    ctx.r4=ctx.r29+16;ctx.r5=1;ctx.r31=mode==1?0x800682D8:0x80067B7C;
    writeTestMatrix(memory,ctx.r4,native.world[2]);hazardMode=0;hazardCalls=0;
    mgsApplyVrBulletAim(&ctx);
    EXPECT_EQ(hazardCalls,admitted?1u:0u);
    EXPECT_EQ(int32_t(memory.read32(ctx.r4+20)),std::lround(native.world[2][12])+(admitted?100:0));
    EXPECT_EQ(games::mgs::vrWeaponConstraint().nativeFallback,!admitted);
    if(admitted) {
      // The same shooting state can refresh a validated offset without a shot.
      ctx.r4=liveControl;ctx.r31=0x8005D0FC;hazardCalls=0;
      const std::vector<uint8_t> beforeRefresh(memory.ramPtr(),memory.ramPtr()+Memory::RAM_SIZE);
      mgsApplyVrControl(&ctx);
      EXPECT_EQ(hazardCalls,1u);
      EXPECT_EQ(std::memcmp(beforeRefresh.data(),memory.ramPtr(),beforeRefresh.size()),0);
      // Real shoot-helper exit: ATTACK clears while the same callback is
      // installed, then it switches to idle. Keep one learned shot offset
      // through every producer capture/publication and control refresh.
      for(unsigned transition=0;transition<3;++transition) {
        SCOPED_TRACE(transition);
        memory.write32(0x800AE1C8,transition==0?0x800u:0u);
        memory.write32(liveActor+0x9c4,transition==2?0x80054854u:0x800599ACu);
        const auto continuing=games::mgs::capturePlayerView(scene,memory);
        ASSERT_TRUE(continuing);
        ASSERT_EQ(continuing->arms[0].owners[2],hand);
        // This is the renderer's production handoff for a still-active arm.
        games::mgs::setVrWeaponPose({continuing->arms[0].owners[2],desired,continuing->arms[0].world});
        hazardMode=1;hazardCalls=0;
        mgsApplyVrControl(&ctx);
        EXPECT_EQ(hazardCalls,2u); // obstruction and clear near-side recheck
        const auto constrained=games::mgs::vrWeaponConstraint();
        EXPECT_FALSE(constrained.nativeFallback);
        EXPECT_EQ(constrained.pose.wristOwner,hand);
        EXPECT_LT(constrained.pose.wristWorld[12],desired[12]);
      }
      // Recover from the obstruction while idle, without another NewBullet.
      hazardMode=0;hazardCalls=0;mgsApplyVrControl(&ctx);
      EXPECT_EQ(hazardCalls,1u);
      EXPECT_FALSE(games::mgs::vrWeaponConstraint().nativeFallback);
      EXPECT_EQ(games::mgs::vrWeaponConstraint().pose.wristOwner,0u);
    }
  }
}

TEST_F(MgsVrMovement, WallContactPreservesTangentAndCrossesContinuously) {
  const uint32_t body=actor+0x3000,hand=body+72+4*92,map=actor+0x5000;
  memory.write32(control+0x7c,body);memory.write16(body+46,16);
  memory.write32(control+0x2c,map);memory.write32(map+8,actor+0x6000);
  memory.write16(0x800B752C,0);
  constexpr std::array<uint32_t,8> entry{0x27bdffc0,0xafbf003c,0xafbe0038,0xafb70034,0xafb60030,0xafb5002c,0xafb40028,0xafb30024};
  for(unsigned i=0;i<entry.size();++i)memory.write32(0x80029974+i*4,entry[i]);
  // A vertical native HZD_SEG, with its authenticated x,z,y,height layout.
  memory.write16(testWallAddress,uint16_t(-6000));memory.write16(testWallAddress+2,uint16_t(-1000));
  memory.write16(testWallAddress+8,uint16_t(-4000));memory.write16(testWallAddress+10,uint16_t(-1000));
  input::VrInputSample movement;movement.facing=0;games::mgs::setVrMovement(movement);
  games::mgs::setVrWeaponPose({});
  g_testRecompDispatchHook=testWallDispatch;wallHitIsFloor=false;
  struct Cleanup {
    ~Cleanup() { games::mgs::setVrWeaponPose({});g_testRecompDispatchHook=nullptr;wallHitIsFloor=false; }
  } cleanup;
  ctx.r29=0x801f0000;
  bool first=true;
  const auto resolve=[&](Vec3 from,Vec3 requested) {
    auto native=testArm();
    for(unsigned joint=0;joint<3;++joint) {
      native.world[joint]=arm::identity();
      const auto p=arm::add(from,joint==0?Vec3{-300,0,0}:joint==1?Vec3{-150,300,0}:Vec3{});
      for(unsigned axis=0;axis<3;++axis)native.world[joint][12+axis]=p[axis];
    }
    writeTestMatrix(memory,hand,native.world[2]);
    Matrix desired=arm::identity();
    for(unsigned axis=0;axis<3;++axis)desired[12+axis]=requested[axis];
    games::mgs::setVrWeaponPose({hand,desired,native.world});
    hazardCalls=0;
    if(first) {
      first=false;ctx.r4=ctx.r29+16;ctx.r5=1;ctx.r31=0x80067B7C;
      auto shot=native.world[2];shot[13]-=400; // muzzle coincides with wrist
      writeTestMatrix(memory,ctx.r4,shot);mgsApplyVrBulletAim(&ctx);
    } else { ctx.r4=control;ctx.r31=0x8005D0FC;mgsApplyVrControl(&ctx); }
    const auto constraint=games::mgs::vrWeaponConstraint();
    EXPECT_FALSE(constraint.nativeFallback);
    EXPECT_EQ(hazardCalls,std::lround(requested[2])>=-1000?2u:1u);
    return constraint.pose.wristOwner?arm::position(constraint.pose.wristWorld):requested;
  };
  // Candidate7 exact recovery request/native-source records. The first shot
  // learns one offset; every later sample uses the production control refresh.
  struct Contact { Vec3 from,to; };
  const std::array<Contact,14> capture{{
    {{-5455,1462,-1205},{-5392,1174,-970}},{{-5451,1461,-1214},{-5387,1174,-971}},
    {{-5453,1457,-1229},{-5377,1173,-973}},{{-5455,1456,-1247},{-5368,1173,-977}},
    {{-5447,1456,-1274},{-5359,1172,-979}},{{-5446,1458,-1304},{-5349,1171,-979}},
    {{-5446,1459,-1332},{-5326,1170,-990}},{{-5425,1439,-1365},{-5276,1161,-1023}},
    {{-5484,1434,-1144},{-5276,1161,-1023}},{{-5518,1387,-1109},{-5389,1176,-975}},
    {{-5541,1351,-1109},{-5376,1173,-979}},{{-5562,1318,-1133},{-5368,1170,-993}},
    {{-5585,1286,-1170},{-5358,1168,-1022}},{{-5580,1271,-1279},{-5311,1157,-1164}}
  }};
  float maxTangent=0;
  for(const auto &sample:capture) {
    const auto accepted=resolve(sample.from,sample.to);
    const float tangent=std::hypot(accepted[0]-sample.to[0],accepted[1]-sample.to[1]);
    maxTangent=std::max(maxTangent,tangent);
    EXPECT_LT(tangent,0.1f); // camera translation cancels in this correction
    EXPECT_LE(accepted[2],-1001);
  }
  // Dense approach/release around contact: no fixed inset jump. The bound is
  // one native coordinate quantum plus this quarter-unit requested step.
  Vec3 previous{};bool havePrevious=false;float maxStep=0;
  for(int i=0;i<=64;++i) {
    const float z=-1004+0.25f*(i<=32?i:64-i);
    const auto accepted=resolve({-5400,1400,-1200},{-5350,1200,z});
    EXPECT_LT(accepted[2],-1000);
    if(havePrevious) {
      const float step=arm::length(arm::sub(accepted,previous));
      maxStep=std::max(maxStep,step);EXPECT_LE(step,1.25f);
    }
    previous=accepted;havePrevious=true;
  }
  // Floor-tagged pointers must never be decoded as a vertical wall plane.
  wallHitIsFloor=true;
  const auto floor=resolve(capture[0].from,capture[0].to);
  EXPECT_GT(std::hypot(floor[0]-capture[0].to[0],floor[1]-capture[0].to[1]),1.0f);
  std::cout<<"[wall-contact] tangent_max="<<maxTangent<<" dense_step_max="<<maxStep<<'\n';
}

TEST_F(MgsVrMovement, NativeShotUsesWristAndRestoresHazardWorkRegions) {
  const uint32_t body=actor+0x3000,hand=body+72+4*92,map=actor+0x5000;
  memory.write32(control+0x7c,body);memory.write16(body+46,16);
  memory.write32(control+0x2c,map);memory.write32(map+8,actor+0x6000);
  memory.write16(0x800B752C,0);
  constexpr std::array<uint32_t,8> entry{0x27bdffc0,0xafbf003c,0xafbe0038,0xafb70034,0xafb60030,0xafb5002c,0xafb40028,0xafb30024};
  for(unsigned i=0;i<entry.size();++i)memory.write32(0x80029974+i*4,entry[i]);
  auto native=testArm();writeTestMatrix(memory,hand,native.world[2]);
  Matrix desired=arm::identity();desired[12]=400;desired[13]=1000;desired[14]=100;
  input::VrInputSample movement;movement.facing=0;games::mgs::setVrMovement(movement);
  games::mgs::setVrWeaponPose({hand,desired,native.world});
  ctx.r29=0x801f0000;ctx.r4=ctx.r29+16;ctx.r5=1;ctx.r31=0x80067B7C;
  Matrix shot=native.world[2];shot[12]+=20;shot[13]-=370;shot[14]+=60;
  writeTestMatrix(memory,ctx.r4,shot);
  const auto before=ctx;
  std::vector<uint8_t> ram(memory.ramPtr(),memory.ramPtr()+Memory::RAM_SIZE);
  const std::vector<uint8_t> scratch(memory.scratchpadPtr(),memory.scratchpadPtr()+Memory::SCRATCHPAD_SIZE);
  g_testRecompDispatchHook=testHazardDispatch;hazardMode=0;hazardCalls=0;
  mgsApplyVrBulletAim(&ctx);
  EXPECT_EQ(hazardCalls,1u);
  EXPECT_EQ(std::memcmp(&ctx,&before,sizeof(ctx)),0);
  EXPECT_EQ(std::memcmp(scratch.data(),memory.scratchpadPtr(),scratch.size()),0);
  EXPECT_EQ(int32_t(memory.read32(ctx.r4+20)),420);
  EXPECT_EQ(int32_t(memory.read32(ctx.r4+24)),630);
  EXPECT_EQ(int32_t(memory.read32(ctx.r4+28)),160);
  // The only changed architectural RAM is the caller-owned shot matrix.
  std::memcpy(ram.data()+(ctx.r4&0x1fffffff),memory.ramPtr()+(ctx.r4&0x1fffffff),32);
  EXPECT_EQ(std::memcmp(ram.data(),memory.ramPtr(),ram.size()),0);
  EXPECT_FALSE(games::mgs::vrWeaponConstraint().nativeFallback);

  writeTestMatrix(memory,ctx.r4,shot);hazardMode=1;hazardCalls=0;
  mgsApplyVrBulletAim(&ctx);
  EXPECT_EQ(hazardCalls,2u); // clamp and recheck after solving reach limits
  const auto clamped=games::mgs::vrWeaponConstraint();
  EXPECT_FALSE(clamped.nativeFallback);EXPECT_EQ(clamped.pose.wristOwner,hand);
  EXPECT_LT(clamped.pose.wristWorld[12],desired[12]);
  EXPECT_NEAR(int32_t(memory.read32(ctx.r4+20)),clamped.pose.wristWorld[12]+20,0.51);

  // A failed second check restores the native gun and original shot together.
  writeTestMatrix(memory,ctx.r4,shot);hazardMode=2;hazardCalls=0;
  mgsApplyVrBulletAim(&ctx);
  EXPECT_TRUE(games::mgs::vrWeaponConstraint().nativeFallback);
  EXPECT_EQ(int32_t(memory.read32(ctx.r4+20)),220);
  EXPECT_EQ(int32_t(memory.read32(ctx.r4+24)),430);
  // Moving clear refreshes through normal control without firing another shot.
  hazardMode=0;hazardCalls=0;ctx.r4=control;ctx.r31=0x8005D0FC;
  mgsApplyVrControl(&ctx);
  EXPECT_EQ(hazardCalls,1u);
  EXPECT_FALSE(games::mgs::vrWeaponConstraint().nativeFallback);
  EXPECT_EQ(games::mgs::vrWeaponConstraint().pose.wristOwner,0u);

  // Entry authentication fails closed, without calling unverified code.
  ctx.r4=ctx.r29+16;ctx.r31=0x80067B7C;memory.write32(0x80029974,0);
  hazardCalls=0;mgsApplyVrBulletAim(&ctx);EXPECT_EQ(hazardCalls,0u);
  EXPECT_TRUE(games::mgs::vrWeaponConstraint().nativeFallback);
  // This is the same production API reset used by beginFrame on focus loss,
  // including the standby path where no present/updateArms call follows.
  games::mgs::setVrWeaponPose({});
  EXPECT_FALSE(games::mgs::vrWeaponConstraint().nativeFallback);
  EXPECT_EQ(games::mgs::vrWeaponConstraint().pose.wristOwner,0u);
  games::mgs::setVrShotDirection({});
  writeTestMatrix(memory,ctx.r4,shot);hazardCalls=0;
  mgsApplyVrBulletAim(&ctx);
  EXPECT_EQ(hazardCalls,0u); // no stale published wrist can move this shot
  EXPECT_EQ(int32_t(memory.read32(ctx.r4+20)),220);
  // Reacquisition cannot revive the pre-focus weapon offset/constraint.
  memory.write32(0x80029974,entry[0]);
  games::mgs::setVrWeaponPose({hand,desired,native.world});
  ctx.r4=control;ctx.r31=0x8005D0FC;hazardCalls=0;
  mgsApplyVrControl(&ctx);
  EXPECT_EQ(hazardCalls,0u);
  EXPECT_FALSE(games::mgs::vrWeaponConstraint().nativeFallback);
  EXPECT_EQ(games::mgs::vrWeaponConstraint().pose.wristOwner,0u);
  ctx.r4=ctx.r29+16;ctx.r31=0x80067B7C;
  mgsApplyVrBulletAim(&ctx);EXPECT_EQ(hazardCalls,1u);
  EXPECT_EQ(int32_t(memory.read32(ctx.r4+20)),420);
  games::mgs::setVrWeaponPose({});g_testRecompDispatchHook=nullptr;
}

TEST(MgsVrArms, TrackingFadeKeepsBentJointsAndVerticesContinuousThroughNativeReturn) {
  constexpr float fadeSeconds=0.12f;
  constexpr float pi=3.14159265358979323846f;
  const std::array<Vec3,4> localVertices{{{60,0,0},{0,50,0},{0,0,70},{15,-80,25}}};
  for(bool bent:{true,false}) for(int hz:{72,90,120}) {
    SCOPED_TRACE(::testing::Message()<<"bent="<<bent<<" hz="<<hz);
    auto native=testArm();
    if(bent) {
      native.world[1][12]=380;native.world[1][13]=1160;
      native.world[2][12]=200;native.world[2][13]=920;
    }
    // Non-identity authored bases exercise segment orientation, not just
    // endpoint positions. Each source mesh keeps this actual native basis.
    for(unsigned joint=0;joint<3;++joint) {
      const auto origin=arm::position(native.world[joint]);
      const auto q=yaw(0.3f+float(joint)*0.4f);
      native.world[joint]=arm::frame(rotate(q,{1,0,0}),rotate(q,{0,1,0}),origin);
    }
    const float upper=arm::length(arm::sub(arm::position(native.world[1]),arm::position(native.world[0])));
    const float lower=arm::length(arm::sub(arm::position(native.world[2]),arm::position(native.world[1])));
    const float dt=1.0f/float(hz);
    // A shortest rigid rotation can travel at most pi over the fade. This
    // geometric speed bound includes the final frame; no jump allowance.
    const float angleStep=pi*dt/fadeSeconds;
    const float jointStepBound=(upper+lower)*angleStep;
    const float vertexStepBound=jointStepBound+85*angleStep;
    arm::State state;arm::Target hand;hand.tracked=true;
    std::array<Matrix,3> previous=native.world;
    std::array<std::array<Vec3,4>,3> previousVertices{};
    for(unsigned joint=0;joint<3;++joint)for(unsigned v=0;v<localVertices.size();++v)
      previousVertices[joint][v]=arm::point(native.world[joint],localVertices[v]);
    float maxJointStep=0,maxVertexStep=0,finalJointStep=0,finalVertexStep=0;
    bool wasActive=false,returnedToNative=false;
    const auto observe=[&](const arm::Solution &solution,bool fading) {
      const auto &current=solution.active?solution.world:native.world;
      float jointStep=0,vertexStep=0;
      for(unsigned joint=0;joint<3;++joint) {
        jointStep=std::max(jointStep,arm::length(arm::sub(arm::position(current[joint]),arm::position(previous[joint]))));
        for(unsigned v=0;v<localVertices.size();++v) {
          const auto nativePoint=arm::point(native.world[joint],localVertices[v]);
          const auto posed=solution.active?arm::point(solution.changes[joint],nativePoint):nativePoint;
          vertexStep=std::max(vertexStep,arm::length(arm::sub(posed,previousVertices[joint][v])));
          previousVertices[joint][v]=posed;
        }
      }
      maxJointStep=std::max(maxJointStep,jointStep);maxVertexStep=std::max(maxVertexStep,vertexStep);
      if(fading) {
        EXPECT_LE(jointStep,jointStepBound+0.001f)<<"weight="<<state.weight;
        EXPECT_LE(vertexStep,vertexStepBound+0.001f)<<"weight="<<state.weight;
      }
      if(wasActive&&!solution.active) {
        returnedToNative=true;finalJointStep=jointStep;finalVertexStep=vertexStep;
      }
      EXPECT_NEAR(arm::length(arm::sub(arm::position(current[1]),arm::position(current[0]))),upper,0.001f);
      EXPECT_NEAR(arm::length(arm::sub(arm::position(current[2]),arm::position(current[1]))),lower,0.001f);
      previous=current;wasActive=solution.active;
    };
    // Acquire at one target, then transport the elbow across and around the
    // shoulder. The last tracked pole is deliberately unlike the native bend.
    const Vec3 shoulder=arm::position(native.world[0]);
    for(unsigned frame=0;frame<360;++frame) {
      const float angle=frame<30?0:float(frame-30)*(4.7f/329);
      const Vec3 target{shoulder[0]+450*std::cos(angle),shoulder[1]+450*std::sin(angle),shoulder[2]+180};
      const float roll=angle*0.3f;
      hand.wrist=arm::frame({std::cos(roll),std::sin(roll),0},{-std::sin(roll),std::cos(roll),0},target);
      const auto solution=arm::solve(native,hand,{-1,-1.5f,0},dt,state);
      ASSERT_TRUE(solution.active);
      observe(solution,frame<30);
    }
    hand.tracked=false;
    for(unsigned frame=0;frame<30;++frame) {
      const auto solution=arm::solve(native,hand,{-1,-1.5f,0},dt,state);
      observe(solution,true);
      // The remaining distance must vanish with the remaining fade weight,
      // including mesh vertices that expose residual rotation/twist.
      if(solution.active) for(unsigned joint=0;joint<3;++joint) {
        const float reach=joint==0?0:(joint==1?upper:upper+lower);
        EXPECT_LE(arm::length(arm::sub(arm::position(solution.world[joint]),arm::position(native.world[joint]))),reach*pi*state.weight+0.001f);
        for(const auto vertex:localVertices)
          EXPECT_LE(arm::length(arm::sub(arm::point(solution.changes[joint],arm::point(native.world[joint],vertex)),arm::point(native.world[joint],vertex))),
                    (reach+arm::length(vertex))*pi*state.weight+0.001f);
      }
    }
    EXPECT_TRUE(returnedToNative);EXPECT_EQ(state.weight,0);
    std::cout<<"[arm-fade] bent="<<bent<<" hz="<<hz
      <<" max_joint_step="<<maxJointStep<<" max_vertex_step="<<maxVertexStep
      <<" final_joint_step="<<finalJointStep<<" final_vertex_step="<<finalVertexStep<<'\n';
  }
}

TEST(MgsVrArms, StraightNativeArmHasNoResidualBendAsTrackingWeightVanishes) {
  const auto native=testArm();
  arm::State state;arm::Target hand;hand.tracked=true;
  hand.wrist=arm::identity();hand.wrist[12]=-200;hand.wrist[13]=1200;hand.wrist[14]=300;
  for(unsigned frame=0;frame<20;++frame)arm::solve(native,hand,{-1,-1.5f,0},1.0f/120,state);
  hand.tracked=false;
  // Inspect the limit independently of display cadence. A fixed reach cap
  // on a blended target leaves about 30 units of bend even at tiny weights.
  for(float weight:{0.01f,0.001f,0.0001f}) {
    state.weight=weight;
    const auto solution=arm::solve(native,hand,{-1,-1.5f,0},0,state);
    ASSERT_TRUE(solution.active);
    const float elbowError=arm::length(arm::sub(solution.elbow,arm::position(native.world[1])));
    const float wristError=arm::length(arm::sub(solution.wrist,arm::position(native.world[2])));
    EXPECT_LE(elbowError,300*3.14159265358979323846f*weight+0.001f);
    EXPECT_LE(wristError,600*3.14159265358979323846f*weight+0.001f);
    std::cout<<"[arm-fade-limit] weight="<<weight<<" elbow_error="<<elbowError<<" wrist_error="<<wristError<<'\n';
  }
  const auto final=arm::solve(native,hand,{-1,-1.5f,0},1.0f/120,state);
  EXPECT_FALSE(final.active);EXPECT_EQ(state.weight,0);
}

TEST(MgsVrCinema, AuthoredCameraPreservesCompositionWithPositiveBoundedStereoDepth) {
  CinemaView cinema;
  cinema.source.projection=320;
  cinema.source.sourceView={0,0,-1,0, 0,1,0,0, 1,0,0,0, 400,-120,800,1};
  for(bool wide:{false,true}) {
    cinema.source.widescreen=wide;
    for(float z:{1.f,100.f,1500.f,10000.f,100000.f}) {
      const Vec3 world{800-z,220,100};
      const auto camera=cinema.camera(world),picture=cinema.picture(camera);
      EXPECT_NEAR(camera[0],wide?375:500,1e-5);
      EXPECT_NEAR(camera[1],100*58.f/64,1e-5);
      EXPECT_FLOAT_EQ(camera[2],z);
      EXPECT_GT(-picture[2],CinemaView::distance);
      EXPECT_LT(-picture[2],CinemaView::distance+CinemaView::depthRange);
      // Project back onto the physical window: exactly the original native
      // pixel composition, independently of the depth compression.
      const float x=160+picture[0]*2/(-picture[2])*320/1.9f;
      const float y=112-picture[1]*2/(-picture[2])*224/1.06875f;
      EXPECT_NEAR(x,160+camera[0]*320/z,0.02);
      EXPECT_NEAR(y,112+camera[1]*320/z,0.02);
      const float leftEye=-.032f,rightEye=.032f;
      // At z=1 this fixture projects hundreds of metres outside the window.
      // Its ~21 micrometre disparity is smaller than one float ULP there.
      // Measure the actual float vertices in double precision, rather than
      // rounding two independent ray intersections to the same float value.
      const auto atWindow=[&](double eye) {
        return eye+(double(picture[0])-eye)*2/double(-picture[2]);
      };
      EXPECT_LT(atWindow(leftEye),atWindow(rightEye)); // always behind screen
      EXPECT_GT(atWindow(rightEye)-atWindow(leftEye),0);
      EXPECT_LT(atWindow(rightEye)-atWindow(leftEye),.064f/3+.00001f);
      const double geometricDisparity=(double(rightEye)-leftEye)*(1-2/double(-picture[2]));
      EXPECT_NEAR(atWindow(rightEye)-atWindow(leftEye),geometricDisparity,1e-10);
      // Visible points must retain the same strict guarantee in production
      // float precision, including the centre and both sides of the picture.
      for(float horizontal:{-.9f,0.f,.9f}) {
        const auto visible=cinema.picture({horizontal*z*.5f,0,z});
        const auto visibleAtWindow=[&](float eye) {
          return eye+(visible[0]-eye)*2/(-visible[2]);
        };
        EXPECT_LT(visibleAtWindow(leftEye),visibleAtWindow(rightEye));
        EXPECT_GT(visibleAtWindow(rightEye)-visibleAtWindow(leftEye),0);
        EXPECT_LT(visibleAtWindow(rightEye)-visibleAtWindow(leftEye),.064f/3+.00001f);
        EXPECT_LT(std::abs(visibleAtWindow(leftEye)),CinemaView::width/2);
        EXPECT_LT(std::abs(visibleAtWindow(rightEye)),CinemaView::width/2);
      }
    }
  }
  // q=d/z cancels the compressed physical depth for native UV/shade
  // interpolation. Without it a sloped textured face swims across camera cuts.
  const float z0=200,z1=8000,b=.4f,u0=10,u1=220;
  const float q0=cinema.perspective(z0),q1=cinema.perspective(z1);
  const float rendered=((1-b)*u0*q0/cinema.depth(z0)+b*u1*q1/cinema.depth(z1))/
      ((1-b)*q0/cinema.depth(z0)+b*q1/cinema.depth(z1));
  const float native=((1-b)*u0/z0+b*u1/z1)/((1-b)/z0+b/z1);
  EXPECT_NEAR(rendered,native,1e-5);
}

TEST(MgsVrCinema, NativeCaptionBandIsAdjacentPageIndependentAndBelowBothPictureFormats) {
  for(float height:{CinemaView::height,CinemaView::height*160/224}) {
    for(uint32_t page:{0u,320u}) {
      const auto centre=CinemaView::caption(page+160,204,page,height);
      EXPECT_FLOAT_EQ(centre[0],0);
      EXPECT_FLOAT_EQ(centre[1],-height/2-CinemaView::captionHeight/2);
      EXPECT_FLOAT_EQ(centre[2],-2);
      // Native font's maximum 38 rows and one-pixel outline fit the band.
      const auto top=CinemaView::caption(page+40,184,page,height);
      const auto bottom=CinemaView::caption(page+280,224,page,height);
      EXPECT_LT(top[1],-height/2);
      EXPECT_GT(bottom[1],-height/2-CinemaView::captionHeight);
    }
  }
}

TEST(MgsVrCinema, CinemaRequiresLiveProducerAndMatchingCameraWhileCodecAlwaysWins) {
  replay::DrawPacket source;source.originalMesh.emplace();
  source.originalMesh->sourceTopology=true;source.originalMesh->temporalIdentity={7,3,0,0};
  std::vector<replay::Operation> operations{source};
  replay::MgsCinemaView cinema;cinema.actor=0x100000;cinema.sceneEpoch=7;cinema.projection=320;
  EXPECT_EQ(mgsPresentation(operations,{},cinema),MgsPresentation::StereoCinema);
  cinema.sceneEpoch=8;
  EXPECT_EQ(mgsPresentation(operations,{},cinema),MgsPresentation::Cinema);
  cinema.sceneEpoch=7;cinema.actor=0;
  EXPECT_EQ(mgsPresentation(operations,{},cinema),MgsPresentation::Cinema);
  cinema.actor=0x100000;cinema.projection=0;
  EXPECT_EQ(mgsPresentation(operations,{},cinema),MgsPresentation::Cinema);
  cinema.projection=320;
  EXPECT_EQ(mgsPresentation({}, {},cinema),MgsPresentation::Reuse); // paused DMO camera
  cinema.kind=replay::MgsCinemaKind::Movie;cinema.moviePicture={0,24,320,160};
  EXPECT_EQ(mgsPresentation({}, {},cinema),MgsPresentation::MovieCinema);
  replay::DrawPacket codec;codec.screenEffect.kind=replay::MgsScreenEffectKind::CodecScreen;
  operations.push_back(codec);
  EXPECT_EQ(mgsPresentation(operations,{},cinema),MgsPresentation::Cinema);
  EXPECT_EQ(mgsPresentation(operations,{}),MgsPresentation::Cinema);
}

TEST(MgsVrCinema, ProductionNearClipKeepsFiniteTexturedCrossingFaces) {
  struct Vertex {Vec3 position;std::array<float,2> uv;Vec3 shade;};
  std::vector<Vertex> triangle{{{-3,0,-1},{0,0},{0,0,0}},{{3,0,3},{8,0},{1,1,1}},{{0,3,3},{8,8},{1,1,1}}};
  const auto clipped=CinemaView::clipNear(triangle);
  ASSERT_EQ(clipped.size(),4u);
  CinemaView cinema;cinema.source.projection=320;
  unsigned intersections=0;
  for(const auto &v:clipped) {
    EXPECT_GE(v.position[2],1);
    for(float component:cinema.picture(v.position)) EXPECT_TRUE(std::isfinite(component));
    if(v.position[2]==1) {
      ++intersections;EXPECT_FLOAT_EQ(v.uv[0],4);EXPECT_FLOAT_EQ(v.shade[0],.5);
    }
  }
  EXPECT_EQ(intersections,2u);
  for(auto &v:triangle)v.position[2]=-1;
  EXPECT_TRUE(CinemaView::clipNear(triangle).empty());
}

TEST(MgsVrCinema, RetainedPictureUsesVisiblePixelsAndStillRejectsNativeContextChanges) {
  std::vector<gpu::Color16> vram(1024*512);
  gpu::DisplayScanoutPlan plan;plan.activeExtent={320,224};
  for(unsigned page:{0u,320u})vram[50*1024+page+100].raw=0x1234;
  const auto original=CinemaPicture::capture(vram,plan);
  vram[400*1024+900].raw=0x7fff; // offscreen glyph/texture upload
  EXPECT_EQ(CinemaPicture::capture(vram,plan),original);
  vram[50*1024+100].raw|=0x8000; // mask bit is not visible
  EXPECT_EQ(CinemaPicture::capture(vram,plan),original);
  plan.sourceByteX=640; // identical native display in the other framebuffer
  EXPECT_EQ(CinemaPicture::capture(vram,plan),original);
  vram[50*1024+420].raw^=1;EXPECT_NE(CinemaPicture::capture(vram,plan),original);
  plan.enabled=false;EXPECT_TRUE(CinemaPicture::capture(vram,plan).pixels.empty());
  replay::MgsCinemaView previous;previous.actor=0x15c3d8;
  replay::FrameLatch latch;
  EXPECT_TRUE(retainCinema(MgsPresentation::Reuse,latch,previous,true));
  EXPECT_FALSE(retainCinema(MgsPresentation::Reuse,latch,previous,false));
  for(auto presentation:{MgsPresentation::Cinema,MgsPresentation::Gameplay})
    EXPECT_FALSE(retainCinema(presentation,latch,previous,true)); // title/Codec/live Snake
  latch.mgsPresentationObserved=true;
  EXPECT_FALSE(retainCinema(MgsPresentation::Reuse,latch,previous,true)); // explicit end
  latch.mgsCinemaView=previous;
  EXPECT_TRUE(retainCinema(MgsPresentation::Reuse,latch,previous,false)); // authenticated new UI/caption
  latch.mgsCinemaView->actor++;
  EXPECT_FALSE(retainCinema(MgsPresentation::Reuse,latch,previous,true));
}

TEST(MgsVrCinema, NativeWaterHalvesMapToTheSamePhysicalSourcePlaneForEachEye) {
  replay::DrawPacket left,right;
  for(uint32_t page:{0u,320u}) {
    left.state.texturePage.raw=uint16_t(0x100+page/64);
    right.state.texturePage.raw=uint16_t(0x100+(page+128)/64);
    left.vertices[1].u=160;right.vertices[0].u=32;
    left.vertices[1].v=right.vertices[0].v=112;
    const auto a=cinemaWaterSourcePosition(left,1,page),b=cinemaWaterSourcePosition(right,0,page);
    EXPECT_EQ(a,(Vec3{0,0,-2}));EXPECT_EQ(a,b);
    EyeView eye;eye.left=eye.down=-1;eye.right=eye.up=1;
    for(float x:{-.032f,.032f}) {
      eye.position={x,0,0};const auto m=eyeMatrix({},eye);
      const float clipX=m[0]*a[0]+m[4]*a[1]+m[8]*a[2]+m[12];
      const float clipW=m[3]*a[0]+m[7]*a[1]+m[11]*a[2]+m[15];
      EXPECT_NEAR(clipX/clipW,-x/2,1e-7);
    }
  }
}
