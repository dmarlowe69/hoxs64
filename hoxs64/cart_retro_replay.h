#pragma once

class Cart;

class CartRetroReplay : public CartCommon
{
public:
	 CartRetroReplay(Cart *pCart, IC6510 *pCpu, bit8 *pC64RamMemory);

	virtual void Reset(ICLK sysclock);
	virtual bit8 ReadRegister(bit16 address, ICLK sysclock);
	virtual void WriteRegister(bit16 address, ICLK sysclock, bit8 data);

	virtual void CartFreeze();
	virtual void CheckForCartFreeze();

protected:
	virtual void UpdateIO();
private:

	bool m_bDE01WriteDone;
};