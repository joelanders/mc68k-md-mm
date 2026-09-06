#include "hdi08.h"

#include <iostream>

int testReceiveOrder(const bool littleEndian)
{
	mc68k::Hdi08 hdi;
	hdi.icr(littleEndian ? mc68k::Hdi08::Hlend : 0);
	hdi.setRxEmptyCallback([&](bool) { hdi.relatchRx(); });
	bool queuedSecond = false;
	hdi.setReadIsrCallback([&](uint8_t status)
	{
		// The DSP-side status callback can deliver another word synchronously.
		if(!queuedSecond)
		{
			queuedSecond = true;
			hdi.writeRx(0xbb3344);
		}
		return status;
	});
	hdi.writeRx(0xaa1122);
	auto readWord = [&]()
	{
		if(littleEndian)
		{
			const auto high = hdi.read8(mc68k::PeriphAddress::HdiTXL);
			const auto middle = hdi.read8(mc68k::PeriphAddress::HdiTXM);
			const auto low = hdi.read8(mc68k::PeriphAddress::HdiTXH);
			return (static_cast<uint32_t>(high) << 16)
				| (static_cast<uint32_t>(middle) << 8) | low;
		}
		const auto high = hdi.read16(mc68k::PeriphAddress::HdiUnused4);
		const auto low = hdi.read16(mc68k::PeriphAddress::HdiTXM);
		return (static_cast<uint32_t>(high) << 16) | low;
	};
	const auto first = readWord();
	const auto second = readWord();
	if(first != 0xaa1122 || second != 0xbb3344)
	{
		std::cerr << "receive callback reordered/lost words: " << std::hex
			<< first << ", " << second << '\n';
		return 1;
	}
	if(hdi.hostRxWordsAvailable() != 0)
		return 1;
	return 0;
}

int main()
{
	if(testReceiveOrder(false) || testReceiveOrder(true))
		return 1;
	std::cout << "HI08 receive callback preserves FIFO order in both byte orders\n";
	return 0;
}
