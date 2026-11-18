#include "cpuState.h"
#include "mc68k.h"
#include "musashiEntry.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

// m68k_execute_run must execute exactly what the same number of m68k_execute_one calls
// execute, account the cycles as documented in m68k.h, and end on each of its three
// conditions. A published code page must give the same result as the host callback,
// without calling it, and must show writes to the page at once.
namespace
{
	constexpr uint32_t g_program = 0x1000;
	constexpr uint32_t g_patchedInstruction = 0x101a;
	constexpr uint32_t g_selfBranch = 0x101c;

	class TestCpu final : public mc68k::Mc68k
	{
	public:
		TestCpu() : Mc68k(M68K_CPU_TYPE_MCF5206E)
		{
			uint32_t address = g_program;
			for(const uint16_t word : {
				0x7005,						// 1000  moveq   #5,d0
				0x223c, 0x1234, 0x5678,		// 1002  move.l  #$12345678,d1
				0xd280,						// 1008  add.l   d0,d1
				0x5380,						// 100a  subq.l  #1,d0
				0x66fa,						// 100c  bne.s   $1008
				0x227c, 0x0000, 0x101a,		// 100e  movea.l #$101a,a1
				0x343c, 0x7602,				// 1014  move.w  #$7602,d2
				0x3282,						// 1018  move.w  d2,(a1)     the only write: turns the next
				0x7601,						// 101a  moveq   #1,d3       instruction into moveq #2,d3
				0x60fe})					// 101c  bra.s   $101c
			{
				memory[address] = static_cast<uint8_t>(word >> 8);
				memory[address + 1] = static_cast<uint8_t>(word);
				address += 2;
			}
			setPC(g_program);
			m68k_set_reg(getCpuState(), M68K_REG_SR, 0x2700);
			m68k_set_reg(getCpuState(), M68K_REG_SP, 0x0f00);
		}

		uint8_t read8(uint32_t a) override { return memory.at(a); }
		uint16_t read16(uint32_t a) override { return static_cast<uint16_t>((read8(a) << 8) | read8(a + 1)); }
		uint16_t readImm16(uint32_t a) override { ++immediateCalls; return read16(a); }
		void write8(uint32_t a, uint8_t v) override
		{
			memory.at(a) = v;
			if(stopOnWrite)
				stop = 1;
		}
		void write16(uint32_t a, uint16_t v) override
		{
			write8(a, static_cast<uint8_t>(v >> 8));
			write8(a + 1, static_cast<uint8_t>(v));
		}

		m68k_run makeRun(const uint64_t _limit, const unsigned _stopOpcode)
		{
			m68k_run run{};
			run.cycles = &cycles;
			run.clock = &clock;
			run.owed = &owed;
			run.stop = &stop;
			run.limit = _limit;
			run.stop_opcode = _stopOpcode;
			return run;
		}

		std::array<uint8_t, 0x2000> memory{};
		unsigned immediateCalls = 0;
		bool stopOnWrite = false;
		unsigned char stop = 0;
		uint64_t cycles = 0;
		uint64_t clock = 0;
		uint32_t owed = 0;
	};

	int failures = 0;
	void check(const char* _label, const uint64_t _got, const uint64_t _expected)
	{
		if(_got == _expected)
			return;
		std::fprintf(stderr, "%s: got %llu, expected %llu\n", _label,
			static_cast<unsigned long long>(_got), static_cast<unsigned long long>(_expected));
		++failures;
	}

	bool sameCpu(TestCpu& _a, TestCpu& _b)
	{
		return std::memcmp(static_cast<m68ki_cpu_core*>(_a.getCpuState()),
			static_cast<m68ki_cpu_core*>(_b.getCpuState()), M68KI_CPU_STATE_SIZE) == 0
			&& _a.memory == _b.memory;
	}

	struct Reference
	{
		uint64_t cycles = 0;
		uint64_t instructions = 0;
		uint32_t lastCycles = 0;
	};

	// One instruction at a time, until _instructions have run or the self-branch has executed.
	Reference step(TestCpu& _cpu, const uint64_t _instructions)
	{
		Reference r;
		while(r.instructions < _instructions)
		{
			r.lastCycles = static_cast<uint32_t>(m68k_execute_one(_cpu.getCpuState()));
			r.cycles += r.lastCycles;
			++r.instructions;
			if(_cpu.getCpuState()->ir == 0x60fe)
				break;
		}
		return r;
	}

	constexpr uint64_t g_noLimit = std::numeric_limits<uint64_t>::max();
	constexpr uint64_t g_allInstructions = 22;		// 2, five passes of three, 3, the patched one, the branch
	constexpr uint64_t g_throughWrite = 20;

