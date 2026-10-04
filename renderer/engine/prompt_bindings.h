#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <string>

namespace DarkRecomp::Prompts {
// Value-only labels shared by input configuration and the renderer. The
// renderer takes an immutable snapshot; it never reaches into game input.
using BindingLabels = std::array<std::string, 22>;
inline BindingLabels defaultBindingLabels() {
    return {"E","R/Esc","F","Spc","Q","G","RMB","LMB","Ct","Sh/MMB","Ent","Tab",
            "3/^","4/v","1/<","2/>","3/4","1/2","WASD","Mse","WASD/Mse","Ct/Sh"};
}
inline std::atomic<std::shared_ptr<const BindingLabels>>& bindingLabelStore() {
    static std::atomic<std::shared_ptr<const BindingLabels>> labels{
        std::make_shared<const BindingLabels>(defaultBindingLabels())};
    return labels;
}
inline void setBindingLabels(BindingLabels labels) {
    bindingLabelStore().store(std::make_shared<const BindingLabels>(std::move(labels)), std::memory_order_release);
}
inline std::shared_ptr<const BindingLabels> bindingLabels() {
    return bindingLabelStore().load(std::memory_order_acquire);
}
}
