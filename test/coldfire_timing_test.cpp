#include "cpuState.h"
#include "mc68k.h"
#include "musashiEntry.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace
{
	class TestCpu final : public mc68k::Mc68k
	{
	public:
		explicit TestCpu(unsigned type) : Mc68k(type) {}
		using Mc68k::execInstruction;
		uint8_t read8(uint32_t a) override { return memory.at(a); }
		uint16_t read16(uint32_t a) override { return (read8(a) << 8) | read8(a + 1); }
		uint16_t readImm16(uint32_t a) override { return read16(a); }
		void write8(uint32_t a, uint8_t v) override { memory.at(a) = v; }
		void write16(uint32_t a, uint16_t v) override { write8(a, v >> 8); write8(a + 1, v); }
		std::array<uint8_t, 1024> memory{};
	};

	int failures = 0;
	void check(const char* label, uint32_t got, uint32_t expected)
	{
		if(got == expected) return;
		std::fprintf(stderr, "%s: got %u, expected %u\n", label, got, expected);
		++failures;
	}

	void instruction(const char* name, std::initializer_list<uint16_t> words,
		uint32_t cfCycles, uint32_t legacyCycles, uint16_t sr = 0x2700,
		uint32_t expectedPc = 0)
	{
		for(const auto type : {M68K_CPU_TYPE_MCF5206E, M68K_CPU_TYPE_68020})
		{
			TestCpu cpu(type);
			uint32_t address = 0x20;
			for(const auto word : words) { cpu.write16(address, word); address += 2; }
			cpu.setPC(0x20);
			m68k_set_reg(cpu.getCpuState(), M68K_REG_SR, sr);
			m68k_set_reg(cpu.getCpuState(), M68K_REG_SP, 0x3c0);
			m68k_set_reg(cpu.getCpuState(), M68K_REG_A0, 0x100);
			m68k_set_reg(cpu.getCpuState(), M68K_REG_D0, 100);
			m68k_set_reg(cpu.getCpuState(), M68K_REG_D1, 5);
			const auto cycles = cpu.execInstruction();
			check(name, cycles, type == M68K_CPU_TYPE_MCF5206E ? cfCycles : legacyCycles);
			if(expectedPc) check("branch PC", cpu.getPC(), expectedPc);
		}
	}
}

int main()
{
	// MCF5206EUM tables 3-5..3-11; legacy expectations guard other products.
	instruction("MOVEQ", {0x702a}, 1, 2);
	instruction("MOVE.B load", {0x1010}, 3, 6);
	instruction("MOVE.B store", {0x1080}, 1, 4);
	instruction("MOVE.W load", {0x3010}, 3, 6);
	instruction("MOVE.L indexed load", {0x2030, 0x1800}, 3, 9);
	instruction("LEA indexed", {0x41f0, 0x1800}, 2, 9);
	instruction("NOP", {0x4e71}, 3, 2);
	instruction("TRAP", {0x4e40}, 15, 20);
	instruction("MOVE.L absolute", {0x2039, 0x0000, 0x0100}, 2, 6);
	instruction("MOVE.W register", {0x3200}, 1, 2);
	instruction("ADD.L register", {0xd081}, 1, 2);
	instruction("MULS.L register", {0x4c01, 0x0800}, 18, 43);
	instruction("DIVS.L register", {0x4c41, 0x0800}, 35, 84);
	instruction("LSL.L immediate", {0xe588}, 1, 4);
	instruction("MOVEM.L save two", {0x48d0, 0x0003}, 3, 16);
	instruction("MOVEM.L restore two", {0x4cd0, 0x0003}, 3, 20);
	instruction("MOVE supervisor SR", {0x46fc, 0x2700}, 1, 10);
	instruction("MOVE user SR", {0x46fc, 0x0000}, 7, 10);
	instruction("BRA", {0x6006}, 2, 10, 0x2700, 0x28);
	instruction("BSR.W", {0x6100, 0x0006}, 3, 7, 0x2700, 0x28);
	instruction("BNE forward taken", {0x6606}, 3, 6, 0x2700, 0x28);
	instruction("BNE forward untaken", {0x6606}, 1, 4, 0x2704, 0x22);
	instruction("BNE backward taken", {0x66fc}, 2, 6, 0x2700, 0x1e);
	instruction("BNE backward untaken", {0x66fc}, 3, 4, 0x2704, 0x22);
	instruction("BNE.W forward taken", {0x6600, 0x0006}, 3, 6, 0x2700, 0x28);
	instruction("BNE.W forward untaken", {0x6600, 0x0006}, 1, 6, 0x2704, 0x24);
	instruction("BNE.W backward taken", {0x6600, 0xfffc}, 2, 6, 0x2700, 0x1e);
	instruction("BNE.W backward untaken", {0x6600, 0xfffc}, 3, 6, 0x2704, 0x24);
	return failures ? 1 : 0;
}
