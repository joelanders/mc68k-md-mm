#include "hdi08.h"

#include <iostream>
#include <stdexcept>

namespace
{
	void require(bool condition, const char* message)
	{
		if(!condition) throw std::runtime_error(message);
	}
}

int main()
{
	try
	{
		{
			mc68k::Hdi08 host;
			bool pending = false;
			unsigned deliveries = 0;
			unsigned cancellations = 0;
			host.setWriteIrqCallback([&](uint8_t vector)
			{
				require(vector == 0x20, "shared command vector changed");
				pending = true;
				++deliveries;
			});
			host.setHostCommandCallbacks([&] { return pending; }, [&]
			{
				pending = false;
				++cancellations;
			});
			host.write8(mc68k::PeriphAddress::HdiCVR, mc68k::Hdi08::Hc | 0x10);
			require(deliveries == 1 && host.read8(mc68k::PeriphAddress::HdiCVR)
				== (mc68k::Hdi08::Hc | 0x10), "callback return acknowledged a pending command");
			pending = false; // The DSP accepts the command independently of the host.
			require(host.read8(mc68k::PeriphAddress::HdiCVR) == 0x10,
				"acceptance did not clear HC or preserve HV");
			host.write8(mc68k::PeriphAddress::HdiCVR, mc68k::Hdi08::Hc | 0x10);
			host.write8(mc68k::PeriphAddress::HdiCVR, 0x11);
			require(deliveries == 2 && cancellations == 1 && !pending
				&& host.read8(mc68k::PeriphAddress::HdiCVR) == 0x11,
				"clearing HC did not cancel pending delivery or preserve the new vector");
		}
		{
			mc68k::Hdi08 host;
			host.setRxEmptyCallback([](bool) {});
			bool delivered = false;
			host.setReadIsrCallback([&](uint8_t sampled)
			{
				if(!delivered)
				{
					require(!(sampled & mc68k::Hdi08::Rxdf), "receive fixture was not initially empty");
					delivered = true; // writeRx may re-enter status observation.
					host.writeRx(0x123456);
					require(!host.canReceiveData(), "callback did not populate the receive latch");
				}
				return host.refreshReceiveStatus(sampled);
			});
			require(host.read8(mc68k::PeriphAddress::HdiISR) & mc68k::Hdi08::Rxdf,
				"status returned stale RXDF after callback latched data");
			require(host.read8(mc68k::PeriphAddress::HdiTXH) == 0x12
				&& host.read8(mc68k::PeriphAddress::HdiTXM) == 0x34
				&& host.read8(mc68k::PeriphAddress::HdiTXL) == 0x56,
				"receive-status refresh changed the latched word");
			const auto flags = mc68k::Hdi08::Rxdf | mc68k::Hdi08::Hf2 | mc68k::Hdi08::Txde;
			require(host.refreshReceiveStatus(flags) == (flags & ~mc68k::Hdi08::Rxdf),
				"receive-status refresh retained consumed RXDF or changed unrelated flags");
		}
		{
			mc68k::Hdi08 legacy;
			unsigned calls = 0;
			legacy.setWriteIrqCallback([&](uint8_t vector) { require(vector == 0x20, "legacy vector changed"); ++calls; });
			legacy.write8(mc68k::PeriphAddress::HdiCVR, mc68k::Hdi08::Hc | 0x10);
			require(calls == 1 && legacy.read8(mc68k::PeriphAddress::HdiCVR) == 0x10,
				"unconfigured host no longer acknowledges synchronously");
		}
		std::cout << "HI08 host lifecycle: PASS\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "HI08 host lifecycle: " << error.what() << '\n';
		return 1;
	}
}
