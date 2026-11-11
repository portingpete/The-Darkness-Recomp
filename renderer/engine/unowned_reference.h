#pragma once
#include <memory>

namespace DarkRecomp {
// A queued world draw refers to its stored geometry and decoded images
// without owning them: reference counts taken and released for every draw
// were the largest shared cost of the engine and display threads. The owners
// are kept once per pin epoch (a few frames) instead and travel with the
// parts published in that epoch (SimpleMesh::retained), so the references are
// valid while the part that carries the draw exists.
// unownedReference() makes such a reference. Copying, moving and destroying
// it touches no reference count; use_count() is zero.
template<class T> std::shared_ptr<const T> unownedReference(const T* object) noexcept {
    return std::shared_ptr<const T>(std::shared_ptr<const T>(),object);
}
// A real owner of a live object, for anything kept beyond the draw's part: a
// GPU cache entry, a worker job, a weak reference. An object that was never
// owned by a shared pointer is returned as given.
template<class T> std::shared_ptr<const T> ownerOf(const std::shared_ptr<const T>& reference) noexcept {
    if(!reference)return {};
    if(auto owner=reference->weak_from_this().lock())return owner;
    return reference;
}
}
