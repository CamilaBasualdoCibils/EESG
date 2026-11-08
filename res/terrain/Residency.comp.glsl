#version 450
layout(local_size_x=64) in;
const uint INVALID=0xffffffffu;
struct Node { ivec4 region; uvec4 state; };
layout(std430,set=0,binding=0) readonly buffer InQueue { uint inCount; uint incoming[]; };
layout(std430,set=0,binding=1) buffer OutQueue { uint outCount; uint outgoing[]; };
layout(std430,set=0,binding=2) buffer Tree { uvec4 counts; uvec4 levels[META_LEVELS+1]; Node nodes[]; };
layout(std430,set=0,binding=3) readonly buffer Input { uint words[]; };
layout(std140,set=0,binding=4) uniform Stage { uvec4 stage; };
float f(uint index) { return uintBitsToFloat(words[index]); }
vec4 v(uint index) { return vec4(f(index),f(index+1),f(index+2),f(index+3)); }
void initialize(uint index,ivec3 origin,int level,uint parent) {
  nodes[index].region=ivec4(origin,level);
  nodes[index].state=uvec4(INVALID,parent,0,0);
}
uint allocateChildren() {
  uint before=atomicAdd(counts.x,0u);
  for(;;) {
    if(before>NODE_CAPACITY-8u) { atomicOr(counts.w,1u); return INVALID; }
    uint actual=atomicCompSwap(counts.x,before,before+8u);
    if(actual==before) return before;
    before=actual;
  }
}
void main() {
  uint id=gl_GlobalInvocationID.x;
  if(stage.x==0u) {
    if(id!=0u) return;
    uint roots=words[0]; counts=uvec4(roots,roots,0,0);
    for(uint i=0u;i<=META_LEVELS;++i) levels[i]=uvec4(0);
    outCount=roots;
    for(uint i=0u;i<roots;++i) {
      uint offset=4u+i*4u;
      initialize(i,ivec3(words[offset],words[offset+1],words[offset+2]),META_LEVELS,INVALID);
      outgoing[i]=i;
    }
    return;
  }
  if(id>=inCount) return;
  uint index=incoming[id];
  Node node=nodes[index]; uint level=uint(node.region.w);
  atomicAdd(levels[level].x,1u);
  float width=32.0*float(1u<<level);
  vec3 minimum=vec3(node.region.xyz)*32.0;
  vec3 maximum=minimum+vec3(width);
  bool wanted=false, subdivide=false;
  uint lod=INVALID;
  // Each node uses the regular chunk's internal octree depth.
  // Meta level contributes to the effective world-space cell LOD.
  float finestCell=width/float(1u<<CHUNK_DEPTH);
  for(uint observer=0u;observer<words[1];++observer) {
    uint base=words[2]+observer*36u;
    vec4 position=v(base);
    vec4 projection=v(base+4u);
    float distance;
    if(projection.y==1.0) {
      bool outside=false;
      for(uint p=0u;p<6u;++p) {
        vec4 plane=v(base+12u+4u*p);
        vec3 positive=mix(minimum,maximum,greaterThanEqual(plane.xyz,vec3(0)));
        if(dot(plane.xyz,positive)+plane.w<0.0) { outside=true; break; }
      }
      if(outside) continue;
      // Use nearest clip-W over the entire AABB, rather than radial distance;
      // this preserves detail near screen edges and across the camera plane.
      vec4 clipW=v(base+8u);
      vec3 nearest=mix(maximum,minimum,greaterThanEqual(clipW.xyz,vec3(0)));
      distance=max(dot(clipW.xyz,nearest)+clipW.w,0.0);
    } else {
      vec3 delta=position.xyz-clamp(position.xyz,minimum,maximum);
      distance=length(delta);
    }
    wanted=true;
    float allowedCell=position.w*distance/projection.x;
    if(level>0u && finestCell>allowedCell) { subdivide=true; continue; }
    uint candidate=level;
    float cell=finestCell;
    for(uint internalLOD=0u;internalLOD<CHUNK_DEPTH;++internalLOD) {
      if(cell*2.0>allowedCell) break;
      cell*=2.0; ++candidate;
    }
    lod=min(lod,candidate); // All observers must be satisfied.
  }
  if(!wanted) { atomicAdd(levels[level].y,1u); return; }
  if(!subdivide) {
    nodes[index].state.z=lod; nodes[index].state.w=1u;
    atomicAdd(counts.z,1u); atomicAdd(levels[level].w,1u); return;
  }
  uint first=allocateChildren();
  if(first==INVALID) return;
  uint offset=atomicAdd(outCount,8u);
  int stride=1<<(level-1u);
  for(uint child=0u;child<8u;++child) {
    ivec3 bits=ivec3(child&1u,(child>>1u)&1u,(child>>2u)&1u);
    initialize(first+child,node.region.xyz+bits*stride,int(level)-1,index);
    outgoing[offset+child]=first+child;
  }
  nodes[index].state.x=first; nodes[index].state.w=2u;
  atomicAdd(levels[level].z,1u);
}
