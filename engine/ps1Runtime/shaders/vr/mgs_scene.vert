#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 shade;
layout(location = 3) in uint material;
layout(location = 4) in float perspective;
layout(location = 5) in vec3 feedbackPosition;

layout(push_constant) uniform Eye {
    mat4 clipFromAnchor;
} eye;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec3 fragShade;
layout(location = 2) flat out uint fragMaterial;
layout(location = 3) out float fragPerspective;
layout(location = 4) out vec4 fragFeedbackClip;

void main() {
    gl_Position = eye.clipFromAnchor * vec4(position, 1.0);
    fragUv = uv * perspective;
    fragShade = shade * perspective;
    fragPerspective = perspective;
    fragMaterial = material;
    fragFeedbackClip = eye.clipFromAnchor * vec4(feedbackPosition, 1.0);
}
