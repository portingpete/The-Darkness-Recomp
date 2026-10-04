#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace DarkRecomp::Prompts {
// Value-only labels shared by input configuration and the renderer. The
// renderer takes an immutable snapshot; it never reaches into game input.
using BindingLabels = std::array<std::string, 22>;
inline BindingLabels defaultBindingLabels() {
    return {"E","R/Esc","F","Spc","Q","G","RMB","LMB","Ct","Sh/MMB","Ent","Tab",
            "3/^","4/v","1/<","2/>","3/4","1/2","WASD","Mse","WASD/Mse","Ct/Sh"};
}
inline BindingLabels defaultGameplayBindingLabels() {
    return {"E","R","F","Spc","Q","G","RMB","LMB","Ct","Sh/MMB","Ent","Tab",
            "3","4","1","2","3/4","1/2","WASD","Mse","WASD/Mse","Ct/Sh"};
}
struct BindingLabelSet { BindingLabels menu, gameplay; };
inline std::atomic<std::shared_ptr<const BindingLabelSet>>& bindingLabelStore() {
    static std::atomic<std::shared_ptr<const BindingLabelSet>> labels{
        std::make_shared<const BindingLabelSet>(BindingLabelSet{defaultBindingLabels(), defaultGameplayBindingLabels()})};
    return labels;
}
// Both contexts belong to one configuration generation. Keep an owned snapshot
// of that generation alive even when a binding changes during queued rendering.
inline void setBindingLabels(BindingLabels menu, BindingLabels gameplay) {
    bindingLabelStore().store(std::make_shared<const BindingLabelSet>(
        BindingLabelSet{std::move(menu), std::move(gameplay)}), std::memory_order_release);
}
// Custom artwork callers can explicitly share their labels between contexts.
inline void setBindingLabels(BindingLabels labels) {
    setBindingLabels(labels, labels);
}
inline std::shared_ptr<const BindingLabels> bindingLabels(bool gameplay = false) {
    auto snapshot = bindingLabelStore().load(std::memory_order_acquire);
    const auto* selected = gameplay ? &snapshot->gameplay : &snapshot->menu;
    return std::shared_ptr<const BindingLabels>(std::move(snapshot), selected);
}
}
