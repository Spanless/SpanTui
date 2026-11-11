#version 440

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 quadSizePx;
    vec2 shapeSizePx;
    vec4 cornerRadiiPx;
    float shapeInsetPx;
    float strokeWidthPx;
    float fillEnabledValue;
    float strokeEnabledValue;
    float strokeStrengthValue;
    float shadowEnabledValue;
    float shadowBlurPx;
    float shadowOffsetYPx;
    float shadowStrengthValue;
    vec4 fillTopColor;
    vec4 fillMiddleColor;
    vec4 fillBottomColor;
    vec4 strokeColor;
    vec4 shadowColor;
};

float selectedRadius(vec2 p, vec4 radii)
{
    if (p.y < 0.0)
        return p.x < 0.0 ? radii.x : radii.y;
    return p.x < 0.0 ? radii.w : radii.z;
}

float roundedRectDistance(vec2 p, vec2 halfSize, vec4 radii, float inset,
                          out float cornerMask)
{
    vec2 bounds = max(vec2(0.0), halfSize - vec2(inset));
    vec4 insetRadii = max(vec4(0.0), radii - vec4(inset));
    float limit = max(0.0, min(bounds.x, bounds.y));
    float radius = min(selectedRadius(p, insetRadii), limit);
    vec2 q = abs(p) - bounds + vec2(radius);
    cornerMask = smoothstep(0.0, 1.0, min(q.x, q.y));
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - radius;
}

float antialiasWidth(float distanceToEdge)
{
    // Distance is expressed in physical pixels. fwidth therefore produces one
    // monotonic transition band on every RHI backend without window-wide MSAA.
    return max(0.35, 0.5 * fwidth(distanceToEdge));
}

float coverage(float distanceToEdge, float aa)
{
    return 1.0 - smoothstep(-aa, aa, distanceToEdge);
}

vec4 premultiplied(vec4 colorValue, float coverageValue)
{
    // ShaderEffect premultiplies every QColor uniform before it reaches this
    // block. Coverage scales that premultiplied value once; multiplying RGB by
    // alpha again turns translucent white into a dirty gray fringe.
    return colorValue * coverageValue;
}

vec4 over(vec4 top, vec4 bottom)
{
    return top + bottom * (1.0 - top.a);
}

// Abramowitz–Stegun style erf approximation (max error ~5e-4), enough for
// an 8-bit shadow ramp.
float erfApprox(float x)
{
    float s = sign(x);
    float a = abs(x);
    float t = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    t *= t;
    return s - s / (t * t);
}

vec4 materialColor(vec2 shapePosition)
{
    float y = clamp(shapePosition.y / max(1.0, shapeSizePx.y), 0.0, 1.0);
    const float middle = 0.54;
    if (y <= middle)
        return mix(fillTopColor, fillMiddleColor, y / middle);
    return mix(fillMiddleColor, fillBottomColor, (y - middle) / (1.0 - middle));
}

