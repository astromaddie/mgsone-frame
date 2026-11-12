#pragma once
#include "eye_view.h"
#include "scene_view.h"
#include "runtime/gpu/display_geometry.h"
#include "runtime/gpu/modern_replay.h"
#include <vector>

namespace ps1::gpu::vr {
// Keep a cutscene through native frames which submit no new ordering table,
// but only while the actual displayed picture is unchanged. A fresh ownerless
// OT is an explicit context exit even if its pixels happen to be identical.
inline bool retainCinema(MgsPresentation presentation,const replay::FrameLatch &current,
                         const replay::MgsCinemaView &previous,bool pictureCompatible,
                         std::span<const replay::Operation> operations={}) {
  if(presentation==MgsPresentation::Cinema || presentation==MgsPresentation::Gameplay) return false;
  const auto &a=current.display.aperture;
  const auto covers=[&](const replay::NativeRect &r) {
    return r.x<=a.x && r.y<=a.y && int64_t(r.x)+r.width>=int64_t(a.x)+a.width &&
        int64_t(r.y)+r.height>=int64_t(a.y)+a.height;
  };
  for(const auto &op:operations) {
    if(std::holds_alternative<replay::Reset>(op) || std::holds_alternative<replay::SurfaceSeed>(op)) return false;
    if(const auto *fill=std::get_if<replay::FillRect>(&op);fill && covers(fill->destination)) return false;
    if(const auto *paint=std::get_if<replay::PaintRect>(&op);paint && covers(paint->destination)) return false;
    if(const auto *draw=std::get_if<replay::DrawPacket>(&op)) {
      auto clear=*draw;clear.state.mask.forceMaskBit=false;
      if(isMgsHudBackground(clear,uint32_t(a.x),a.width,a.height)) return false;
    }
  }
  if(current.mgsCinemaView)
    return current.mgsCinemaView->actor==previous.actor && current.mgsCinemaView->kind==previous.kind;
  return !current.mgsPresentationObserved && pictureCompatible;
}

// Exact displayed pixels, independent of unrelated texture uploads and the
// alternating draw-page address. RGB555's mask bit is not a visible colour.
struct CinemaPicture {
  uint32_t width=0,height=0;
  DisplayScanoutFormat format=DisplayScanoutFormat::RGB555;
  std::vector<uint8_t> pixels;
  bool operator==(const CinemaPicture &) const = default;
  bool sameOutside(const CinemaPicture &other,const replay::NativeRect &picture) const {
    if(width!=other.width || height!=other.height || format!=other.format || pixels.empty() ||
        pixels.size()!=other.pixels.size() || picture.x<0 || picture.y<0 ||
        uint64_t(picture.x)+picture.width>width || uint64_t(picture.y)+picture.height>height)
      return false;
    const unsigned bytes=format==DisplayScanoutFormat::RGB555?2:3;
    for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
      if(x>=uint32_t(picture.x) && x<uint32_t(picture.x)+picture.width &&
          y>=uint32_t(picture.y) && y<uint32_t(picture.y)+picture.height) continue;
      for(unsigned channel=0;channel<bytes;++channel)
        if(pixels[(y*width+x)*bytes+channel]!=other.pixels[(y*width+x)*bytes+channel]) return false;
    }
    return true;
  }
  static CinemaPicture capture(std::span<const Color16> vram,const DisplayScanoutPlan &plan) {
    CinemaPicture picture;
    if(!plan.enabled || vram.size()<1024*512 || plan.activeExtent.width>1024 || plan.activeExtent.height>512)
      return picture;
    picture.width=plan.activeExtent.width;picture.height=plan.activeExtent.height;picture.format=plan.format;
    const uint32_t bytes=plan.format==DisplayScanoutFormat::RGB555?2:3;
    picture.pixels.reserve(picture.width*picture.height*bytes);
    for(uint32_t y=0;y<picture.height;++y) for(uint32_t x=0;x<picture.width*bytes;++x) {
      const uint32_t row=(plan.sourceY+y*plan.rowStep)&511u,column=(plan.sourceByteX+x)&2047u;
      uint16_t value=vram[row*1024+column/2].raw;
      if(bytes==2)value&=0x7fff;
      picture.pixels.push_back(uint8_t(value>>((column&1u)*8)));
    }
    return picture;
  }
};

