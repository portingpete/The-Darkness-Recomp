#include "native_menu_text.h"
#include "native_menu_text.generated.h"
#include <atomic>

namespace DarkRecomp::Native {
namespace {
std::atomic<bool> russian{false};
}
void initializeNativeMenuText(bool russianRevision, uint32_t consoleLanguage) noexcept {
    russian.store(russianRevision && consoleLanguage == 1, std::memory_order_relaxed);
}
bool russianNativeMenus() noexcept { return russian.load(std::memory_order_relaxed); }
std::string_view nativeMenuText(std::string_view english) noexcept {
    return russianNativeMenus() ? russianNativeMenuText(english) : english;
}
}
