// RED STAGE: the API with placeholder bodies (no ledger values kept, no check, no drain); the green
// implementation replaces this file.
// GPU paged-KV plan (rev 16), step G3: the submission ledger (§4.4, *The submission ledger*), the
// reset-site check PoolSubmitAssert, the device's submission surface (BeginRecording(), Submit(),
// Wait(v), Poll(v), Drain()) over abstract fence and queue primitives, and GpuSubmission, the
// draining token Submit() returns. D3D12-free: harness::Device (d3d12_harness.h) derives from
// SubmissionDevice and implements the primitives on its private queue, allocator and fence, so the
// ledger's rules are written once, here, and run under ThreadSanitizer on Linux (cell 3.6h).
//
// The ledger's four host-side values, each a std::atomic:
//   submitted_  the fence value of the last submission whose Signal succeeded;
//   observed_   the highest fence value the host has seen complete; it only rises (a
//               compare-and-swap maximum, so two observers that land out of order never lower it);
//   unsignaled_ set by Submit() after ExecuteCommandLists, cleared when a Signal for that work
//               succeeds; still set when Submit() returns or throws means stranded;
//   lost_       set when a wait on a signalled value cannot be armed or confirmed; never cleared.
//
// Submitters are serialised by P1's precondition (one thread at a time inside a reset, a
// submission or a blocking wait on a device). Observers are not: Poll(v) may run beside a Submit()
// on another thread, which the atomics and the monotonic maximum make safe.
//
// The test build (SUPERSLM_LEDGER_YIELD_HOOK, defined on the whole test target so every
// translation unit of the binary sees one definition of this class) carries a hook between the
// load of observed_ and its compare-and-swap, which 3.6h uses to force an interleaving inside one
// update.
#ifndef SUPERSLM_GPU_SUBMISSION_LEDGER_H
#define SUPERSLM_GPU_SUBMISSION_LEDGER_H

#include <atomic>
#include <cstdint>
#include <stdexcept>

#include "gpu_inflight_registry.h"

namespace superslm_gpu {
namespace harness {

// A submission the engine cannot confirm recovered: a command list that could not be confirmed
// Closed, or submitted work whose completion could not be confirmed (a failed fence Signal with no
// successful retry, or a failed SetEventOnCompletion). Every catch site reads it as rule 0 of the
// fault classification (d3d12_harness.h), whatever else is true. (Moved here from d3d12_harness.h
// at G3, unchanged, so the ledger can throw it without D3D12.)
class GpuSubmissionStrandedError : public std::runtime_error {
public:
	explicit GpuSubmissionStrandedError(const char* what) : std::runtime_error(what) {}
};

// The reset-site check's caller-contract failure: a reset while the host has not observed the
// device's last submission, or (the boundary guard check) a reset inside a C ABI call that passed
// no EntrySubmitGuard. A std::logic_error, as the tree's other caller-contract errors are, so the
// fault classifier never reads it as an allocation failure.
class PoolSubmitContractError : public std::logic_error {
public:
	explicit PoolSubmitContractError(const char* what) : std::logic_error(what) {}
};

#if defined(SUPERSLM_LEDGER_YIELD_HOOK)
// Test build only: called with the value being landed, between the load of observed_ and the
// compare-and-swap that raises it.
using LedgerYieldHookFn = void (*)(uint64_t landing_value);
inline std::atomic<LedgerYieldHookFn>& LedgerYieldHookForTest() noexcept {
	static std::atomic<LedgerYieldHookFn> hook{nullptr};
	return hook;
}
#endif

class SubmissionDevice;

class SubmissionLedger {
public:
	// The guard's ledger term (§4.4, *Scope*): Stranded is checked first, then Unobserved.
	enum class GuardTerm { Observed, Unobserved, Stranded };

	SubmissionLedger() noexcept = default;
	SubmissionLedger(const SubmissionLedger&) = delete;
	SubmissionLedger& operator=(const SubmissionLedger&) = delete;

