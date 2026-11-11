#version 440

layout(location = 0) in vec2 atlasCoord;
layout(location = 1) in vec4 ink;
layout(location = 2) in vec2 glyphKind;
// Dual-source blending: the colour, and the weight that keeps each channel
// of what is behind (one minus it).
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 fragWeights;

layout(std140, binding = 0) uniform buf {
    mat4 modelViewMatrix;
    mat4 projectionMatrix;
    float qt_Opacity;
    float dpr;
    float maskSize;
};

layout(binding = 1) uniform sampler2D mask;
layout(binding = 2) uniform sampler2D colors;

// The same as in terminal_cell.frag.
vec2 alphaRange(float scale)
{
    float threshold = 0.5 - 0.065 * (1.0 - (clamp(scale, 0.15, 0.3) - 0.15) / 0.15);
    float spread = 0.06 / scale;
    return vec2(max(0.0, threshold - spread), min(threshold + spread, 1.0));
}

// A text glyph's field is sampled at the pixel's centre and a third and two
// thirds of a pixel to either side, and its red, green and blue stripes each
// get their own coverage, as in Qt Quick's hiqsubpixeldistancefieldtext.frag.
// Qt blends with the ink as a constant colour; here each channel keeps what
// is behind by its own weight, so a row's glyphs of any colour draw at once.
// Everything else blends as premultiplied colour.
void main()
{
    vec2 step = dFdx(atlasCoord);
    if (glyphKind.y > 0.5) {
        fragColor = textureLod(colors, atlasCoord, 0.0) * ink;
        fragWeights = vec4(fragColor.a);
        return;
    }
    float c = textureLod(mask, atlasCoord, 0.0).r;
    if (glyphKind.x > 0.5) {
        vec2 range = alphaRange(1.0 / (step.x * maskSize));
        vec4 n = vec4(textureLod(mask, atlasCoord - 0.667 * step, 0.0).r,
                      textureLod(mask, atlasCoord - 0.333 * step, 0.0).r,
                      textureLod(mask, atlasCoord + 0.333 * step, 0.0).r,
                      textureLod(mask, atlasCoord + 0.667 * step, 0.0).r);
        vec2 d = min(abs(n.yw - n.xz) * 2.0, 0.67);
        vec2 lo = mix(vec2(range.x), vec2(0.5), d);
        vec2 hi = mix(vec2(range.y), vec2(0.5), d);
        n = smoothstep(lo.xxyy, hi.xxyy, n);
        c = smoothstep(lo.x + lo.y, hi.x + hi.y, 2.0 * c);
        vec3 stripes = 0.333 * (n.xyz + n.yzw + c);
        fragColor = vec4(ink.rgb * stripes, ink.a * c);
        fragWeights = vec4(stripes * ink.a, fragColor.a);
    } else {
        fragColor = ink * c;
        fragWeights = vec4(fragColor.a);
    }
}
