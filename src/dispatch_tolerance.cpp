// Temporary diagnostic: unregistered indirect guest calls are logged (unique
// targets, appended across runs) and no-op'd instead of fataling. Addresses
// collected here are added to nfscarbon_manifest.toml as function hints.

#include <cstdio>
#include <mutex>
#include <set>

#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

namespace {

void NoopTrap(PPCContext& ctx, uint8_t* /*base*/) {
  static std::mutex mutex;
  static std::set<uint32_t> seen;
  std::lock_guard<std::mutex> lock(mutex);
  if (seen.insert(ctx.last_indirect_target).second) {
    if (std::FILE* f = std::fopen("unregistered_targets.txt", "a")) {
      std::fprintf(f, "0x%08X\n", (unsigned)ctx.last_indirect_target);
      std::fclose(f);
    }
  }
  ctx.r3.u64 = 0;
}

}  // namespace

namespace rex::runtime {

::PPCFunc* ResolveIndirectFunction(uint32_t guest_address) {
  if (Runtime* rt = Runtime::instance()) {
    if (FunctionDispatcher* d = rt->function_dispatcher()) {
      if (::PPCFunc* f = d->GetFunction(guest_address)) {
        return f;
      }
    }
  }
  return &NoopTrap;
}

}  // namespace rex::runtime
