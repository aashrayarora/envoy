#include "common/buffer/buffer_impl.h"

#include <cstdint>
#include <string>

#include "common/common/assert.h"
#include "common/common/stack_array.h"

#include "event2/buffer.h"

namespace Envoy {
namespace Buffer {

// RawSlice is the same structure as evbuffer_iovec. This was put into place to avoid leaking
// libevent into most code since we will likely replace evbuffer with our own implementation at
// some point. However, we can avoid a bunch of copies since the structure is the same.
static_assert(sizeof(RawSlice) == sizeof(evbuffer_iovec), "RawSlice != evbuffer_iovec");
static_assert(offsetof(RawSlice, mem_) == offsetof(evbuffer_iovec, iov_base),
              "RawSlice != evbuffer_iovec");
static_assert(offsetof(RawSlice, len_) == offsetof(evbuffer_iovec, iov_len),
              "RawSlice != evbuffer_iovec");

void OwnedImpl::add(const void* data, uint64_t size) { evbuffer_add(buffer_.get(), data, size); }

void OwnedImpl::addBufferFragment(BufferFragment& fragment) {
  evbuffer_add_reference(
      buffer_.get(), fragment.data(), fragment.size(),
      [](const void*, size_t, void* arg) { static_cast<BufferFragment*>(arg)->done(); }, &fragment);
}

void OwnedImpl::add(absl::string_view data) {
  evbuffer_add(buffer_.get(), data.data(), data.size());
}

void OwnedImpl::add(const Instance& data) {
  ASSERT(&data != this);
  uint64_t num_slices = data.getRawSlices(nullptr, 0);
  STACK_ARRAY(slices, RawSlice, num_slices);
  data.getRawSlices(slices.begin(), num_slices);
  for (const RawSlice& slice : slices) {
    add(slice.mem_, slice.len_);
  }
}

void OwnedImpl::prepend(absl::string_view data) {
  // Prepending an empty string seems to mess up libevent internally.
  // evbuffer_prepend doesn't have a check for empty (unlike
  // evbuffer_prepend_buffer which does). This then results in an allocation of
  // an empty chain, which causes problems with a following move/append. This
  // only seems to happen the original buffer was created via
  // addBufferFragment(), this forces the code execution path in
  // evbuffer_prepend related to immutable buffers.
  if (data.size() == 0) {
    return;
  }
  evbuffer_prepend(buffer_.get(), data.data(), data.size());
}

void OwnedImpl::prepend(Instance& data) {
  ASSERT(&data != this);
  int rc =
      evbuffer_prepend_buffer(buffer_.get(), static_cast<LibEventInstance&>(data).buffer().get());
  ASSERT(rc == 0);
  ASSERT(data.length() == 0);
  static_cast<LibEventInstance&>(data).postProcess();
}

void OwnedImpl::commit(RawSlice* iovecs, uint64_t num_iovecs) {
  int rc =
      evbuffer_commit_space(buffer_.get(), reinterpret_cast<evbuffer_iovec*>(iovecs), num_iovecs);
  ASSERT(rc == 0);
}

void OwnedImpl::copyOut(size_t start, uint64_t size, void* data) const {
  ASSERT(start + size <= length());

  evbuffer_ptr start_ptr;
  int rc = evbuffer_ptr_set(buffer_.get(), &start_ptr, start, EVBUFFER_PTR_SET);
  ASSERT(rc != -1);

  ev_ssize_t copied = evbuffer_copyout_from(buffer_.get(), &start_ptr, data, size);
  ASSERT(static_cast<uint64_t>(copied) == size);
}

void OwnedImpl::drain(uint64_t size) {
  ASSERT(size <= length());
  int rc = evbuffer_drain(buffer_.get(), size);
  ASSERT(rc == 0);
}

uint64_t OwnedImpl::getRawSlices(RawSlice* out, uint64_t out_size) const {
  return evbuffer_peek(buffer_.get(), -1, nullptr, reinterpret_cast<evbuffer_iovec*>(out),
                       out_size);
}

uint64_t OwnedImpl::length() const { return evbuffer_get_length(buffer_.get()); }

void* OwnedImpl::linearize(uint32_t size) {
  ASSERT(size <= length());
  void* const ret = evbuffer_pullup(buffer_.get(), size);
  RELEASE_ASSERT(ret != nullptr || size == 0,
                 "Failure to linearize may result in buffer overflow by the caller.");
  return ret;
}

void OwnedImpl::move(Instance& rhs) {
  ASSERT(&rhs != this);
  // We do the static cast here because in practice we only have one buffer implementation right
  // now and this is safe. Using the evbuffer move routines require having access to both evbuffers.
  // This is a reasonable compromise in a high performance path where we want to maintain an
  // abstraction in case we get rid of evbuffer later.
  int rc = evbuffer_add_buffer(buffer_.get(), static_cast<LibEventInstance&>(rhs).buffer().get());
  ASSERT(rc == 0);
  static_cast<LibEventInstance&>(rhs).postProcess();
}

void OwnedImpl::move(Instance& rhs, uint64_t length) {
  ASSERT(&rhs != this);
  // See move() above for why we do the static cast.
  int rc = evbuffer_remove_buffer(static_cast<LibEventInstance&>(rhs).buffer().get(), buffer_.get(),
                                  length);
  ASSERT(static_cast<uint64_t>(rc) == length);
  static_cast<LibEventInstance&>(rhs).postProcess();
}

Api::IoCallUint64Result OwnedImpl::read(Network::IoHandle& io_handle, uint64_t max_length) {
  if (max_length == 0) {
    return Api::ioCallUint64ResultNoError();
  }
  constexpr uint64_t MaxSlices = 2;
  RawSlice slices[MaxSlices];
  const uint64_t num_slices = reserve(max_length, slices, MaxSlices);
  Api::IoCallUint64Result result = io_handle.readv(max_length, slices, num_slices);
  if (result.ok()) {
    // Read succeeded.
    uint64_t num_slices_to_commit = 0;
    uint64_t bytes_to_commit = result.rc_;
    ASSERT(bytes_to_commit <= max_length);
    while (bytes_to_commit != 0) {
      slices[num_slices_to_commit].len_ =
          std::min(slices[num_slices_to_commit].len_, static_cast<size_t>(bytes_to_commit));
      ASSERT(bytes_to_commit >= slices[num_slices_to_commit].len_);
      bytes_to_commit -= slices[num_slices_to_commit].len_;
      num_slices_to_commit++;
    }
    ASSERT(num_slices_to_commit <= num_slices);
    commit(slices, num_slices_to_commit);
  }
  return result;
}

uint64_t OwnedImpl::reserve(uint64_t length, RawSlice* iovecs, uint64_t num_iovecs) {
  ASSERT(length > 0);
  int ret = evbuffer_reserve_space(buffer_.get(), length, reinterpret_cast<evbuffer_iovec*>(iovecs),
                                   num_iovecs);
  RELEASE_ASSERT(ret >= 1, "Failure to allocate may result in callers writing to uninitialized "
                           "memory, buffer overflows, etc");
  return static_cast<uint64_t>(ret);
}

ssize_t OwnedImpl::search(const void* data, uint64_t size, size_t start) const {
  evbuffer_ptr start_ptr;
  if (-1 == evbuffer_ptr_set(buffer_.get(), &start_ptr, start, EVBUFFER_PTR_SET)) {
    return -1;
  }

  evbuffer_ptr result_ptr =
      evbuffer_search(buffer_.get(), static_cast<const char*>(data), size, &start_ptr);
  return result_ptr.pos;
}

Api::IoCallUint64Result OwnedImpl::write(Network::IoHandle& io_handle) {
  constexpr uint64_t MaxSlices = 16;
  RawSlice slices[MaxSlices];
  const uint64_t num_slices = std::min(getRawSlices(slices, MaxSlices), MaxSlices);
  Api::IoCallUint64Result result = io_handle.writev(slices, num_slices);
  if (result.ok() && result.rc_ > 0) {
    drain(static_cast<uint64_t>(result.rc_));
  }
  return result;
}

OwnedImpl::OwnedImpl() : buffer_(evbuffer_new()) {}

OwnedImpl::OwnedImpl(absl::string_view data) : OwnedImpl() { add(data); }

OwnedImpl::OwnedImpl(const Instance& data) : OwnedImpl() { add(data); }

OwnedImpl::OwnedImpl(const void* data, uint64_t size) : OwnedImpl() { add(data, size); }

std::string OwnedImpl::toString() const {
  uint64_t num_slices = getRawSlices(nullptr, 0);
  STACK_ARRAY(slices, RawSlice, num_slices);
  getRawSlices(slices.begin(), num_slices);
  size_t len = 0;
  for (const RawSlice& slice : slices) {
    len += slice.len_;
  }
  std::string output;
  output.reserve(len);
  for (const RawSlice& slice : slices) {
    output.append(static_cast<const char*>(slice.mem_), slice.len_);
  }

  return output;
}

} // namespace Buffer
} // namespace Envoy
