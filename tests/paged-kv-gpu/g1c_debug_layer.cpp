// Paged-KV plan (rev 16.2), G1, group g1c: the debug-layer message count of plan §7's grading rule
// ("the debug-layer message count, read by the test from the device's info queue"), for 3.6 and
// 3.6d's scope control. The one translation unit of this group that includes the engine's
// d3d12_harness.h, as tests/t2178-gpu-batched-prefill-red-suite/cell_trust_and_guard.cpp does.
//
// The count is the harness's DrainDebugLayerMessages(): WARNING-or-worse messages since the last
// drain. D3D12 devices are one per adapter in a process, so the singleton's info queue also carries
// what the per-context devices' work raised (the pool create's zero-fill included).
//
// A build that compiles the library with SUPERSLM_GPU_ALLOC_FAULT_INJECTION must compile this file
// with it too (te425_getdevice.cpp's reason: the header's inline members must be the library's).
//
// KNOWN LIMIT, carried as-is: T-2195 found the layer blind to a desynced resume-barrier latch on the
// RTX 2080 SUPER (driver 560.94), so on that device a 0 is "no message", not "no missing barrier";
// 3.6's byte-equal tokens are its second operand for the latch (see g1c_cell_3_6.cpp).
#include "d3d12_harness.h"

#include "g1c_helpers.h"

size_t g1c::DebugLayerReportable(bool* enabled) {
	superslm_gpu::harness::Device& dev = superslm_gpu::harness::GetDevice();
	*enabled = dev.debug_layer_enabled && dev.debug_info_queue;
	return dev.DrainDebugLayerMessages();
}
