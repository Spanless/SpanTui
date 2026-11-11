#version 440

layout(location = 0) in vec4 position;
layout(location = 1) in vec2 texel;
layout(location = 2) in vec4 color;
// x: 1 for distance-field glyphs, y: 1 for colour glyphs; the rest pads the
// vertex.
layout(location = 3) in vec4 flags;
layout(location = 0) out vec2 atlasCoord;
layout(location = 1) out vec4 ink;
layout(location = 2) out vec2 glyphKind;

layout(std140, binding = 0) uniform buf {
    mat4 modelViewMatrix;
    mat4 projectionMatrix;
    float qt_Opacity;
    float dpr;
    float maskSize;
};

out gl_PerVertex { vec4 gl_Position; };

// Rows place every quad on whole device pixels of their own origin. The row
// itself may sit between device pixels (an item at an odd position under
// fractional scaling), so its origin moves to the nearest one; every vertex
// moves alike, and bitmap texels keep mapping one to one onto pixels.
void main()
{
    atlasCoord = texel;
    ink = color * qt_Opacity;
    glyphKind = flags.xy;
    vec4 origin = modelViewMatrix * vec4(0.0, 0.0, 0.0, 1.0);
    vec2 shift = floor(origin.xy * dpr + 0.5) / dpr - origin.xy;
    vec4 xformed = modelViewMatrix * position;
    gl_Position = projectionMatrix * vec4(xformed.xy + shift * xformed.w, xformed.zw);
}