	uint64_t submitted() const noexcept { return submitted_.load(); }
	uint64_t observed() const noexcept { return observed_.load(); }
	bool unsignaled() const noexcept { return unsignaled_.load(); }
	bool lost() const noexcept { return lost_.load(); }
	bool Stranded() const noexcept { return unsignaled_.load() || lost_.load(); }
	// submitted_ is read first: a poller raising observed_ between the two loads can then only
	// make the answer "observed", never a false "unobserved".
	bool Unobserved() const noexcept {
		const uint64_t s = submitted_.load();
		return observed_.load() < s;
	}
	GuardTerm ReadGuardTerm() const noexcept {
		if (Stranded()) return GuardTerm::Stranded;
		if (Unobserved()) return GuardTerm::Unobserved;
		return GuardTerm::Observed;
	}

private:
	friend class SubmissionDevice;

	// The compare-and-swap maximum. A failed exchange reloads `cur`, so a higher value that landed
	// in between is kept and the loop ends.
	void RaiseObserved(uint64_t) noexcept {}
	void MarkUnsignaled() noexcept {}
	void MarkSignaled(uint64_t) noexcept {}
	void MarkLost() noexcept {}

	std::atomic<uint64_t> submitted_{0};
	std::atomic<uint64_t> observed_{0};
	std::atomic<bool> unsignaled_{false};
	std::atomic<bool> lost_{false};
};

// The check, inside BeginRecording() before the reset. It reads host-side values only and never
// queries the fence, so whether it fires does not depend on GPU timing: it fires if and only if the
// host has not observed the last submission complete, or the ledger is stranded.
inline void PoolSubmitAssert(const SubmissionLedger&) {}

class GpuSubmission;
template <class Record>
class BasicGpuInFlightToken;  // gpu_inflight_token.h

// The device's submission surface. harness::Device derives from it and implements the six
// primitives privately on its queue, allocator and fence; nothing else in the library reaches
// them. Every reset goes through BeginRecording() and every submission through Submit().
class SubmissionDevice {
public:
	SubmissionDevice(const SubmissionDevice&) = delete;
	SubmissionDevice& operator=(const SubmissionDevice&) = delete;

	// The boundary guard check, then PoolSubmitAssert, then the allocator's and the list's reset.
	// Throws GpuSubmissionStrandedError (stranded ledger) or PoolSubmitContractError (unobserved
	// ledger, or a C ABI call that passed no EntrySubmitGuard) before the reset.
	void BeginRecording() { ResetAllocatorAndList(); }

	// ExecuteCommandLists, unsignaled_ set, Signal at a fresh value; see the definition below.
	[[nodiscard]] GpuSubmission Submit(bool retry = true);

	// Today's fence wait: returns without arming the event when the fence is already at `v`.
	// Raises observed_ to `v` only once the fence has reached it. A wait that cannot be armed or
	// confirmed sets lost_ and throws GpuSubmissionStrandedError.
	void Wait(uint64_t) {}
	bool Poll(uint64_t v) noexcept { return CompletedFenceValue() >= v; }
	void Drain() {}

	const SubmissionLedger& ledger() const noexcept { return ledger_; }

protected:
	SubmissionDevice() noexcept = default;
	virtual ~SubmissionDevice() = default;

	// The primitives (harness::Device: alloc->Reset() then list->Reset(alloc, nullptr);
	// [the queue gate's wait, fault-injection builds]; ExecuteCommandLists; queue->Signal(fence, v);
	// fence->GetCompletedValue(); SetEventOnCompletion and WaitForSingleObject).
	virtual void ResetAllocatorAndList() = 0;
	virtual void ExecuteRecordedList() = 0;
	// Returns false on a failed Signal, with the failure's code (an HRESULT on the device) in
	// `*failure`; ThrowSignalFailure(code) throws that failure as the classifier expects it
	// (GpuAllocationError for E_OUTOFMEMORY, std::runtime_error otherwise, or the T2169 seam's own).
	virtual bool SignalFence(uint64_t value, int32_t* failure) noexcept = 0;
	[[noreturn]] virtual void ThrowSignalFailure(int32_t failure) = 0;
	virtual uint64_t CompletedFenceValue() noexcept = 0;
	// Arms the fence event at `value` and waits for it; false when the wait cannot be armed or
	// confirmed (SetEventOnCompletion failed). Called only while the fence is below `value`.
	virtual bool WaitFenceEvent(uint64_t value) noexcept = 0;

