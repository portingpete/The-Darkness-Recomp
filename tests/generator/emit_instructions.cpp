#include "recompiler.h"
#include <array>
#include <stdexcept>

struct FixtureSpec {
    const char* mnemonic;
    int opcode;
    const char* kind;
    int destination, a, b, shift;
    bool record;
};

static bool translate(const FixtureSpec& spec, Recompiler& compiler) {
    std::string name = std::string(spec.mnemonic) + (spec.record ? "." : "");
    powerpc_opcode opcode{};
    opcode.name = name.c_str();
    opcode.id = spec.opcode;
    ppc_insn instruction{};
    instruction.opcode = &opcode;
    instruction.operands[0] = spec.destination;
    instruction.operands[1] = spec.a;
    instruction.operands[2] = std::string_view(spec.kind) == "Convert" ? spec.shift : spec.b;
    instruction.operands[3] = spec.shift;
    RecompilerLocalVariables locals{};
    CSRState state = CSRState::Unknown;
    auto table = compiler.config.switchTables.end();
    uint32_t data[2]{};
    return compiler.Recompile(Function(0x1000, 4), 0x1000, instruction, data, table, locals, state);
}

static int check_diagnostics(const char* directory, const char* header) {
    std::filesystem::create_directories(directory);
    Recompiler compiler;
    compiler.config.outDirectoryPath = directory;
    compiler.diagnosticCount = 1; // An error already found by Analyse().
    int failures = compiler.Recompile(std::filesystem::path(header)) || compiler.diagnosticCount != 1;
    for (auto [name, opcode] : {std::pair{"frsqrte", PPC_INST_FRSQRTE},
                               std::pair{"frsqrtes", PPC_INST_FRSQRTES},
                               std::pair{"fnmadd", PPC_INST_FNMADD}}) {
        Recompiler record;
        failures += translate({name, opcode, "Scalar", 0, 1, 2, 0, true}, record);
        Recompiler plain;
        failures += !translate({name, opcode, "Scalar", 0, 1, 2, 0, false}, plain);
    }
    std::printf("Analysis diagnostic retention and unsupported floating record forms: %d failures\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--check-diagnostics")
        return check_diagnostics(argv[2], argv[3]);
    if (argc != 2) return 1;
    std::vector<FixtureSpec> specs;
    for (auto [name, opcode] : {std::pair{"vctuxs", PPC_INST_VCTUXS},
                               std::pair{"vcfpuxws128", PPC_INST_VCFPUXWS128}})
        for (int shift : {0, 1, 31})
            for (int destination : {0, 1})
                specs.push_back({name, opcode, "Convert", destination, 1, 2, shift, false});
    const std::array<std::array<int, 3>, 5> aliases{{{0, 1, 2}, {1, 1, 2}, {2, 1, 2}, {0, 1, 1}, {1, 1, 1}}};
    for (auto [name, opcode] : {std::pair{"vpkuwum", PPC_INST_VPKUWUM},
                               std::pair{"vpkuwum128", PPC_INST_VPKUWUM128},
                               std::pair{"vpkuhus", PPC_INST_VPKUHUS},
                               std::pair{"vpkuhus128", PPC_INST_VPKUHUS128}})
        for (auto registers : aliases)
            specs.push_back({name, opcode,
                opcode == PPC_INST_VPKUWUM || opcode == PPC_INST_VPKUWUM128 ? "PackWords" : "PackHalves",
                registers[0], registers[1], registers[2], 0, false});
    for (auto [name, opcode] : {std::pair{"vcmpequh", PPC_INST_VCMPEQUH},
                               std::pair{"vcmpgtsh", PPC_INST_VCMPGTSH},
                               std::pair{"vcmpgtuh", PPC_INST_VCMPGTUH}})
        for (bool record : {false, true})
            for (int destination : {0, 1, 2})
                specs.push_back({name, opcode, opcode == PPC_INST_VCMPEQUH ? "CompareEqual" :
                    opcode == PPC_INST_VCMPGTSH ? "CompareSigned" : "CompareUnsigned",
                    destination, 1, 2, 0, record});
    for (int destination : {0, 1, 2, 3})
        specs.push_back({"fnmadd", PPC_INST_FNMADD, "FusedNegativeMultiplyAdd", destination, 1, 2, 3, false});
    for (bool record : {false, true})
        for (int destination : {0, 1, 2})
            specs.push_back({"mulhdu", PPC_INST_MULHDU, "MultiplyHighUnsigned", destination, 1, 2, 0, record});

    std::ofstream output(argv[1]);
    for (size_t index = 0; index < specs.size(); ++index) {
        const auto& spec = specs[index];
        Recompiler compiler;
        if (!translate(spec, compiler))
            throw std::runtime_error(std::string("Regression instruction was not translated: ") + spec.mnemonic);
        output << "static void translated_" << index << "(PPCContext& ctx) {\n"
                  "    [[maybe_unused]] PPCRegister temp{}; [[maybe_unused]] PPCVRegister vTemp{};\n" << compiler.out << "}\n";
    }
    output << "static const Fixture fixtures[] = {\n";
    for (size_t index = 0; index < specs.size(); ++index) {
        const auto& spec = specs[index];
        output << "{\"" << spec.mnemonic << "\", Kind::" << spec.kind << ", " << spec.destination
               << ", " << spec.a << ", " << spec.b << ", " << spec.shift << ", "
               << (spec.record ? "true" : "false") << ", translated_" << index << "},\n";
    }
    output << "};\n";
    return output.good() ? 0 : 1;
}
