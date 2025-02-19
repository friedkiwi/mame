// license:BSD-3-Clause
// copyright-holders:R. Belmont
/***************************************************************************

    konmedal020.cpp: Konami 68EC020/VGA based medal games

    Mighty Joker?
    GS471
    (c) 1997 Konami

    Major ICs:
    MC68EC020-25 CPU
    YMZ280B sound
    16552 serial UART
    Oak OTI64111 "Spitfire" Super VGA video
    K056879 input/EEPROM interface

***************************************************************************/

#include "emu.h"
#include "cpu/m68000/m68000.h"
#include "machine/eepromser.h"
#include "machine/gen_latch.h"
#include "machine/nvram.h"
#include "machine/timer.h"
#include "sound/ymz280b.h"
#include "video/pc_vga.h"
#include "emupal.h"
#include "screen.h"
#include "speaker.h"

namespace {

class konmedal020_state : public driver_device
{
public:
	konmedal020_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_maincpu(*this, "maincpu"),
		m_vga(*this, "vga"),
		m_ymz(*this, "ymz")
	{ }

	void gs471(machine_config &config);

protected:
	virtual void machine_start() override;
	virtual void machine_reset() override;
	virtual void video_start() override;

	required_device<cpu_device> m_maincpu;
	required_device<vga_device> m_vga;
	required_device<ymz280b_device> m_ymz;

private:
	void gs471_main(address_map &map);
};

void konmedal020_state::video_start()
{
}

void konmedal020_state::gs471_main(address_map &map)
{
	map(0x000000, 0x1fffff).rom().region("maincpu", 0);
	map(0x200000, 0x23ffff).ram();
	// Watchdog and system control at 0x380000
	map(0x3e0000, 0x3e1fff).ram();  // NVRAM?
	map(0x800000, 0x8fffff).ram();  // VGA VRAM, probably
	map(0xf003b0, 0xf003bf).rw(m_vga, FUNC(vga_device::port_03b0_r), FUNC(vga_device::port_03b0_w));
	map(0xf003c0, 0xf003cf).rw(m_vga, FUNC(vga_device::port_03c0_r), FUNC(vga_device::port_03c0_w));
	map(0xf003d0, 0xf003df).rw(m_vga, FUNC(vga_device::port_03d0_r), FUNC(vga_device::port_03d0_w));
}

static INPUT_PORTS_START( gs471 )
INPUT_PORTS_END


void konmedal020_state::machine_start()
{
}

void konmedal020_state::machine_reset()
{
}

void konmedal020_state::gs471(machine_config &config)
{
	/* basic machine hardware */
	M68EC020(config, m_maincpu, XTAL(25'000'000));
	m_maincpu->set_addrmap(AS_PROGRAM, &konmedal020_state::gs471_main);
	//NVRAM(config, "nvram", nvram_device::DEFAULT_ALL_0);

	/* video hardware */
	screen_device &screen(SCREEN(config, "screen", SCREEN_TYPE_RASTER));
	screen.set_raw(25.175_MHz_XTAL, 800, 0, 640, 524, 0, 480);
	screen.set_screen_update(m_vga, FUNC(vga_device::screen_update));

	VGA(config, m_vga, 0).set_screen("screen");

	/* sound hardware */
	SPEAKER(config, "lspeaker").front_left();
	SPEAKER(config, "rspeaker").front_right();

	YMZ280B(config, m_ymz, XTAL(16'934'400)); // 16.9344 MHz xtal verified on PCB
	m_ymz->add_route(0, "lspeaker", 0.75);
	m_ymz->add_route(1, "rspeaker", 0.75);
}

ROM_START( gs471 )
	ROM_REGION( 0x200000, "maincpu", 0 ) /* main program */
	ROM_LOAD32_WORD_SWAP("471-b04.15t", 0x000000, 0x080000, CRC(78f071b1) SHA1(4dac30917ea903e0fe803a988351992a30de668a))
	ROM_LOAD32_WORD_SWAP("471-b05.17t-2", 0x000002, 0x080000, CRC(45b1febf) SHA1(b504153631d11f7a9ebfb47fea6b09ce10b95654))
	ROM_LOAD32_WORD_SWAP("471-b06.20t", 0x100000, 0x080000, CRC(7bc5f090) SHA1(f25095883c1b747fd7971c8841000ef33878081d))
	ROM_LOAD32_WORD_SWAP("471-b07.22t", 0x100002, 0x080000, CRC(7d9153b5) SHA1(cebfbd1531e479b27ae8f176a47d928be9cfab88))

	ROM_REGION( 0x100000, "ymz", 0 )
	ROM_LOAD("471-b01.15l", 0x000000, 0x080000, CRC(064c4830) SHA1(a4051a16d7bed7a5aab0dafc570b9bc0ddb0fac5))
	ROM_LOAD("471-b02.18l", 0x080000, 0x080000, CRC(4a3f6c74) SHA1(d631d988a3334de0a4d13bd2b1bfa2133da7507e))
ROM_END

} // Anonymous namespace

GAME( 1997, gs471,  0, gs471,  gs471, konmedal020_state, empty_init, ROT0, "Konami", "unknown medal game GS471", MACHINE_NOT_WORKING )
