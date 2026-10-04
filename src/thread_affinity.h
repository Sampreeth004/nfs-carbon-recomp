#pragma once

#include <memory>

namespace nfscarbon::afinidad {

class Vigilante;

struct BorrarVigilante {
  void operator()(Vigilante* v) const;
};

using VigilantePtr = std::unique_ptr<Vigilante, BorrarVigilante>;

VigilantePtr Arrancar();

}  // namespace nfscarbon::afinidad