// The native draw page can contain the NEXT authored shot while the display
// still holds the previous one. Keep camera, source and UI together per page;
// a CPU-only state load has no replay history for an unseen displayed page.
struct CinemaPageHistory {
  struct Page {
    replay::CrashReplayFramePtr scene,panel;
    CinemaPicture picture;
    std::vector<Color16> nativeVram;
  };
  std::array<Page,2> pages;
  uint32_t actor=0;
  uint64_t epoch=0,lastLatch=0;
  void reset() { pages={};actor=0;epoch=lastLatch=0; }
  static std::optional<uint32_t> sourcePage(std::span<const replay::Operation> operations,
                                             const replay::MgsCinemaView &view) {
    std::optional<uint32_t> page;
    for(const auto &op:operations) if(const auto *p=std::get_if<replay::DrawPacket>(&op);
        p && isSourceWorldFace(*p) && p->originalMesh->temporalIdentity[0]==view.sceneEpoch) {
      const auto next=mgsHudPage(p->state.drawArea);
      if(!next || (page && page!=next)) return {};
      page=next;
    }
    return page;
  }
  static bool hasUi(std::span<const replay::Operation> operations,uint32_t page) {
    for(const auto &op:operations) {
      if(const auto *p=std::get_if<replay::DrawPacket>(&op);
          p && isMgsHudCandidate(*p) && mgsHudPage(p->state.drawArea)==page) return true;
      if(const auto *p=std::get_if<replay::PaintRect>(&op))
        if(const auto draw=mgsHudPaintRect(*p,320,224);draw && mgsHudPage(draw->state.drawArea)==page) return true;
    }
    return false;
  }
  static bool supported(const DisplayScanoutPlan &plan) {
    return plan.enabled && plan.format==DisplayScanoutFormat::RGB555 &&
        plan.sourceY==0 && plan.rowStep==1 && plan.activeExtent.width==320 &&
        plan.activeExtent.height==224 && (plan.sourceByteX==0 || plan.sourceByteX==640);
  }
  void observe(const replay::CrashReplayFramePtr &frame,MgsPresentation presentation,
               std::span<const Color16> vram,const DisplayScanoutPlan &display) {
    if(!frame || !supported(display) || frame->latch().frameNumber<lastLatch ||
        presentation==MgsPresentation::Cinema || presentation==MgsPresentation::Gameplay ||
        presentation==MgsPresentation::MovieCinema) {reset();return;}
    const auto &view=frame->latch().mgsCinemaView;
    if(frame->latch().mgsPresentationObserved && (!view || !view->actor ||
        view->kind!=replay::MgsCinemaKind::Scripted)) {reset();return;}
    if(view && (view->actor!=actor ||
        (presentation==MgsPresentation::StereoCinema && view->sceneEpoch!=epoch))) {
      reset();actor=view->actor;epoch=view->sceneEpoch;
    }
    lastLatch=frame->latch().frameNumber;
    const auto source=presentation==MgsPresentation::StereoCinema && view
        ? sourcePage(frame->operations(),*view) : std::nullopt;
    if(presentation==MgsPresentation::StereoCinema && !source) {reset();return;}
    for(unsigned i=0;i<pages.size();++i) {
      auto &page=pages[i];const uint32_t x=i*320;
      if(!page.scene && source!=x) continue;
      auto plan=display;plan.sourceByteX=x*2;
      auto picture=CinemaPicture::capture(vram,plan);
      if(picture.pixels.empty()) {page={};continue;}
      if(source==x) {
        page={frame,frame,std::move(picture),std::vector<Color16>(vram.begin(),vram.end())};continue;
      }
      auto latch=frame->latch();latch.display.aperture={int32_t(x),0,320,224};
      if(!retainCinema(MgsPresentation::Reuse,latch,*page.scene->latch().mgsCinemaView,
                       picture==page.picture,frame->operations())) {page={};continue;}
      if(view && hasUi(frame->operations(),x)) {
        page.panel=frame;page.picture=std::move(picture);
        page.nativeVram.assign(vram.begin(),vram.end());
      } else if(picture!=page.picture) page={};
    }
  }
  const Page *displayed(std::span<const Color16> vram,const DisplayScanoutPlan &plan) const {
    if(!supported(plan))return nullptr;
    const auto &page=pages[plan.sourceByteX/640];
    return page.scene && page.picture==CinemaPicture::capture(vram,plan)?&page:nullptr;
  }
};

