#include "src/widescreen_math.h"

#include <cassert>

using nfscarbon::Matrix;

bool Inside(const Matrix& combined, float x, float y, float z) {
  for (const auto& plane : nfscarbon::Frustum(combined)) {
    if (plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < -0.00001f)
      return false;
  }
  return true;
}

int main() {
  const Matrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  const Matrix retail{9.0f/16,0,0,0, 0,-1,0,0,
                      0,0,100.0f/99.9f,1, 0,0,-10.0f/99.9f,0};
  for (const float aspect : {16.0f/9, 18.0f/9, 20.0f/9, 21.0f/9}) {
    Matrix wide = retail;
    wide[0] *= (16.0f/9) / aspect;
    // Equal world-space lengths have equal physical screen lengths.
    assert(std::abs(wide[0] * aspect - std::abs(wide[5])) < 0.00001f);
    assert(wide[5] == retail[5]);
    assert(nfscarbon::Multiply(identity, wide) == wide);
    assert(Inside(wide, 0, 0, 1));
    assert(!Inside(wide, 0, 0, 0.05f));
    assert(!Inside(wide, 0, 0, 101));
    assert(!Inside(wide, 0, 2, 1));
  }
  Matrix wide = retail;
  wide[0] *= 0.8f;  // 20:9
  assert(!Inside(retail, 2.4f, 0, 1.2f));
  assert(Inside(wide, 2.4f, 0, 1.2f));
  assert(Inside(wide, -2.4f, 0, 1.2f));
  assert(!Inside(wide, 3, 0, 1.2f));
  // Camera translated right by 10: world (10,0,1) is directly ahead.
  Matrix view = identity;
  view[12] = -10;
  const auto combined = nfscarbon::Multiply(view, wide);
  assert(std::abs(combined[12] + 10 * wide[0]) < 0.00001f);
  assert(Inside(combined, 10, 0, 1));
  assert(!Inside(combined, 0, 0, 1));
}