	void wholeProgram()
	{
		TestCpu a, b;
		const auto ref = step(a, g_noLimit);
		check("reference instruction count", ref.instructions, g_allInstructions);
		check("reference d1", a.getDReg(1), 0x12345678u + 15u);
		check("reference d3 (patched instruction)", a.getDReg(3), 2);

		auto run = b.makeRun(g_noLimit, 0x60fe);
		const auto last = m68k_execute_run(b.getCpuState(), &run);
		check("run ends on the stop opcode", run.ended_on_opcode, 1);
		check("run instruction count", run.instructions, ref.instructions);
		check("run elapsed cycles", run.elapsed, ref.cycles);
		check("run last instruction cycles", last, ref.lastCycles);
		check("cycles counter holds every instruction", b.cycles, ref.cycles);
		check("clock holds all but the last instruction", b.clock, ref.cycles - ref.lastCycles);
		check("owed holds all but the last instruction", b.owed, ref.cycles - ref.lastCycles);
		check("run leaves the same CPU and memory", sameCpu(a, b), 1);
	}

	void limit()
	{
		TestCpu a, b, c;
		const auto one = step(a, 1);

		// The instruction that reaches the limit is the last one: nothing goes to clock and owed.
		auto run = b.makeRun(1, 0x60fe);
		auto last = m68k_execute_run(b.getCpuState(), &run);
		check("limit 1: instructions", run.instructions, 1);
		check("limit 1: elapsed", run.elapsed, one.cycles);
		check("limit 1: returned cycles", last, one.lastCycles);
		check("limit 1: clock", b.clock, 0);
		check("limit 1: owed", b.owed, 0);
		check("limit 1: not the stop opcode", run.ended_on_opcode, 0);
		check("limit 1: same CPU", sameCpu(a, b), 1);

		// One cycle more lets the second instruction start.
		const auto two = step(a, 1);
		run = c.makeRun(one.cycles + 1, 0x60fe);
		last = m68k_execute_run(c.getCpuState(), &run);
		check("limit past first: instructions", run.instructions, 2);
		check("limit past first: elapsed", run.elapsed, one.cycles + two.cycles);
		check("limit past first: returned cycles", last, two.lastCycles);
		check("limit past first: clock", c.clock, one.cycles);
		check("limit past first: owed", c.owed, one.cycles);
		check("limit past first: same CPU", sameCpu(a, c), 1);
	}

	void stopFlag()
	{
		TestCpu a, b;
		const auto ref = step(a, g_throughWrite);

		// The host sets the flag from a callback inside the instruction that writes.
		b.stopOnWrite = true;
		auto run = b.makeRun(g_noLimit, 0x60fe);
		const auto last = m68k_execute_run(b.getCpuState(), &run);
		check("stop flag: instructions", run.instructions, g_throughWrite);
		check("stop flag: elapsed", run.elapsed, ref.cycles);
		check("stop flag: returned cycles", last, ref.lastCycles);
		check("stop flag: program counter", b.getPC(), g_patchedInstruction);
		check("stop flag: not the stop opcode", run.ended_on_opcode, 0);
		check("stop flag: same CPU", sameCpu(a, b), 1);

		// A host callback may take what is owed in the middle of a run.
		check("stop flag: owed before the last instruction", b.owed, ref.cycles - ref.lastCycles);
	}

	void codePage()
	{
		TestCpu a, b;
		const auto ref = step(a, g_noLimit);
		check("callback fetches without a page", a.immediateCalls > 0, 1);

		m68k_set_immediate_page(b.getCpuState(), g_program, b.memory.data() + g_program);
		auto run = b.makeRun(g_noLimit, 0x60fe);
		m68k_execute_run(b.getCpuState(), &run);
		check("page: no callback fetches", b.immediateCalls, 0);
		check("page: elapsed", run.elapsed, ref.cycles);
		// The program patched an instruction inside the page before running it.
		check("page: write to the page is seen", b.getDReg(3), 2);
		check("page: same CPU and memory", sameCpu(a, b), 1);

		// Withdrawn: the callback serves the opcode stream again.
		m68k_set_immediate_page(b.getCpuState(), 0, nullptr);
		m68k_execute_one(b.getCpuState());
		check("withdrawn page: callback fetch", b.immediateCalls, 1);
		check("withdrawn page: still on the self-branch", b.getPC(), g_selfBranch);

		// A page elsewhere does not serve this code.
		TestCpu c;
		m68k_set_immediate_page(c.getCpuState(), 0, c.memory.data());
		const auto other = step(c, g_noLimit);
		check("other page: callback fetches", c.immediateCalls, a.immediateCalls);
		check("other page: cycles", other.cycles, ref.cycles);
	}
}

int main()
{
	wholeProgram();
	limit();
	stopFlag();
	codePage();
	return failures ? 1 : 0;
}