// The native water pass reads the scene after its owned OT255 clear, then
// emits opaque framebuffer strips before credits/letterbox/captions. Keep that
// exact producer/order boundary, never infer it from a black rectangle alone.
struct CinemaWaterPass {
  uint32_t actor=0,phase=0,strips=0;
  size_t sceneBegin=0,firstStrip=0;
  bool valid=false;
};
inline CinemaWaterPass cinemaWaterPass(std::span<const replay::Operation> operations) {
  CinemaWaterPass pass;size_t clear=operations.size();
  for(size_t i=0;i<operations.size();++i) if(const auto *p=std::get_if<replay::DrawPacket>(&operations[i]);p && p->waterEffect[0]) {
    if(!pass.actor) {pass.actor=p->waterEffect[0];pass.phase=p->waterEffect[1];pass.firstStrip=i;}
    if(p->waterEffect[0]!=pass.actor || p->waterEffect[1]!=pass.phase || p->opcode!=0x64 ||
        !p->textured || p->state.blend.enabled || p->state.texturePage.depth!=replay::TextureDepth::Direct15)
      return pass;
    ++pass.strips;
  }
  if(!pass.actor) return pass;
  for(size_t i=0;i<pass.firstStrip;++i) if(const auto *p=std::get_if<replay::DrawPacket>(&operations[i]);
      p && p->mgsWaterClearActor==pass.actor) clear=i;
  if(clear==operations.size())return pass;
  bool source=false;
  for(size_t i=clear+1;i<operations.size();++i) if(const auto *p=std::get_if<replay::DrawPacket>(&operations[i]);p && isSourceWorldFace(*p)) {
    if(i>=pass.firstStrip)return pass;
    source=true;
  }
  pass.sceneBegin=clear+1;pass.valid=source;
  return pass;
}
inline Vec3 cinemaWaterSourcePosition(const replay::DrawPacket &packet,unsigned lane,uint32_t page) {
  const auto &v=packet.vertices[lane];
  // Native tpage X is in 64-word units; both 160px halves become one source
  // coordinate in the physical picture, independent of its framebuffer page.
  const float x=float((packet.state.texturePage.raw&15u)*64u)+float(v.u)-page;
  const float y=float((packet.state.texturePage.raw&16u)*16u)+float(v.v);
  return {(x/320-.5f)*1.9f,(.5f-y/224)*1.06875f,-2};
}

// A fixed picture window and adjacent native-caption band. Depth is compressed
// behind the window so foreground geometry can never obscure its captions.
struct CinemaView {
  static constexpr float width=1.9f,height=1.06875f,distance=2.0f,captionHeight=0.26f;
  static constexpr float depthRange=1.0f,depthScale=1500.0f;
  replay::MgsCinemaView source;
  Vec3 camera(Vec3 world) const {
    Vec3 result{};
    for(unsigned r=0;r<3;++r)
      result[r]=source.sourceView[12+r]+source.sourceView[r]*world[0]+
          source.sourceView[4+r]*world[1]+source.sourceView[8+r]*world[2];
    result[0]*=source.widescreen?0.75f:1.0f;
    result[1]*=58.0f/64.0f;
    return result;
  }
  float depth(float z) const { return distance+depthRange*z/(z+depthScale); }
  Vec3 picture(Vec3 camera) const {
    const float d=depth(camera[2]);
    return {camera[0]*source.projection/320*width*d/(distance*camera[2]),
           -camera[1]*source.projection/224*height*d/(distance*camera[2]),-d};
  }
  // Clip before dividing by native Z. The physical window masks the sides;
  // off-screen source faces remain available to both eye views at its edges.
  template<class Vertex> static std::vector<Vertex> clipNear(const std::vector<Vertex> &polygon) {
    std::vector<Vertex> result;
    if(polygon.empty()) return result;
    auto previous=polygon.back();float pd=previous.position[2]-1;
    for(const auto &current:polygon) {
      const float cd=current.position[2]-1;
      if((pd>=0)!=(cd>=0)) {
        const float t=pd/(pd-cd);auto v=previous;
        for(unsigned a=0;a<3;++a) {
          v.position[a]+=(current.position[a]-previous.position[a])*t;
          v.shade[a]+=(current.shade[a]-previous.shade[a])*t;
        }
        for(unsigned a=0;a<2;++a)v.uv[a]+=(current.uv[a]-previous.uv[a])*t;
        v.position[2]=1;
        result.push_back(v);
      }
      if(cd>=0)result.push_back(current);
      previous=current;pd=cd;
    }
    return result;
  }
  // Correct native UV/shade interpolation after the bounded depth transform.
  float perspective(float z) const { return depth(z)/z; }
  static Vec3 caption(double x,double y,uint32_t page,float pictureHeight=height) {
    return {float(x-page-160)*width/320,
            -pictureHeight/2-captionHeight/2-float(y-204)*width/320,-distance};
  }
};
}
