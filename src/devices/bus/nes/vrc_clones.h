// license:BSD-3-Clause
// copyright-holders: kmg, Fabio Priuli
#ifndef MAME_BUS_NES_VRC_CLONES_H
#define MAME_BUS_NES_VRC_CLONES_H

#pragma once

#include "konami.h"


// ======================> nes_shuiguan_device

class nes_shuiguan_device : public nes_konami_vrc4_device
{
public:
	// construction/destruction
	nes_shuiguan_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);

	virtual u8 read_m(offs_t offset) override;
	virtual void write_m(offs_t offset, u8 data) override;
	virtual void write_h(offs_t offset, u8 data) override;

	virtual void pcb_reset() override;

protected:
	// device-level overrides
	virtual void device_start() override;

private:
	u8 m_reg;
};


// ======================> nes_tf1201_device

class nes_tf1201_device : public nes_konami_vrc4_device
{
public:
	// construction/destruction
	nes_tf1201_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);

	virtual void write_h(offs_t offset, u8 data) override;

protected:
	// device-level overrides
	virtual void device_start() override;
};


// device type definition
DECLARE_DEVICE_TYPE(NES_SHUIGUAN, nes_shuiguan_device)
DECLARE_DEVICE_TYPE(NES_TF1201,   nes_tf1201_device)

#endif // MAME_BUS_NES_VRC_CLONES_H