	// The last fence value a Signal was attempted at (today's fence_val). Only submitters touch
	// it, under P1's precondition.
	uint64_t fence_value_counter() const noexcept { return fence_val_; }

private:
	friend class GpuSubmission;

	// The non-throwing drain every token's destructor uses: false when the wait strands, with
	// lost_ set.
	bool WaitNoThrow(uint64_t) noexcept { return true; }

	SubmissionLedger ledger_;
	uint64_t fence_val_ = 0;
};

// The draining token Submit() returns: move-only, holding the submission's record (its device and
// fence value). A synchronous site ends it with Wait(), today's throwing wait, which observes the
// value and empties it; destroyed while it still holds its record, it drains through the
// non-throwing drain and frees the record, so an early return, a jump or an exception between
// Submit() and the wait leaves nothing unobserved. The asynchronous submitters move it into the
// layer-loop record a GpuInFlightToken carries (gpu_inflight_token.h).
class [[nodiscard]] GpuSubmission final : public inflight::InFlightHolder {
public:
	GpuSubmission() noexcept = default;
	GpuSubmission(GpuSubmission&& other) noexcept : rec_(other.rec_) {
		other.rec_ = nullptr;
	}
	// A move-assignment into a submission that still holds a record drains that record first.
	GpuSubmission& operator=(GpuSubmission&& other) noexcept {
		if (this != &other) {
			rec_ = other.rec_;
			other.rec_ = nullptr;
		}
		return *this;
	}
	~GpuSubmission() { delete rec_; }

	bool holds() const noexcept { return rec_ != nullptr; }
	uint64_t fence_value() const noexcept { return rec_ ? rec_->fence_value() : 0; }
	// True once the fence has reached this submission's value (observed_ raised); never waits and
	// never empties the token. An empty submission reads as reached.
	bool ReadyNoWait() const noexcept { return rec_ == nullptr || rec_->device()->Poll(rec_->fence_value()); }

	// The throwing wait: observes the value, frees the record and empties the token; on a wait
	// that cannot be confirmed the ledger is stranded (lost_), the record is freed, and
	// GpuSubmissionStrandedError is thrown.
	void Wait() {
		delete rec_;
		rec_ = nullptr;
	}
	// The non-throwing drain: false when the wait strands (lost_ set). Frees the record and empties
	// the token either way.
	bool DrainNoThrow() noexcept {
		inflight::InFlightRecord* rec = rec_;
		rec_ = nullptr;
		if (rec == nullptr) return true;
		const bool ok = rec->device()->WaitNoThrow(rec->fence_value());
		inflight::Unlink(rec);
		delete rec;
		return ok;
	}

private:
	friend class SubmissionDevice;
	template <class Record>
	friend class BasicGpuInFlightToken;

	explicit GpuSubmission(inflight::InFlightRecord* rec) noexcept : rec_(rec) { inflight::RegisterNew(rec_, this); }
	void DrainFromWalk() noexcept override { DrainNoThrow(); }

	inflight::InFlightRecord* rec_ = nullptr;
};

// Submit(retry): ExecuteCommandLists, then unsignaled_ set, then Signal at a fresh value. On
// success submitted_ is raised and unsignaled_ cleared, and the record is handed out in a
// GpuSubmission. On a failed Signal with `retry` (the default) Submit() signals once more at a
// fresh value, never the failed one; if that succeeds it raises submitted_, clears unsignaled_,
// drains the fresh value and rethrows the first failure (a recovered retry leaves nothing in
// flight); if the retry fails, or that drain strands, unsignaled_ (or lost_) stays set and it
// throws GpuSubmissionStrandedError (rule 0). Without `retry` a failed Signal is stranded at once.
// The record and this thread's registry list are allocated before ExecuteCommandLists, so an
// allocation failure submits nothing.
inline GpuSubmission SubmissionDevice::Submit(bool) {
	inflight::InFlightRecord* rec = new inflight::InFlightRecord(this);
	ExecuteRecordedList();
	int32_t failure = 0;
	rec->value_ = ++fence_val_;
	(void)SignalFence(rec->value_, &failure);
	return GpuSubmission(rec);
}

}  // namespace harness
}  // namespace superslm_gpu

#endif  // SUPERSLM_GPU_SUBMISSION_LEDGER_H
