#version 440

layout(location = 0) in vec2 atlasCoord;
layout(location = 1) in vec4 ink;
layout(location = 2) in vec2 glyphKind;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 modelViewMatrix;
    mat4 projectionMatrix;
    float qt_Opacity;
    float dpr;
    float maskSize;
};

layout(binding = 1) uniform sampler2D mask;
layout(binding = 2) uniform sampler2D colors;

// Qt Quick's distance-field thresholds for a field drawn `scale` device
// pixels per texel (TerminalFieldText::alphaRange; the same in
// terminal_cell_subpixel.frag).
vec2 alphaRange(float scale)
{
    float threshold = 0.5 - 0.065 * (1.0 - (clamp(scale, 0.15, 0.3) - 0.15) / 0.15);
    float spread = 0.06 / scale;
    return vec2(max(0.0, threshold - spread), min(threshold + spread, 1.0));
}

// Premultiplied throughout: a colour texel (emoji) is scaled by the ink; a
// mask texel is the ink's coverage, once a text glyph's field has become
// greyscale coverage as in Qt Quick's distancefieldtext.frag.
void main()
{
    float texelsPerPixel = dFdx(atlasCoord.x) * maskSize;
    if (glyphKind.y > 0.5) {
        fragColor = textureLod(colors, atlasCoord, 0.0) * ink;
        return;
    }
    float texel = textureLod(mask, atlasCoord, 0.0).r;
    if (glyphKind.x > 0.5) {
        vec2 range = alphaRange(1.0 / texelsPerPixel);
        texel = smoothstep(range.x, range.y, texel);
    }
    fragColor = ink * texel;
}
