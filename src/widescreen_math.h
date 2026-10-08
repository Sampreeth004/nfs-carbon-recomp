#pragma once

#include <array>
#include <cmath>

namespace nfscarbon {
using Matrix = std::array<float, 16>;
using Plane = std::array<float, 4>;

inline Matrix Multiply(const Matrix& view, const Matrix& projection) {
  Matrix result{};
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      for (int k = 0; k < 4; ++k) {
        result[row * 4 + col] += view[row * 4 + k] * projection[k * 4 + col];
      }
    }
  }
  return result;
}

// Row-vector D3D projection: -w <= x,y <= w, 0 <= z <= w.
inline std::array<Plane, 6> Frustum(const Matrix& matrix) {
  std::array<Plane, 6> planes{};
  for (int row = 0; row < 4; ++row) {
    const int i = row * 4;
    planes[0][row] = matrix[i + 3] + matrix[i];
    planes[1][row] = matrix[i + 3] - matrix[i];
    planes[2][row] = matrix[i + 3] + matrix[i + 1];
    planes[3][row] = matrix[i + 3] - matrix[i + 1];
    planes[4][row] = matrix[i + 2];
    planes[5][row] = matrix[i + 3] - matrix[i + 2];
  }
  for (Plane& plane : planes) {
    const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1]
                                   + plane[2] * plane[2]);
    if (length > 0.0f) {
      for (float& component : plane) component /= length;
    }
  }
  return planes;
}
}
