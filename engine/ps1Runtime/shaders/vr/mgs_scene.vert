#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 shade;
layout(location = 3) in uint material;

layout(push_constant) uniform Eye {
    mat4 clipFromAnchor;
} eye;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec3 fragShade;
layout(location = 2) flat out uint fragMaterial;

void main() {
    gl_Position = eye.clipFromAnchor * vec4(position, 1.0);
    fragUv = uv;
    fragShade = shade;
    fragMaterial = material;
}
