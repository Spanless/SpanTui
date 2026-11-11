#version 440

layout(location = 0) in vec4 qt_Vertex;
layout(location = 1) in vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 texCoord;

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

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    texCoord = qt_MultiTexCoord0;
    gl_Position = qt_Matrix * qt_Vertex;
}
