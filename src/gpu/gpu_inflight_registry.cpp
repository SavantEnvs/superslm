// GPU paged-KV plan (rev 16), step G3: the boundary token check's registry.
// RED STAGE: the API only. Every function is a placeholder that registers nothing, walks nothing
// and installs no binding, so each host-only cell (tests/gpu-g3-core) runs and goes red for its
// own reason. The green implementation replaces this file.
#include "gpu_inflight_registry.h"

namespace superslm_gpu {
namespace inflight {

namespace detail {
struct RegList {};
}  // namespace detail

struct RegistryAccess {};

void ReserveRegistration() {}
void RegisterNew(InFlightRecord*, InFlightHolder*) noexcept {}
void Reregister(InFlightRecord*, InFlightHolder*) noexcept {}
void Unlink(InFlightRecord*) noexcept {}
GpuApiCallScope::GpuApiCallScope(const char*) noexcept {}
GpuApiCallScope::~GpuApiCallScope() noexcept {}
void NoteEntrySubmitGuardRan() noexcept {}
bool BoundaryGuardCheckRefuses() noexcept { return false; }
RegistryBinding CaptureRegistryBinding() noexcept { return RegistryBinding(); }
GpuTaskBindingScope::GpuTaskBindingScope(const RegistryBinding&) noexcept {}
GpuTaskBindingScope::~GpuTaskBindingScope() noexcept {}
uint64_t BoundaryCheckReportCount() noexcept { return 0; }
uint64_t BoundaryCheckWalkedCount() noexcept { return 0; }
int64_t LiveRecordsForTest() noexcept { return 0; }
std::size_t OwnListEntriesForTest() noexcept { return 0; }
int DepthForTest() noexcept { return 0; }

}  // namespace inflight
}  // namespace superslm_gpu
