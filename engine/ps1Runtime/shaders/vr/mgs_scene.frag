#version 450
layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec3 fragShade;
layout(location = 2) flat in uint fragMaterial;
layout(location = 3) in float fragPerspective;
layout(location = 4) in vec4 fragFeedbackClip;
layout(location = 0) out vec4 outColor;

struct Material {
    uvec4 data;    // cached index and palette offsets, width and height
    ivec4 origin;  // cached UV origin, palette mode and scroll
    uvec4 native;  // native tpage, CLUT, sampling mode, opacity and sidedness
    uint palette[8];
};

layout(set = 0, binding = 0, std430) readonly buffer AssetWords { uint asset[]; };
layout(set = 0, binding = 1, std430) readonly buffer VramWords { uint vram[]; };
layout(set = 0, binding = 2, std430) readonly buffer Materials { Material materials[]; };
layout(set = 0, binding = 3) uniform sampler2D cinemaEye;

uint nativeWord(uint x, uint y) {
    uint address = ((y & 511u) * 1024u + (x & 1023u));
    return (vram[address >> 1u] >> ((address & 1u) * 16u)) & 65535u;
}

uint nativeByte(uint x, uint y) {
    return (nativeWord(x >> 1u, y) >> ((x & 1u) * 8u)) & 255u;
}

uint clut(uint id, uint index) {
    return nativeWord((id & 63u) * 16u + index, id >> 6u);
}

uint sampleTexture(Material m, uvec2 uv) {
    if (m.origin.z > 0) {
        ivec2 local = ivec2(uv) - m.origin.xy;
        if (all(greaterThanEqual(local, ivec2(0))) &&
            all(lessThan(local, ivec2(m.data.zw)))) {
            uint scroll = uint(m.origin.w);
            uint period = scroll >> 16u;
            if (period > 0u && uint(local.y) < period)
                local.y = int((uint(local.y) + (scroll & 65535u)) % period);
            uint byteOffset = m.data.x + uint(local.y) * m.data.z + uint(local.x);
            uint index = (asset[byteOffset >> 2u] >> ((byteOffset & 3u) * 8u)) & 255u;
            if (m.origin.z == 2)
                return index < 16u
                    ? (m.palette[index >> 1u] >> ((index & 1u) * 16u)) & 65535u
                    : clut(m.native.y, index);
            if (m.origin.z == 3)
                return clut(m.native.y, index);
            return asset[(m.data.y >> 2u) + index] & 65535u;
        }
    }
    uint page = m.native.x;
    uint x = (page & 15u) * 64u;
    uint y = ((page >> 4u) & 1u) * 256u + uv.y;
    uint depth = (page >> 7u) & 3u;
    if (depth == 0u)
        return clut(m.native.y, (nativeWord(x + uv.x / 4u, y) >> ((uv.x & 3u) * 4u)) & 15u);
    if (depth == 1u)
        return clut(m.native.y, (nativeWord(x + uv.x / 2u, y) >> ((uv.x & 1u) * 8u)) & 255u);
    return nativeWord(x + uv.x, y);
}

vec3 rgb555(uint texel) {
    uvec3 c = uvec3(texel, texel >> 5u, texel >> 10u) & uvec3(31);
    return vec3((c << 3u) | (c >> 2u)) / 255.0;
}

// Native PS1 artwork and shades are display-encoded. The sRGB eye image
// encodes linear shader output once and blends translucent draws in linear RGB.
vec3 linearRgb(vec3 rgb) {
    return mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
               greaterThan(rgb, vec3(0.04045)));
}

// Sampling the sRGB eye image already decodes to linear. Native modulation
// happens in display RGB, exactly once, before the destination sRGB encode.
vec3 displayRgb(vec3 linear) {
    return mix(linear * 12.92, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055,
               greaterThan(linear, vec3(0.0031308)));
}

void main() {
    Material m = materials[fragMaterial];
    if ((m.native.w & 256u) != 0u && !gl_FrontFacing) discard;
    uvec2 uv = uvec2(ivec2(floor(fragUv / fragPerspective)));
    vec3 rgb = fragShade / fragPerspective;
    if (m.native.z == 5u) {
        outColor = vec4(texture(cinemaEye, fragUv).rgb, 1.0);
        return;
    } else if (m.native.z == 4u) {
        vec2 sourceUv = fragFeedbackClip.xy / fragFeedbackClip.w * 0.5 + 0.5;
        rgb *= displayRgb(texture(cinemaEye, sourceUv).rgb);
    } else if (m.native.z == 1u) {
        uint texel = sampleTexture(m, uv & uvec2(255));
        if (texel == 0u) discard;
        rgb *= rgb555(texel);
    } else if (m.native.z == 2u) {
        rgb = rgb555(nativeWord(uv.x, m.data.y + uv.y * m.data.z));
    } else if (m.native.z == 3u) {
        uint x = m.data.x + uv.x * 3u;
        uint y = m.data.y + uv.y * m.data.z;
        rgb = vec3(nativeByte(x, y), nativeByte(x + 1u, y),
                   nativeByte(x + 2u, y)) / 255.0;
    }
    outColor = vec4(linearRgb(clamp(rgb, 0.0, 1.0)), float(m.native.w & 255u) / 255.0);
}
