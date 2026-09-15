#pragma once
#include <cstdint>
#include <string>
#include "runtime/guest/guest_types.h"

namespace DarkRecomp {

void InitializeFileSystem();

// Kernel function implementations
void Hook_NtOpenFile(PPCContext& ctx, uint8_t* base);
void Hook_NtCreateFile(PPCContext& ctx, uint8_t* base);
void Hook_NtReadFile(PPCContext& ctx, uint8_t* base);
void Hook_NtReadFileScatter(PPCContext& ctx, uint8_t* base);
void Hook_NtWriteFile(PPCContext& ctx, uint8_t* base);
void Hook_NtClose(PPCContext& ctx, uint8_t* base);
void Hook_NtSetInformationFile(PPCContext& ctx, uint8_t* base);
void Hook_NtQueryInformationFile(PPCContext& ctx, uint8_t* base);
void Hook_NtQueryFullAttributesFile(PPCContext& ctx, uint8_t* base);
void Hook_NtFlushBuffersFile(PPCContext& ctx, uint8_t* base);

} // namespace DarkRecomp
