#pragma once
#include "scene_view.h"
#include <algorithm>

namespace ps1::gpu::vr {
namespace arm {
inline Vec3 add(Vec3 a, Vec3 b) { for (unsigned i=0;i<3;++i) a[i]+=b[i]; return a; }
inline Vec3 sub(Vec3 a, Vec3 b) { for (unsigned i=0;i<3;++i) a[i]-=b[i]; return a; }
inline Vec3 scale(Vec3 a, float s) { for (float &v:a) v*=s; return a; }
inline float dot(Vec3 a, Vec3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
inline float length(Vec3 a) { return std::sqrt(dot(a,a)); }
inline Vec3 unit(Vec3 a) { const float n=length(a); return n>1e-6f?scale(a,1/n):Vec3{}; }
inline Vec3 position(const Matrix &m) { return {m[12],m[13],m[14]}; }
inline Vec3 vector(const Matrix &m,Vec3 v) {
  return {m[0]*v[0]+m[4]*v[1]+m[8]*v[2],m[1]*v[0]+m[5]*v[1]+m[9]*v[2],m[2]*v[0]+m[6]*v[1]+m[10]*v[2]};
}
inline Vec3 point(const Matrix &m,Vec3 p) { return add(vector(m,p),position(m)); }
inline Matrix identity() { return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
inline bool finite(Vec3 v) { return std::all_of(v.begin(),v.end(),[](float x){return std::isfinite(x);}); }
inline Matrix frame(Vec3 x,Vec3 y,Vec3 p) {
  x=unit(x); y=unit(sub(y,scale(x,dot(x,y)))); const Vec3 z=cross(x,y);
  return {x[0],x[1],x[2],0,y[0],y[1],y[2],0,z[0],z[1],z[2],0,p[0],p[1],p[2],1};
}
inline std::optional<Matrix> delta(const Matrix &from,const Matrix &to) {
  // Invert the linear basis directly. Subtracting inverse-transformed world
  // origins to obtain its columns loses precision at stage-sized offsets.
  const std::array<double,3> a{from[0],from[1],from[2]},b{from[4],from[5],from[6]},c{from[8],from[9],from[10]};
  const auto crossD=[](const auto &u,const auto &v) {
    return std::array<double,3>{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
  };
  const std::array rows{crossD(b,c),crossD(c,a),crossD(a,b)};
  const double determinant=a[0]*rows[0][0]+a[1]*rows[0][1]+a[2]*rows[0][2];
  if(!std::isfinite(determinant)||std::abs(determinant)<1e-6)return {};
  Matrix out=identity();
  for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row) {
    double value=0;
    for(unsigned k=0;k<3;++k)value+=double(to[k*4+row])*rows[k][col]/determinant;
    out[col*4+row]=float(value);
  }
  for(unsigned row=0;row<3;++row) {
    double value=to[12+row];
    for(unsigned col=0;col<3;++col)value-=double(out[col*4+row])*from[12+col];
    out[12+row]=float(value);
  }
  return out;
}
// Spherical interpolation from identity keeps a rigid rotation at every
// tracking weight and has a bounded angular speed all the way to native.
inline Quaternion blendRotation(Quaternion q,float weight) {
  float norm=0;for(float v:q)norm+=v*v;
  if(!std::isfinite(norm)||norm<1e-12f)return {0,0,0,1};
  for(float &v:q)v/=std::sqrt(norm);
  if(q[3]<0)for(float &v:q)v=-v;
  const float sine=length({q[0],q[1],q[2]});
  if(sine<1e-7f)return {0,0,0,1};
  const float angle=std::atan2(sine,q[3]);
  const float factor=std::sin(angle*weight)/sine;
  return {q[0]*factor,q[1]*factor,q[2]*factor,std::cos(angle*weight)};
}
inline Matrix rotated(const Matrix &native,Quaternion q,Vec3 origin) {
  Matrix out=native;
  for(unsigned c=0;c<3;++c) {
    const auto v=rotate(q,{native[c*4],native[c*4+1],native[c*4+2]});
    for(unsigned r=0;r<3;++r)out[c*4+r]=v[r];
  }
  for(unsigned r=0;r<3;++r)out[12+r]=origin[r];
  return out;
}
// Rotate an animated segment into its solved direction without changing its
// length or scale. The transported elbow pole resolves the antiparallel case.
inline Matrix redirect(const Matrix &native,Vec3 oldDirection,Vec3 direction,Vec3 pole,Vec3 origin,float weight=1) {
  const Vec3 a=unit(oldDirection),b=unit(direction);
  const float d=std::clamp(dot(a,b),-1.0f,1.0f);
  Quaternion q{};
  if (d < -0.9999f) {
    Vec3 axis=unit(cross(a,pole));
    if (length(axis)<0.5f) axis=unit(cross(a,std::abs(a[0])<0.8f?Vec3{1,0,0}:Vec3{0,1,0}));
    q={axis[0],axis[1],axis[2],0};
  } else {
    const Vec3 axis=cross(a,b); const float n=std::sqrt(2*(1+d));
    q={axis[0]/n,axis[1]/n,axis[2]/n,n*0.5f};
  }
  return rotated(native,blendRotation(q,weight),origin);
}
struct Target { Matrix wrist=identity(); bool tracked=false; };
struct State { Vec3 pole{}; bool hasPole=false; float weight=0; Matrix lastTarget=identity(); };
struct Solution {
  std::array<Matrix,3> world{}, changes{};
  Vec3 elbow{}, wrist{};
  bool active=false;
};
inline Solution solve(const replay::MgsArmPose &native,const Target &target,
                      Vec3 preferredPole,float dt,State &state) {
  Solution result;
  const Vec3 shoulder=position(native.world[0]),elbow=position(native.world[1]),wrist=position(native.world[2]);
  const float upper=length(sub(elbow,shoulder)),lower=length(sub(wrist,elbow));
  if (!native.owners[0] || !native.owners[1] || !native.owners[2] ||
      !finite(shoulder)||!finite(elbow)||!finite(wrist)||
      upper<30 || lower<30 || upper>700 || lower>700) { state={}; return result; }
  bool tracked=target.tracked;
  for(float value:target.wrist) tracked &= std::isfinite(value);
  tracked &= length(sub(position(target.wrist),shoulder))<2000;
  if(tracked) state.lastTarget=target.wrist;
  const float step=std::clamp(dt,0.0f,0.05f)/0.12f;
  state.weight=std::clamp(state.weight+(tracked?step:-step),0.0f,1.0f);
  if(state.weight==0) { state.hasPole=false; return result; }
  // Solve the full tracked pose first. Blending the target alone leaves the
  // transported pole unrelated to the native elbow at the end of a fade.
  const Vec3 requested=position(state.lastTarget);
  Vec3 ray=sub(requested,shoulder);
  if(length(ray)<1) ray=sub(wrist,shoulder);
  if(length(ray)<1) { state={};return result; }
  const Vec3 axis=unit(ray);
  const float reach=std::clamp(length(ray),std::abs(upper-lower)+5,(upper+lower)*0.995f);
  Vec3 nativePole=sub(elbow,shoulder);
  const Vec3 nativeAxis=unit(sub(wrist,shoulder));
  nativePole=sub(nativePole,scale(nativeAxis,dot(nativePole,nativeAxis)));
  Vec3 pole=state.hasPole?state.pole:(length(nativePole)>1?unit(nativePole):preferredPole);
  pole=sub(pole,scale(axis,dot(pole,axis)));
  if(length(pole)<0.05f) {
    pole=sub(preferredPole,scale(axis,dot(preferredPole,axis)));
    if(length(pole)<0.05f) pole=cross(axis,std::abs(axis[0])<0.8f?Vec3{1,0,0}:Vec3{0,0,1});
  }
  pole=unit(pole); state.pole=pole;state.hasPole=true;
  const float along=(upper*upper-lower*lower+reach*reach)/(2*reach);
  const float bend=std::sqrt(std::max(0.0f,upper*upper-along*along));
  const Vec3 trackedElbow=add(shoulder,add(scale(axis,along),scale(pole,bend)));
  const Vec3 trackedWrist=add(shoulder,scale(axis,reach));
  // Blend rigid segment rotations, then rebuild their endpoints. Both lengths
  // remain native, while positions AND orientations converge to native. The
  // tracked reach cap cannot keep a straight native arm bent at weight zero.
  result.world[0]=redirect(native.world[0],sub(elbow,shoulder),sub(trackedElbow,shoulder),pole,shoulder,state.weight);
  const auto upperChange=delta(native.world[0],result.world[0]);
  if(!upperChange){state={};return {};}
  result.elbow=point(*upperChange,elbow);
  result.world[1]=redirect(native.world[1],sub(wrist,elbow),sub(trackedWrist,trackedElbow),pole,result.elbow,state.weight);
  const auto lowerChange=delta(native.world[1],result.world[1]);
  if(!lowerChange){state={};return {};}
  result.wrist=point(*lowerChange,wrist);
  // Blend orientations with quaternions so 180-degree wrist rolls never
  // collapse an interpolated basis. Native bone scale remains intact.
  const auto rotationQuaternion=[](Matrix m) {
    for(unsigned c=0;c<3;++c) {
      const auto v=unit({m[c*4],m[c*4+1],m[c*4+2]});
      for(unsigned r=0;r<3;++r)m[c*4+r]=v[r];
    }
    Quaternion q{}; const float trace=m[0]+m[5]+m[10];
    if(trace>0) {float s=std::sqrt(trace+1)*2;q={(m[6]-m[9])/s,(m[8]-m[2])/s,(m[1]-m[4])/s,s/4};}
    else {unsigned i=m[5]>m[0]?1:0;if(m[10]>m[i*4+i])i=2;unsigned j=(i+1)%3,k=(i+2)%3;
      float s=std::sqrt(std::max(0.0f,1+m[i*4+i]-m[j*4+j]-m[k*4+k]))*2;
      if(s<1e-5f)return Quaternion{0,0,0,1};q[i]=s/4;q[j]=(m[j*4+i]+m[i*4+j])/s;q[k]=(m[k*4+i]+m[i*4+k])/s;q[3]=(m[j*4+k]-m[k*4+j])/s;}
    return q;
  };
  Quaternion a=rotationQuaternion(native.world[2]),b=rotationQuaternion(state.lastTarget);
  float an=0,bn=0;
  for(unsigned i=0;i<4;++i){an+=a[i]*a[i];bn+=b[i]*b[i];}
  for(unsigned i=0;i<4;++i){a[i]/=std::sqrt(an);b[i]/=std::sqrt(bn);}
  // Rotate the actual native basis rather than rebuilding it from a
  // quaternion: even the retained root's tiny scale/shear stays continuous.
  const Vec3 av{-a[0],-a[1],-a[2]},bv{b[0],b[1],b[2]};
  const Vec3 relative=add(add(scale(av,b[3]),scale(bv,a[3])),cross(bv,av));
  const Quaternion q{relative[0],relative[1],relative[2],b[3]*a[3]-dot(bv,av)};
  result.world[2]=rotated(native.world[2],blendRotation(q,state.weight),result.wrist);
  for(unsigned j=0;j<3;++j) {
    const auto change=delta(native.world[j],result.world[j]);
    if(!change){state={};return {};}
    result.changes[j]=*change;
  }
  result.active=true;return result;
}
// Inverse of the production body camera, using the same recenter and turn
// basis as eyeMatrix. Grip -Z becomes native weapon -Y; grip +X keeps roll.
inline Target controllerTarget(Vec3 position,Quaternion orientation,const Anchor &anchor,
                               float baseHeading,float stickTurn,Vec3 eye,bool tracked) {
  Target out;out.tracked=tracked;
  float norm=0;
  for(float v:orientation)norm+=v*v;
  if(!finite(position)||!std::isfinite(norm)||norm<0.5f||norm>1.5f) {out.tracked=false;return out;}
  for(float &v:orientation)v/=std::sqrt(norm);
  const Quaternion room=inverse(yaw(2*std::atan2(anchor.heading[1],anchor.heading[3])+stickTurn));
  const auto toWorld=[&](Vec3 v) {
    v=rotate(room,v);const float c=std::cos(baseHeading),s=std::sin(baseHeading);
    return Vec3{-c*v[0]-s*v[2],v[1],s*v[0]-c*v[2]};
  };
  const Vec3 p=add(eye,scale(toWorld(sub(position,anchor.origin)),1000));
  out.wrist=frame(toWorld(rotate(orientation,{1,0,0})),toWorld(rotate(orientation,{0,0,1})),p);
  return out;
}
} // namespace arm
} // namespace ps1::gpu::vr
