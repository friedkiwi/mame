// license:BSD-3-Clause
// copyright-holders:Maurizio Petrarota, Vas Crabb
/***************************************************************************

    ui/auditmenu.h

    Internal UI user interface.

***************************************************************************/
#ifndef MAME_FRONTEND_UI_AUDITMENU_H
#define MAME_FRONTEND_UI_AUDITMENU_H

#pragma once

#include "ui/menu.h"
#include "ui/utils.h"

#include <atomic>
#include <thread>
#include <vector>


namespace ui {

class menu_audit : public menu
{
public:
	menu_audit(mame_ui_manager &mui, render_container &container, std::vector<ui_system_info> &availablesorted);
	virtual ~menu_audit() override;

protected:
	virtual void custom_render(void *selectedref, float top, float bottom, float x, float y, float x2, float y2) override;

private:
	enum class phase { CONSENT, AUDIT };

	virtual void populate(float &customtop, float &custombottom) override;
	virtual void handle() override;

	void audit_fast();
	void audit_all();
	void save_available_machines();

	std::thread m_worker_thread;
	std::size_t const m_unavailable;
	std::string m_prompt;
	std::vector<ui_system_info> &m_availablesorted;
	std::atomic<std::size_t> m_audited;
	std::atomic<game_driver const *> m_current;
	phase m_phase;
	bool m_fast;
};

} // namespace ui

#endif // MAME_FRONTEND_UI_AUDITMENU_H
