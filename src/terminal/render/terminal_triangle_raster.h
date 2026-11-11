#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

namespace span::terminal {

// Visits the pixels of a width × height image whose centres a triangle
// covers, the way a GPU rasterizes: a centre exactly on an edge two triangles
// share belongs to one of them only (the top-left rule). Corners are in that
// image's pixels; `visit(x, y, weights)` gets each corner's barycentric
// weight, in the corners' own order.
template <typename Visit>
void rasterizeTriangle(const float (&cornerX)[3], const float (&cornerY)[3], int width, int height, Visit &&visit)
{
    // Twice the signed area of (a, b, c); positive when c lies right of a→b
    // on a y-down image.
    const auto edge = [](float ax, float ay, float bx, float by, float cx, float cy) {
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    };
    // The top and left edges of a clockwise triangle.
    const auto topLeft = [](float ax, float ay, float bx, float by) { return (ay == by && bx > ax) || by < ay; };

    int order[3] = {0, 1, 2};
    float area = edge(cornerX[0], cornerY[0], cornerX[1], cornerY[1], cornerX[2], cornerY[2]);
    if (area == 0) return;
    if (area < 0) {
        std::swap(order[1], order[2]);
        area = -area;
    }
    const float x[3] = {cornerX[order[0]], cornerX[order[1]], cornerX[order[2]]};
    const float y[3] = {cornerY[order[0]], cornerY[order[1]], cornerY[order[2]]};
    const bool includes[3] = {topLeft(x[1], y[1], x[2], y[2]), topLeft(x[2], y[2], x[0], y[0]),
                              topLeft(x[0], y[0], x[1], y[1])};
    const int firstX = std::max(0, int(std::floor(std::min({x[0], x[1], x[2]}))));
    const int lastX = std::min(width - 1, int(std::ceil(std::max({x[0], x[1], x[2]}))));
    const int firstY = std::max(0, int(std::floor(std::min({y[0], y[1], y[2]}))));
    const int lastY = std::min(height - 1, int(std::ceil(std::max({y[0], y[1], y[2]}))));
    for (int py = firstY; py <= lastY; ++py) {
        for (int px = firstX; px <= lastX; ++px) {
            const float cx = px + 0.5f;
            const float cy = py + 0.5f;
            const float weight[3] = {edge(x[1], y[1], x[2], y[2], cx, cy), edge(x[2], y[2], x[0], y[0], cx, cy),
                                     edge(x[0], y[0], x[1], y[1], cx, cy)};
            bool inside = true;
            for (int k = 0; k < 3 && inside; ++k) inside = weight[k] > 0 || (weight[k] == 0 && includes[k]);
            if (!inside) continue;
            float weights[3];
            for (int k = 0; k < 3; ++k) weights[order[k]] = weight[k] / area;
            visit(px, py, weights);
        }
    }
}

} // namespace span::terminal