void main()
{
    // Physical-edge snap. Fractional layouts at 125%/150%/175% put edges on
    // half-pixel boundaries, splitting a 1px rim into two ~50% rows. Recover
    // the shape's absolute device-space rect from gl_FragCoord, snap its edges
    // onto pixel boundaries, and evaluate the SDF there. axisSign un-flips y
    // on y-up backends so per-corner radii keep their quadrants.
    vec2 localPx = texCoord * quadSizePx;
    vec2 axisSign = vec2(sign(dFdx(localPx.x)), sign(dFdy(localPx.y)));
    vec2 absOrigin = gl_FragCoord.xy - axisSign * localPx;
    vec2 absCenter = absOrigin + axisSign * quadSizePx * 0.5;
    vec2 halfShape = shapeSizePx * 0.5;
    vec2 snappedLo = floor(absCenter - halfShape + 0.5);
    vec2 snappedHi = floor(absCenter + halfShape + 0.5);
    vec2 halfSnapped = max(vec2(0.5), (snappedHi - snappedLo) * 0.5);
    vec2 quadPoint = axisSign * (gl_FragCoord.xy - (snappedHi + snappedLo) * 0.5);

    float cornerMask;
    float outerDistance = roundedRectDistance(quadPoint, halfSnapped,
                                              cornerRadiiPx, shapeInsetPx,
                                              cornerMask);
    float strokeWidth = max(0.0, strokeWidthPx);
    // Derivatives must be evaluated before the per-fragment corner branch.
    // Every sub-sample shares this footprint, keeping QSB translations stable
    // across D3D, Metal, Vulkan, and OpenGL backends.
    float edgeAa = antialiasWidth(outerDistance);
    float outerCoverage = coverage(outerDistance, edgeAa);
    float innerCoverage = coverage(outerDistance + strokeWidth, edgeAa);
    if (cornerMask > 0.0) {
        // Inside a corner arc the single-sample coverage quantizes a small
        // radius into visible steps. Average the same analytic coverage over
        // the four pixel quadrants; straight edges keep the exact single tap.
        float oc = 0.0;
        float ic = 0.0;
        float unused;
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                float d = roundedRectDistance(
                    quadPoint + vec2(float(sx), float(sy)) * 0.25,
                    halfSnapped, cornerRadiiPx, shapeInsetPx, unused);
                oc += coverage(d, edgeAa);
                ic += coverage(d + strokeWidth, edgeAa);
            }
        }
        outerCoverage = mix(outerCoverage, oc * 0.25, cornerMask);
        innerCoverage = mix(innerCoverage, ic * 0.25, cornerMask);
    }
    vec4 result = vec4(0.0);

    // Fully covered fragments clip the shadow to zero below, so skip it.
    if (shadowEnabledValue > 0.5 && outerCoverage < 1.0) {
        vec2 shadowPoint = quadPoint - vec2(0.0, shadowOffsetYPx);
        float shadowCornerMask;
        float shadowDistance = roundedRectDistance(shadowPoint, halfSnapped,
                                                   cornerRadiiPx, 0.0,
                                                   shadowCornerMask);
        // CSS box-shadow semantics: the offset shape convolved with a
        // Gaussian, i.e. 0.5 * erfc(d / (sigma * sqrt2)) — half strength at
        // the shape edge, falling off smoothly outward. A plain Gaussian of
        // the outside distance (full strength AT the edge) reads as a dark
        // outline, and on the unshifted left/right edges as a hard rim.
        float sigma = max(0.75, shadowBlurPx);
        float shadowCoverage = 0.5 - 0.5 * erfApprox(shadowDistance / (sigma * 1.4142136));
        // An outer shadow never paints under its own element: clip by the
        // element's coverage, not the offset shadow shape (that left an
        // unshadowed sliver right under the bottom edge).
        shadowCoverage *= (1.0 - outerCoverage);
        result = premultiplied(shadowColor,
                               shadowCoverage * clamp(shadowStrengthValue, 0.0, 1.0));
    }

    // Fill and rim are disjoint geometric coverage bands. Compositing a
    // fractional rim over an already antialiased full-shape fill counts the
    // same corner coverage twice (an opaque 0.5 edge becomes 0.75), producing
    // the dark stair-step fringe visible on high-contrast focus outlines.
    // Build the full-coverage rim material first, then weight each material by
    // its mutually exclusive area before placing the shape over its shadow.
    bool fillEnabled = fillEnabledValue > 0.5;
    bool strokeEnabled = strokeEnabledValue > 0.5;
    float rimCoverage = max(0.0, outerCoverage - innerCoverage);
    vec2 shapePosition = quadPoint + halfSnapped;
    vec4 fillMaterial = materialColor(shapePosition);
    vec4 strokeMaterial = premultiplied(
        strokeColor, clamp(strokeStrengthValue, 0.0, 1.0));
    vec4 shapeResult = vec4(0.0);

    if (fillEnabled && strokeEnabled) {
        vec4 rimMaterial = over(strokeMaterial, fillMaterial);
        shapeResult = premultiplied(fillMaterial, innerCoverage)
                    + premultiplied(rimMaterial, rimCoverage);
    } else if (fillEnabled) {
        shapeResult = premultiplied(fillMaterial, outerCoverage);
    } else if (strokeEnabled) {
        shapeResult = premultiplied(strokeMaterial, rimCoverage);
    }

    result = over(shapeResult, result);

    fragColor = result * qt_Opacity;
}
