// license:BSD-3-Clause
// copyright-holders:Maurizio Petrarota, Vas Crabb
/*********************************************************************

    ui/selgame.cpp

    Main UI menu.

*********************************************************************/

#include "emu.h"
#include "ui/selgame.h"

#include "ui/auditmenu.h"
#include "ui/icorender.h"
#include "ui/inifile.h"
#include "ui/miscmenu.h"
#include "ui/optsmenu.h"
#include "ui/selector.h"
#include "ui/selsoft.h"
#include "ui/ui.h"

#include "infoxml.h"
#include "luaengine.h"
#include "mame.h"

#include "corestr.h"
#include "drivenum.h"
#include "emuopts.h"
#include "rendutil.h"
#include "romload.h"
#include "softlist_dev.h"
#include "uiinput.h"
#include "unicode.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <locale>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>


extern const char UI_VERSION_TAG[];

namespace ui {

namespace {

constexpr uint32_t FLAGS_UI = ui::menu::FLAG_LEFT_ARROW | ui::menu::FLAG_RIGHT_ARROW;

} // anonymous namespace

class menu_select_game::persistent_data
{
public:
	enum available : unsigned
	{
		AVAIL_NONE                  = 0U,
		AVAIL_SORTED_LIST           = 1U << 0,
		AVAIL_BIOS_COUNT            = 1U << 1,
		AVAIL_UCS_SHORTNAME         = 1U << 2,
		AVAIL_UCS_DESCRIPTION       = 1U << 3,
		AVAIL_UCS_MANUF_DESC        = 1U << 4,
		AVAIL_UCS_DFLT_DESC         = 1U << 5,
		AVAIL_UCS_MANUF_DFLT_DESC   = 1U << 6,
		AVAIL_FILTER_DATA           = 1U << 7
	};

	~persistent_data()
	{
		if (m_thread)
			m_thread->join();
	}

	void cache_data(ui_options const &options)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		if (!m_started)
		{
			m_started = true;
			m_thread = std::make_unique<std::thread>(
					[this, datpath = std::string(options.history_path()), titles = std::string(options.system_names())]
					{
						do_cache_data(datpath, titles);
					});
		}
	}

	void reset_cache()
	{
		if (m_thread)
			m_thread->join();
		std::unique_lock<std::mutex> lock(m_mutex);
		m_thread.reset();
		m_started = false;
		m_available = AVAIL_NONE;
		m_sorted_list.clear();
		m_filter_data = machine_filter_data();
		m_bios_count = 0U;
	}

	bool is_available(available desired)
	{
		return (m_available.load(std::memory_order_acquire) & desired) == desired;
	}

	void wait_available(available desired)
	{
		if (!is_available(desired))
		{
			assert(m_started);
			std::unique_lock<std::mutex> lock(m_mutex);
			m_condition.wait(lock, [this, desired] () { return is_available(desired); });
		}
	}

	std::vector<ui_system_info> &sorted_list()
	{
		wait_available(AVAIL_SORTED_LIST);
		return m_sorted_list;
	}

	int bios_count()
	{
		wait_available(AVAIL_BIOS_COUNT);
		return m_bios_count;
	}

	bool unavailable_systems()
	{
		wait_available(AVAIL_SORTED_LIST);
		return std::find_if(m_sorted_list.begin(), m_sorted_list.end(), [] (ui_system_info const &info) { return !info.available; }) != m_sorted_list.end();
	}

	machine_filter_data &filter_data()
	{
		wait_available(AVAIL_FILTER_DATA);
		return m_filter_data;
	}

	static persistent_data &instance()
	{
		static persistent_data data;
		return data;
	}

private:
	persistent_data()
		: m_started(false)
		, m_available(AVAIL_NONE)
		, m_bios_count(0)
	{
	}

	void notify_available(available value)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_available.fetch_or(value, std::memory_order_release);
		m_condition.notify_all();
	}

	void do_cache_data(std::string const &datpath, std::string const &titles)
	{
		// try to open the titles file for optimisation reasons
		emu_file titles_file(datpath, OPEN_FLAG_READ);
		bool const try_titles(!titles.empty() && !titles_file.open(titles));

		// generate full list - initially ordered by shortname
		populate_list(!try_titles);

		// notify that BIOS count is valie
		notify_available(AVAIL_BIOS_COUNT);

		// try to load localised descriptions
		if (try_titles)
		{
			load_titles(titles_file);

			// populate parent descriptions while still ordered by shortname
			// already done on the first pass if built-in titles are used
			populate_parents();
		}

		// get rid of the "empty" driver - we don't need positions to line up any more
		m_sorted_list.erase(m_sorted_list.begin() + driver_list::find(GAME_NAME(___empty)));

		// sort drivers and notify
		std::collate<wchar_t> const &coll = std::use_facet<std::collate<wchar_t> >(std::locale());
		auto const compare_names =
				[&coll] (std::wstring const &wx, std::wstring const &wy) -> bool
				{
					return 0 > coll.compare(wx.data(), wx.data() + wx.size(), wy.data(), wy.data() + wy.size());
				};
		std::stable_sort(
				m_sorted_list.begin(),
				m_sorted_list.end(),
				[&compare_names] (ui_system_info const &lhs, ui_system_info const &rhs)
				{
					game_driver const &x(*lhs.driver);
					game_driver const &y(*rhs.driver);

					if (!lhs.is_clone && !rhs.is_clone)
					{
						return compare_names(
								lhs.reading_description.empty() ? wstring_from_utf8(lhs.description) : lhs.reading_description,
								rhs.reading_description.empty() ? wstring_from_utf8(rhs.description) : rhs.reading_description);
					}
					else if (lhs.is_clone && rhs.is_clone)
					{
						if (!std::strcmp(x.parent, y.parent))
						{
							return compare_names(
									lhs.reading_description.empty() ? wstring_from_utf8(lhs.description) : lhs.reading_description,
									rhs.reading_description.empty() ? wstring_from_utf8(rhs.description) : rhs.reading_description);
						}
						else
						{
							return compare_names(
									lhs.reading_parent.empty() ? wstring_from_utf8(lhs.parent) : lhs.reading_parent,
									rhs.reading_parent.empty() ? wstring_from_utf8(rhs.parent) : rhs.reading_parent);
						}
					}
					else if (!lhs.is_clone && rhs.is_clone)
					{
						if (!std::strcmp(x.name, y.parent))
						{
							return true;
						}
						else
						{
							return compare_names(
									lhs.reading_description.empty() ? wstring_from_utf8(lhs.description) : lhs.reading_description,
									rhs.reading_parent.empty() ? wstring_from_utf8(rhs.parent) : rhs.reading_parent);
						}
					}
					else
					{
						if (!std::strcmp(x.parent, y.name))
						{
							return false;
						}
						else
						{
							return compare_names(
									lhs.reading_parent.empty() ? wstring_from_utf8(lhs.parent) : lhs.reading_parent,
									rhs.reading_description.empty() ? wstring_from_utf8(rhs.description) : rhs.reading_description);
						}
					}
				});
		notify_available(AVAIL_SORTED_LIST);

		// sort manufacturers and years
		m_filter_data.finalise();
		notify_available(AVAIL_FILTER_DATA);

		// convert shortnames to UCS-4
		for (ui_system_info &info : m_sorted_list)
			info.ucs_shortname = ustr_from_utf8(normalize_unicode(info.driver->name, unicode_normalization_form::D, true));
		notify_available(AVAIL_UCS_SHORTNAME);

		// convert descriptions to UCS-4
		for (ui_system_info &info : m_sorted_list)
			info.ucs_description = ustr_from_utf8(normalize_unicode(info.description, unicode_normalization_form::D, true));
		notify_available(AVAIL_UCS_DESCRIPTION);

		// convert "<manufacturer> <description>" to UCS-4
		std::string buf;
		for (ui_system_info &info : m_sorted_list)
		{
			buf.assign(info.driver->manufacturer);
			buf.append(1, ' ');
			buf.append(info.description);
			info.ucs_manufacturer_description = ustr_from_utf8(normalize_unicode(buf, unicode_normalization_form::D, true));
		}
		notify_available(AVAIL_UCS_MANUF_DESC);

		// convert default descriptions to UCS-4
		if (try_titles)
		{
			for (ui_system_info &info : m_sorted_list)
			{
				std::string_view const fullname(info.driver->type.fullname());
				if (info.description != fullname)
					info.ucs_default_description = ustr_from_utf8(normalize_unicode(fullname, unicode_normalization_form::D, true));
			}
		}
		notify_available(AVAIL_UCS_DFLT_DESC);

		// convert "<manufacturer> <default description>" to UCS-4
		if (try_titles)
		{
			for (ui_system_info &info : m_sorted_list)
			{
				std::string_view const fullname(info.driver->type.fullname());
				if (info.description != fullname)
				{
					buf.assign(info.driver->manufacturer);
					buf.append(1, ' ');
					buf.append(fullname);
					info.ucs_manufacturer_default_description = ustr_from_utf8(normalize_unicode(buf, unicode_normalization_form::D, true));
				}
			}
		}
		notify_available(AVAIL_UCS_MANUF_DFLT_DESC);
	}

	void populate_list(bool copydesc)
	{
		m_sorted_list.reserve(driver_list::total());
		std::unordered_set<std::string> manufacturers, years;
		for (int x = 0; x < driver_list::total(); ++x)
		{
			game_driver const &driver(driver_list::driver(x));
			ui_system_info &ins(m_sorted_list.emplace_back(driver, x, false));
			if (&driver != &GAME_NAME(___empty))
			{
				if (driver.flags & machine_flags::IS_BIOS_ROOT)
					++m_bios_count;

				if ((driver.parent[0] != '0') || driver.parent[1])
				{
					auto const parentindex(driver_list::find(driver.parent));
					if (copydesc)
					{
						if (0 <= parentindex)
						{
							game_driver const &parentdriver(driver_list::driver(parentindex));
							ins.is_clone = !(parentdriver.flags & machine_flags::IS_BIOS_ROOT);
							ins.parent = parentdriver.type.fullname();
						}
						else
						{
							ins.is_clone = false;
							ins.parent = driver.parent;
						}
					}
					else
					{
						ins.is_clone = (0 <= parentindex) && !(driver_list::driver(parentindex).flags & machine_flags::IS_BIOS_ROOT);
					}
				}

				if (copydesc)
					ins.description = driver.type.fullname();

				m_filter_data.add_manufacturer(driver.manufacturer);
				m_filter_data.add_year(driver.year);
			}
		}
	}

	void load_titles(util::core_file &file)
	{
		char readbuf[1024];
		std::string convbuf;
		while (file.gets(readbuf, std::size(readbuf)))
		{
			// shortname, description, and description reading separated by tab
			auto const eoln(
					std::find_if(
						std::begin(readbuf),
						std::end(readbuf),
						[] (char ch) { return !ch || ('\n' == ch) || ('\r' == ch); }));
			auto const split(std::find(std::begin(readbuf), eoln, '\t'));
			if (eoln == split)
				continue;
			std::string_view const shortname(readbuf, split - readbuf);

			// find matching system - still sorted by shortname at this point
			auto const found(
					std::lower_bound(
						m_sorted_list.begin(),
						m_sorted_list.end(),
						shortname,
						[] (ui_system_info const &a, std::string_view const &b)
						{
							return a.driver->name < b;
						}));
			if ((m_sorted_list.end() == found) || (shortname != found->driver->name))
			{
				//osd_printf_verbose("System '%s' not found\n", shortname); very spammy for single-driver builds
				continue;
			}

			// find the end of the description
			auto const descstart(std::next(split));
			auto const descend(std::find(descstart, eoln, '\t'));
			auto const description(strtrimspace(std::string_view(descstart, descend - descstart)));
			if (description.empty())
			{
				osd_printf_warning("Empty translated description for system '%s'\n", shortname);
			}
			else if (!found->description.empty())
			{
				osd_printf_warning(
						"Multiple translated descriptions for system '%s' ('%s' and '%s')\n",
						shortname,
						found->description,
						description);
			}
			else
			{
				found->description = description;
			}

			// populate the reading if it's present
			if (eoln == descend)
				continue;
			auto const readstart(std::next(descend));
			auto const readend(std::find(readstart, eoln, '\t'));
			auto const reading(strtrimspace(std::string_view(readstart, readend - readstart)));
			if (reading.empty())
			{
				osd_printf_warning("Empty translated description reading for system '%s'\n", shortname);
			}
			else
			{
				found->reading_description = wstring_from_utf8(reading);
				found->ucs_reading_description = ustr_from_utf8(normalize_unicode(reading, unicode_normalization_form::D, true));
				convbuf.assign(found->driver->manufacturer);
				convbuf.append(1, ' ');
				convbuf.append(reading);
				found->ucs_manufacturer_reading_description = ustr_from_utf8(normalize_unicode(convbuf, unicode_normalization_form::D, true));
			}
		}

		// fill in untranslated descriptions
		for (ui_system_info &info : m_sorted_list)
		{
			if (info.description.empty())
				info.description = info.driver->type.fullname();
		}
	}

	void populate_parents()
	{
		for (ui_system_info &info : m_sorted_list)
		{
			if ((info.driver->parent[0] != '0') || info.driver->parent[1])
			{
				auto const found(
						std::lower_bound(
							m_sorted_list.begin(),
							m_sorted_list.end(),
							std::string_view(info.driver->parent),
							[] (ui_system_info const &a, std::string_view const &b)
							{
								return a.driver->name < b;
							}));
				if (m_sorted_list.end() != found)
				{
					info.parent = found->description;
					info.reading_parent = found->reading_description;
				}
				else
				{
					info.parent = info.driver->parent;
				}
			}
		}
	}

	// synchronisation
	std::mutex                      m_mutex;
	std::condition_variable         m_condition;
	std::unique_ptr<std::thread>    m_thread;
	std::atomic<bool>               m_started;
	std::atomic<unsigned>           m_available;

	// data
	std::vector<ui_system_info>     m_sorted_list;
	machine_filter_data             m_filter_data;
	int                             m_bios_count;
};

bool menu_select_game::s_first_start = true;


//-------------------------------------------------
//  ctor
//-------------------------------------------------

menu_select_game::menu_select_game(mame_ui_manager &mui, render_container &container, const char *gamename)
	: menu_select_launch(mui, container, false)
	, m_persistent_data(persistent_data::instance())
	, m_icons(MAX_ICONS_RENDER)
	, m_icon_paths()
	, m_displaylist()
	, m_searchlist()
	, m_searched_fields(persistent_data::AVAIL_NONE)
	, m_populated_favorites(false)
{
	std::string error_string, last_filter, sub_filter;
	ui_options &moptions = mui.options();

	// load drivers cache
	m_persistent_data.cache_data(mui.options());

	// check if there are available system icons
	check_for_icons(nullptr);

	// build drivers list
	if (!load_available_machines())
		build_available_list();

	if (s_first_start)
	{
		//s_first_start = false; TODO: why wasn't it ever clearing the first start flag?
		reselect_last::set_driver(moptions.last_used_machine());
		ui_globals::rpanel = std::min<int>(std::max<int>(moptions.last_right_panel(), RP_FIRST), RP_LAST);

		std::string tmp(moptions.last_used_filter());
		std::size_t const found = tmp.find_first_of(',');
		std::string fake_ini;
		if (found == std::string::npos)
		{
			fake_ini = util::string_format(u8"\uFEFF%s = 1\n", tmp);
		}
		else
		{
			std::string const sub_filter(tmp.substr(found + 1));
			tmp.resize(found);
			fake_ini = util::string_format(u8"\uFEFF%s = %s\n", tmp, sub_filter);
		}

		emu_file file(ui().options().ui_path(), OPEN_FLAG_READ);
		if (!file.open_ram(fake_ini.c_str(), fake_ini.size()))
		{
			m_persistent_data.filter_data().load_ini(file);
			file.close();
		}
	}

	// do this after processing the last used filter setting so it overwrites the placeholder
	load_custom_filters();
	m_filter_highlight = m_persistent_data.filter_data().get_current_filter_type();

	if (!moptions.remember_last())
		reselect_last::reset();

	mui.machine().options().set_value(OPTION_SNAPNAME, "%g/%i", OPTION_PRIORITY_CMDLINE);

	ui_globals::curdats_view = 0;
	ui_globals::panels_status = moptions.hide_panels();
	ui_globals::curdats_total = 1;
}

//-------------------------------------------------
//  dtor
//-------------------------------------------------

menu_select_game::~menu_select_game()
{
	std::string error_string, last_driver;
	ui_system_info const *system;
	ui_software_info const *swinfo;
	get_selection(swinfo, system);
	if (swinfo)
		last_driver = swinfo->shortname;
	else if (system)
		last_driver = system->driver->name;

	std::string const filter(m_persistent_data.filter_data().get_config_string());

	ui_options &mopt = ui().options();
	mopt.set_value(OPTION_LAST_RIGHT_PANEL, ui_globals::rpanel, OPTION_PRIORITY_CMDLINE);
	mopt.set_value(OPTION_LAST_USED_FILTER, filter, OPTION_PRIORITY_CMDLINE);
	mopt.set_value(OPTION_LAST_USED_MACHINE, last_driver, OPTION_PRIORITY_CMDLINE);
	mopt.set_value(OPTION_HIDE_PANELS, ui_globals::panels_status, OPTION_PRIORITY_CMDLINE);
	ui().save_ui_options();
}

//-------------------------------------------------
//  handle
//-------------------------------------------------

void menu_select_game::handle()
{
	if (!m_prev_selected)
		m_prev_selected = item(0).ref;

	// if I have to load datfile, perform a hard reset
	if (ui_globals::reset)
	{
		// dumb workaround for not being able to add an exit notifier
		struct cache_reset { ~cache_reset() { persistent_data::instance().reset_cache(); } };
		ui().get_session_data<cache_reset, cache_reset>();

		ui_globals::reset = false;
		machine().schedule_hard_reset();
		stack_reset();
		return;
	}

	// if I have to select software, force software list submenu
	if (reselect_last::get())
	{
		const ui_system_info *system;
		const ui_software_info *software;
		get_selection(software, system);
		menu::stack_push<menu_select_software>(ui(), container(), *system);
		return;
	}

	// ignore pause keys by swallowing them before we process the menu
	machine().ui_input().pressed(IPT_UI_PAUSE);

	// process the menu
	const event *menu_event = process(PROCESS_LR_REPEAT);
	if (menu_event)
	{
		if (dismiss_error())
		{
			// reset the error on any future menu_event
		}
		else switch (menu_event->iptkey)
		{
		case IPT_UI_UP:
			if ((get_focus() == focused_menu::LEFT) && (machine_filter::FIRST < m_filter_highlight))
				--m_filter_highlight;
			break;

		case IPT_UI_DOWN:
			if ((get_focus() == focused_menu::LEFT) && (machine_filter::LAST > m_filter_highlight))
				m_filter_highlight++;
			break;

		case IPT_UI_HOME:
			if (get_focus() == focused_menu::LEFT)
				m_filter_highlight = machine_filter::FIRST;
			break;

		case IPT_UI_END:
			if (get_focus() == focused_menu::LEFT)
				m_filter_highlight = machine_filter::LAST;
			break;

		case IPT_UI_EXPORT:
			inkey_export();
			break;

		case IPT_UI_DATS:
			inkey_dats();
			break;

		default:
			if (menu_event->itemref)
			{
				switch (menu_event->iptkey)
				{
				case IPT_UI_SELECT:
					if (get_focus() == focused_menu::MAIN)
					{
						if (m_populated_favorites)
							inkey_select_favorite(menu_event);
						else
							inkey_select(menu_event);
					}
					break;

				case IPT_CUSTOM:
					// handle IPT_CUSTOM (mouse right click)
					if (!m_populated_favorites)
					{
						menu::stack_push<menu_machine_configure>(
								ui(), container(),
								*reinterpret_cast<ui_system_info const *>(m_prev_selected),
								nullptr,
								menu_event->mouse.x0, menu_event->mouse.y0);
					}
					else
					{
						ui_software_info *sw = reinterpret_cast<ui_software_info *>(m_prev_selected);
						menu::stack_push<menu_machine_configure>(
								ui(), container(),
								*sw->driver,
								[this, empty = sw->startempty] (bool fav, bool changed)
								{
									if (changed)
										reset(empty ? reset_options::SELECT_FIRST : reset_options::REMEMBER_REF);
								},
								menu_event->mouse.x0, menu_event->mouse.y0);
					}
					break;

				case IPT_UI_LEFT:
					if (ui_globals::rpanel == RP_IMAGES)
					{
						// Images
						previous_image_view();
					}
					else if (ui_globals::rpanel == RP_INFOS)
					{
						// Infos
						change_info_pane(-1);
					}
					break;

				case IPT_UI_RIGHT:
					if (ui_globals::rpanel == RP_IMAGES)
					{
						// Images
						next_image_view();
					}
					else if (ui_globals::rpanel == RP_INFOS)
					{
						// Infos
						change_info_pane(1);
					}
					break;

				case IPT_UI_FAVORITES:
					if (uintptr_t(menu_event->itemref) > skip_main_items)
					{
						favorite_manager &mfav(mame_machine_manager::instance()->favorite());
						if (!m_populated_favorites)
						{
							auto const &info(*reinterpret_cast<ui_system_info const *>(menu_event->itemref));
							auto const &driver(*info.driver);
							if (!mfav.is_favorite_system(driver))
							{
								mfav.add_favorite_system(driver);
								machine().popmessage(_("%s\n added to favorites list."), info.description);
							}
							else
							{
								mfav.remove_favorite_system(driver);
								machine().popmessage(_("%s\n removed from favorites list."), info.description);
							}
						}
						else
						{
							ui_software_info const *const swinfo(reinterpret_cast<ui_software_info const *>(menu_event->itemref));
							machine().popmessage(_("%s\n removed from favorites list."), swinfo->longname);
							mfav.remove_favorite_software(*swinfo);
							reset(reset_options::SELECT_FIRST);
						}
					}
					break;

				case IPT_UI_AUDIT:
					menu::stack_push<menu_audit>(ui(), container(), m_persistent_data.sorted_list());
					break;
				}
			}
		}
	}

	// if we're in an error state, overlay an error message
	draw_error_text();
}

//-------------------------------------------------
//  populate
//-------------------------------------------------

void menu_select_game::populate(float &customtop, float &custombottom)
{
	for (auto &icon : m_icons) // TODO: why is this here?  maybe better on resize or setting change?
		icon.second.texture.reset();

	set_switch_image();
	int old_item_selected = -1;

	if (!isfavorite())
	{
		m_populated_favorites = false;
		m_displaylist.clear();
		machine_filter const *const flt(m_persistent_data.filter_data().get_current_filter());

		// if search is not empty, find approximate matches
		if (!m_search.empty())
		{
			populate_search();
			if (flt)
			{
				for (auto it = m_searchlist.begin(); (m_searchlist.end() != it) && (MAX_VISIBLE_SEARCH > m_displaylist.size()); ++it)
				{
					if (flt->apply(it->second))
						m_displaylist.emplace_back(it->second);
				}
			}
			else
			{
				std::transform(
						m_searchlist.begin(),
						std::next(m_searchlist.begin(), (std::min)(m_searchlist.size(), MAX_VISIBLE_SEARCH)),
						std::back_inserter(m_displaylist),
						[] (auto const &entry) { return entry.second; });
			}
		}
		else
		{
			// if filter is set on category, build category list
			std::vector<ui_system_info> const &sorted(m_persistent_data.sorted_list());
			if (!flt)
				std::copy(sorted.begin(), sorted.end(), std::back_inserter(m_displaylist));
			else
				flt->apply(sorted.begin(), sorted.end(), std::back_inserter(m_displaylist));
		}

		// iterate over entries
		int curitem = 0;
		for (ui_system_info const &elem : m_displaylist)
		{
			if (old_item_selected == -1 && elem.driver->name == reselect_last::driver())
				old_item_selected = curitem;

			item_append(elem.description, elem.is_clone ? (FLAGS_UI | FLAG_INVERT) : FLAGS_UI, (void *)&elem);
			curitem++;
		}
	}
	else
	{
		// populate favorites list
		m_populated_favorites = true;
		m_search.clear();
		mame_machine_manager::instance()->favorite().apply_sorted(
				[this, &old_item_selected, curitem = 0] (ui_software_info const &info) mutable
				{
					if (info.startempty)
					{
						if (old_item_selected == -1 && info.shortname == reselect_last::driver())
							old_item_selected = curitem;

						bool cloneof = strcmp(info.driver->parent, "0");
						if (cloneof)
						{
							int cx = driver_list::find(info.driver->parent);
							if (cx != -1 && ((driver_list::driver(cx).flags & machine_flags::IS_BIOS_ROOT) != 0))
								cloneof = false;
						}

						item_append(info.longname, cloneof ? (FLAGS_UI | FLAG_INVERT) : FLAGS_UI, (void *)&info);
					}
					else
					{
						if (old_item_selected == -1 && info.shortname == reselect_last::driver())
							old_item_selected = curitem;
						item_append(info.longname, info.devicetype,
									info.parentname.empty() ? FLAGS_UI : (FLAG_INVERT | FLAGS_UI), (void *)&info);
					}
					curitem++;
				});
	}

	// add special items
	if (stack_has_special_main_menu())
	{
		item_append(menu_item_type::SEPARATOR, FLAGS_UI);
		item_append(_("Configure Options"), FLAGS_UI, (void *)(uintptr_t)CONF_OPTS);
		item_append(_("Configure Machine"), FLAGS_UI, (void *)(uintptr_t)CONF_MACHINE);
		skip_main_items = 3;
	}
	else
		skip_main_items = 0;

	// configure the custom rendering
	customtop = 3.0f * ui().get_line_height() + 5.0f * ui().box_tb_border();
	custombottom = 4.0f * ui().get_line_height() + 3.0f * ui().box_tb_border();

	// reselect prior game launched, if any
	if (old_item_selected != -1)
	{
		set_selected_index(old_item_selected);
		if (ui_globals::visible_main_lines == 0)
			top_line = (selected_index() != 0) ? selected_index() - 1 : 0;
		else
			top_line = selected_index() - (ui_globals::visible_main_lines / 2);

		if (reselect_last::software().empty())
			reselect_last::reset();
	}
	else
	{
		reselect_last::reset();
	}
}

//-------------------------------------------------
//  build a list of available drivers
//-------------------------------------------------

void menu_select_game::build_available_list()
{
	std::size_t const total = driver_list::total();
	std::vector<bool> included(total, false);

	// iterate over ROM directories and look for potential ROMs
	file_enumerator path(machine().options().media_path());
	for (osd::directory::entry const *dir = path.next(); dir; dir = path.next())
	{
		char drivername[50];
		char *dst = drivername;
		char const *src;

		// build a name for it
		for (src = dir->name; *src != 0 && *src != '.' && dst < &drivername[std::size(drivername) - 1]; ++src)
			*dst++ = tolower(uint8_t(*src));

		*dst = 0;
		int const drivnum = driver_list::find(drivername);
		if (0 <= drivnum)
			included[drivnum] = true;
	}

	// now check and include NONE_NEEDED
	if (!ui().options().hide_romless())
	{
		// FIXME: can't use the convenience macros tiny ROM entries
		auto const is_required_rom =
				[] (tiny_rom_entry const &rom) { return ROMENTRY_ISFILE(rom) && !ROM_ISOPTIONAL(rom) && !std::strchr(rom.hashdata, '!'); };
		for (std::size_t x = 0; total > x; ++x)
		{
			game_driver const &driver(driver_list::driver(x));
			if (!included[x] && (&GAME_NAME(___empty) != &driver))
			{
				bool noroms(true);
				tiny_rom_entry const *rom;
				for (rom = driver.rom; !ROMENTRY_ISEND(rom); ++rom)
				{
					// check optional and NO_DUMP
					if (is_required_rom(*rom))
					{
						noroms = false;
						break; // break before incrementing, or it will subtly break the check for all ROMs belonging to parent
					}
				}

				if (!noroms)
				{
					// check if clone == parent
					auto const cx(driver_list::clone(driver));
					if ((0 <= cx) && included[cx])
					{
						game_driver const &parent(driver_list::driver(cx));
						if (driver.rom == parent.rom)
						{
							noroms = true;
						}
						else
						{
							// check if clone < parent
							noroms = true;
							for ( ; noroms && !ROMENTRY_ISEND(rom); ++rom)
							{
								if (is_required_rom(*rom))
								{
									util::hash_collection const hashes(rom->hashdata);

									bool found(false);
									for (tiny_rom_entry const *parentrom = parent.rom; !found && !ROMENTRY_ISEND(parentrom); ++parentrom)
									{
										if (is_required_rom(*parentrom) && (rom->length == parentrom->length))
										{
											util::hash_collection const parenthashes(parentrom->hashdata);
											if (hashes == parenthashes)
												found = true;
										}
									}
									noroms = found;
								}
							}
						}
					}
				}

				if (noroms)
					included[x] = true;
			}
		}
	}

	// copy into the persistent sorted list
	for (ui_system_info &info : m_persistent_data.sorted_list())
		info.available = included[info.index];
}


//-------------------------------------------------
//  force the game select menu to be visible
//  and inescapable
//-------------------------------------------------

void menu_select_game::force_game_select(mame_ui_manager &mui, render_container &container)
{
	// reset the menu stack
	menu::stack_reset(mui.machine());

	// add the quit entry followed by the game select entry
	menu::stack_push_special_main<menu_quit_game>(mui, container);
	menu::stack_push<menu_select_game>(mui, container, nullptr);

	// force the menus on
	mui.show_menu();

	// make sure MAME is paused
	mui.machine().pause();
}

//-------------------------------------------------
//  handle select key event
//-------------------------------------------------

void menu_select_game::inkey_select(const event *menu_event)
{
	auto const system = reinterpret_cast<ui_system_info const *>(menu_event->itemref);

	if (uintptr_t(system) == CONF_OPTS)
	{
		// special case for configure options
		menu::stack_push<menu_game_options>(
				ui(),
				container(),
				m_persistent_data.filter_data(),
				[this] () { reset(reset_options::SELECT_FIRST); });
	}
	else if (uintptr_t(system) == CONF_MACHINE)
	{
		// special case for configure machine
		if (m_prev_selected)
			menu::stack_push<menu_machine_configure>(ui(), container(), *reinterpret_cast<const ui_system_info *>(m_prev_selected));
		return;
	}
	else
	{
		// anything else is a driver
		driver_enumerator enumerator(machine().options(), *system->driver);
		enumerator.next();

		// if there are software entries, show a software selection menu
		for (software_list_device &swlistdev : software_list_device_enumerator(enumerator.config()->root_device()))
		{
			if (!swlistdev.get_info().empty())
			{
				menu::stack_push<menu_select_software>(ui(), container(), *system);
				return;
			}
		}

		// audit the system ROMs first to see if we're going to work
		media_auditor auditor(enumerator);
		media_auditor::summary const summary = auditor.audit_media(AUDIT_VALIDATE_FAST);

		// if everything looks good, schedule the new driver
		if (audit_passed(summary))
		{
			if (!select_bios(*system->driver, false))
				launch_system(*system->driver);
		}
		else
		{
			// otherwise, display an error
			set_error(reset_options::REMEMBER_REF, make_system_audit_fail_text(auditor, summary));
		}
	}
}

//-------------------------------------------------
//  handle select key event for favorites menu
//-------------------------------------------------

void menu_select_game::inkey_select_favorite(const event *menu_event)
{
	ui_software_info *ui_swinfo = (ui_software_info *)menu_event->itemref;

	if ((uintptr_t)ui_swinfo == CONF_OPTS)
	{
		// special case for configure options
		menu::stack_push<menu_game_options>(
				ui(),
				container(),
				m_persistent_data.filter_data(),
				[this] () { reset(reset_options::SELECT_FIRST); });
	}
	else if ((uintptr_t)ui_swinfo == CONF_MACHINE)
	{
		// special case for configure machine
		if (m_prev_selected)
		{
			ui_software_info *swinfo = reinterpret_cast<ui_software_info *>(m_prev_selected);
			menu::stack_push<menu_machine_configure>(
					ui(),
					container(),
					*swinfo->driver,
					[this, empty = swinfo->startempty] (bool fav, bool changed)
					{
						if (changed)
							reset(empty ? reset_options::SELECT_FIRST : reset_options::REMEMBER_REF);
					});
		}
		return;
	}
	else if (ui_swinfo->startempty == 1)
	{
		driver_enumerator enumerator(machine().options(), *ui_swinfo->driver);
		enumerator.next();

		// if there are software entries, show a software selection menu
		for (software_list_device &swlistdev : software_list_device_enumerator(enumerator.config()->root_device()))
		{
			if (!swlistdev.get_info().empty())
			{
				menu::stack_push<menu_select_software>(ui(), container(), *ui_swinfo->driver);
				return;
			}
		}

		// audit the system ROMs first to see if we're going to work
		media_auditor auditor(enumerator);
		media_auditor::summary const summary = auditor.audit_media(AUDIT_VALIDATE_FAST);

		if (audit_passed(summary))
		{
			// if everything looks good, schedule the new driver
			if (!select_bios(*ui_swinfo->driver, false))
			{
				reselect_last::reselect(true);
				launch_system(*ui_swinfo->driver);
			}
		}
		else
		{
			// otherwise, display an error
			set_error(reset_options::REMEMBER_REF, make_system_audit_fail_text(auditor, summary));
		}
	}
	else
	{
		// first audit the system ROMs
		driver_enumerator drv(machine().options(), *ui_swinfo->driver);
		media_auditor auditor(drv);
		drv.next();
		media_auditor::summary const sysaudit = auditor.audit_media(AUDIT_VALIDATE_FAST);
		if (!audit_passed(sysaudit))
		{
			set_error(reset_options::REMEMBER_REF, make_system_audit_fail_text(auditor, sysaudit));
		}
		else
		{
			// now audit the software
			software_list_device *swlist = software_list_device::find_by_name(*drv.config(), ui_swinfo->listname);
			const software_info *swinfo = swlist->find(ui_swinfo->shortname);

			media_auditor::summary const swaudit = auditor.audit_software(*swlist, *swinfo, AUDIT_VALIDATE_FAST);

			if (audit_passed(swaudit))
			{
				reselect_last::reselect(true);
				if (!select_bios(*ui_swinfo, false) && !select_part(*swinfo, *ui_swinfo))
					launch_system(drv.driver(), *ui_swinfo, ui_swinfo->part);
			}
			else
			{
				// otherwise, display an error
				set_error(reset_options::REMEMBER_REF, make_software_audit_fail_text(auditor, swaudit));
			}
		}
	}
}

//-------------------------------------------------
//  returns if the search can be activated
//-------------------------------------------------

bool menu_select_game::isfavorite() const
{
	return machine_filter::FAVORITE == m_persistent_data.filter_data().get_current_filter_type();
}


//-------------------------------------------------
//  change what's displayed in the info box
//-------------------------------------------------

void menu_select_game::change_info_pane(int delta)
{
	auto const cap_delta = [this, &delta] (uint8_t &current, uint8_t &total)
	{
		if ((0 > delta) && (-delta > current))
			delta = -int(unsigned(current));
		else if ((0 < delta) && ((current + unsigned(delta)) >= total))
			delta = int(unsigned(total - current - 1));
		if (delta)
		{
			current += delta;
			m_topline_datsview = 0;
		}
	};
	ui_system_info const *sys;
	ui_software_info const *soft;
	get_selection(soft, sys);
	if (!m_populated_favorites)
	{
		if (uintptr_t(sys) > skip_main_items)
			cap_delta(ui_globals::curdats_view, ui_globals::curdats_total);
	}
	else if (uintptr_t(soft) > skip_main_items)
	{
		if (soft->startempty)
			cap_delta(ui_globals::curdats_view, ui_globals::curdats_total);
		else
			cap_delta(ui_globals::cur_sw_dats_view, ui_globals::cur_sw_dats_total);
	}
}

//-------------------------------------------------
//  populate search list
//-------------------------------------------------

void menu_select_game::populate_search()
{
	// ensure search list is populated
	if (m_searchlist.empty())
	{
		std::vector<ui_system_info> const &sorted(m_persistent_data.sorted_list());
		m_searchlist.reserve(sorted.size());
		for (ui_system_info const &info : sorted)
			m_searchlist.emplace_back(1.0, std::ref(info));
	}

	// keep track of what we matched against
	const std::u32string ucs_search(ustr_from_utf8(normalize_unicode(m_search, unicode_normalization_form::D, true)));

	// check available search data
	if (m_persistent_data.is_available(persistent_data::AVAIL_UCS_SHORTNAME))
		m_searched_fields |= persistent_data::AVAIL_UCS_SHORTNAME;
	if (m_persistent_data.is_available(persistent_data::AVAIL_UCS_DESCRIPTION))
		m_searched_fields |= persistent_data::AVAIL_UCS_DESCRIPTION;
	if (m_persistent_data.is_available(persistent_data::AVAIL_UCS_MANUF_DESC))
		m_searched_fields |= persistent_data::AVAIL_UCS_MANUF_DESC;
	if (m_persistent_data.is_available(persistent_data::AVAIL_UCS_DFLT_DESC))
		m_searched_fields |= persistent_data::AVAIL_UCS_DFLT_DESC;
	if (m_persistent_data.is_available(persistent_data::AVAIL_UCS_MANUF_DFLT_DESC))
		m_searched_fields |= persistent_data::AVAIL_UCS_MANUF_DFLT_DESC;

	for (std::pair<double, std::reference_wrapper<ui_system_info const> > &info : m_searchlist)
	{
		info.first = 1.0;
		ui_system_info const &sys(info.second);

		// match shortnames
		if (m_searched_fields & persistent_data::AVAIL_UCS_SHORTNAME)
			info.first = util::edit_distance(ucs_search, sys.ucs_shortname);

		// match reading
		if (info.first && !sys.ucs_reading_description.empty())
		{
			info.first = (std::min)(util::edit_distance(ucs_search, sys.ucs_reading_description), info.first);

			// match "<manufacturer> <reading>"
			if (info.first)
				info.first = (std::min)(util::edit_distance(ucs_search, sys.ucs_manufacturer_reading_description), info.first);
		}

		// match descriptions
		if (info.first && (m_searched_fields & persistent_data::AVAIL_UCS_DESCRIPTION))
			info.first = (std::min)(util::edit_distance(ucs_search, sys.ucs_description), info.first);

		// match "<manufacturer> <description>"
		if (info.first && (m_searched_fields & persistent_data::AVAIL_UCS_MANUF_DESC))
			info.first = (std::min)(util::edit_distance(ucs_search, sys.ucs_manufacturer_description), info.first);

		// match default description
		if (info.first && (m_searched_fields & persistent_data::AVAIL_UCS_DFLT_DESC) && !sys.ucs_default_description.empty())
		{
			info.first = (std::min)(util::edit_distance(ucs_search, sys.ucs_default_description), info.first);

			// match "<manufacturer> <default description>"
			if (info.first && (m_searched_fields & persistent_data::AVAIL_UCS_MANUF_DFLT_DESC))
				info.first = (std::min)(util::edit_distance(ucs_search, sys.ucs_manufacturer_default_description), info.first);
		}
	}

	// sort according to edit distance
	std::stable_sort(
			m_searchlist.begin(),
			m_searchlist.end(),
			[] (auto const &lhs, auto const &rhs) { return lhs.first < rhs.first; });
}


//-------------------------------------------------
//  get (possibly cached) icon texture
//-------------------------------------------------

render_texture *menu_select_game::get_icon_texture(int linenum, void *selectedref)
{
	game_driver const *const driver(m_populated_favorites
			? reinterpret_cast<ui_software_info const *>(selectedref)->driver
			: reinterpret_cast<ui_system_info const *>(selectedref)->driver);
	assert(driver);

	icon_cache::iterator icon(m_icons.find(driver));
	if ((m_icons.end() == icon) || !icon->second.texture)
	{
		if (m_icon_paths.empty())
			m_icon_paths = make_icon_paths(nullptr);

		// allocate an entry or allocate a texture on forced redraw
		if (m_icons.end() == icon)
		{
			icon = m_icons.emplace(driver, texture_ptr(machine().render().texture_alloc(), machine().render())).first;
		}
		else
		{
			assert(!icon->second.texture);
			icon->second.texture.reset(machine().render().texture_alloc());
		}

		// set clone status
		bool cloneof = strcmp(driver->parent, "0");
		if (cloneof)
		{
			auto cx = driver_list::find(driver->parent);
			if ((cx >= 0) && (driver_list::driver(cx).flags & machine_flags::IS_BIOS_ROOT))
				cloneof = false;
		}

		bitmap_argb32 tmp;
		emu_file snapfile(std::string(m_icon_paths), OPEN_FLAG_READ);
		if (!snapfile.open(std::string(driver->name) + ".ico"))
		{
			render_load_ico_highest_detail(snapfile, tmp);
			snapfile.close();
		}
		if (!tmp.valid() && cloneof && !snapfile.open(std::string(driver->parent) + ".ico"))
		{
			render_load_ico_highest_detail(snapfile, tmp);
			snapfile.close();
		}

		scale_icon(std::move(tmp), icon->second);
	}

	return icon->second.bitmap.valid() ? icon->second.texture.get() : nullptr;
}


void menu_select_game::inkey_export()
{
	std::vector<game_driver const *> list;
	if (m_populated_favorites)
	{
		// iterate over favorites
		mame_machine_manager::instance()->favorite().apply(
				[&list] (ui_software_info const &info)
				{
					assert(info.driver);
					if (info.startempty)
						list.push_back(info.driver);
				});
	}
	else
	{
		list.reserve(m_displaylist.size());
		for (ui_system_info const &info : m_displaylist)
			list.emplace_back(info.driver);
	}

	menu::stack_push<menu_export>(ui(), container(), std::move(list));
}

//-------------------------------------------------
//  load drivers infos from file
//-------------------------------------------------

bool menu_select_game::load_available_machines()
{
	// try to load available drivers from file
	emu_file file(ui().options().ui_path(), OPEN_FLAG_READ);
	if (file.open(std::string(emulator_info::get_configname()) + "_avail.ini"))
		return false;

	char rbuf[MAX_CHAR_INFO];
	std::string readbuf;
	file.gets(rbuf, MAX_CHAR_INFO);
	file.gets(rbuf, MAX_CHAR_INFO);
	readbuf = chartrimcarriage(rbuf);
	std::string a_rev = string_format("%s%s", UI_VERSION_TAG, bare_build_version);

	// version not matching ? exit
	if (a_rev != readbuf)
	{
		file.close();
		return false;
	}

	// load available list
	std::unordered_set<std::string> available;
	while (file.gets(rbuf, MAX_CHAR_INFO))
	{
		readbuf = strtrimspace(rbuf);

		if (readbuf.empty() || ('#' == readbuf[0])) // ignore empty lines and line comments
			;
		else if ('[' == readbuf[0]) // throw out the rest of the file if we find a section heading
			break;
		else
			available.emplace(std::move(readbuf));
	}
	file.close();

	// turn it into the sorted system list we all love
	for (ui_system_info &info : m_persistent_data.sorted_list())
	{
		std::unordered_set<std::string>::iterator const it(available.find(&info.driver->name[0]));
		bool const found(available.end() != it);
		info.available = found;
		if (found)
			available.erase(it);
	}

	return true;
}

//-------------------------------------------------
//  load custom filters info from file
//-------------------------------------------------

void menu_select_game::load_custom_filters()
{
	emu_file file(ui().options().ui_path(), OPEN_FLAG_READ);
	if (!file.open(util::string_format("custom_%s_filter.ini", emulator_info::get_configname())))
	{
		machine_filter::ptr flt(machine_filter::create(file, m_persistent_data.filter_data()));
		if (flt)
			m_persistent_data.filter_data().set_filter(std::move(flt)); // not emplace/insert - could replace bogus filter from ui.ini line
		file.close();
	}

}


//-------------------------------------------------
//  draw left box
//-------------------------------------------------

float menu_select_game::draw_left_panel(float x1, float y1, float x2, float y2)
{
	machine_filter_data &filter_data(m_persistent_data.filter_data());
	return menu_select_launch::draw_left_panel<machine_filter>(filter_data.get_current_filter_type(), filter_data.get_filters(), x1, y1, x2, y2);
}


//-------------------------------------------------
//  get selected software and/or driver
//-------------------------------------------------

void menu_select_game::get_selection(ui_software_info const *&software, ui_system_info const *&system) const
{
	if (m_populated_favorites)
	{
		software = reinterpret_cast<ui_software_info const *>(get_selection_ptr());
		system = nullptr;
	}
	else
	{
		software = nullptr;
		system = reinterpret_cast<ui_system_info const *>(get_selection_ptr());
	}
}

void menu_select_game::make_topbox_text(std::string &line0, std::string &line1, std::string &line2) const
{
	line0 = string_format(_("%1$s %2$s ( %3$d / %4$d machines (%5$d BIOS) )"),
			emulator_info::get_appname(),
			bare_build_version,
			m_available_items,
			(driver_list::total() - 1),
			m_persistent_data.bios_count());

	if (m_populated_favorites)
	{
		line1.clear();
	}
	else
	{
		machine_filter const *const it(m_persistent_data.filter_data().get_current_filter());
		char const *const filter(it ? it->filter_text() : nullptr);
		if (filter)
			line1 = string_format(_("%1$s: %2$s - Search: %3$s_"), it->display_name(), filter, m_search);
		else
			line1 = string_format(_("Search: %1$s_"), m_search);
	}

	line2.clear();
}


std::string menu_select_game::make_software_description(ui_software_info const &software) const
{
	// first line is system
	return string_format(_("System: %1$-.100s"), software.driver->type.fullname()); // TODO: localise description
}


void menu_select_game::filter_selected()
{
	if ((machine_filter::FIRST <= m_filter_highlight) && (machine_filter::LAST >= m_filter_highlight))
	{
		m_persistent_data.filter_data().get_filter(machine_filter::type(m_filter_highlight)).show_ui(
				ui(),
				container(),
				[this] (machine_filter &filter)
				{
					set_switch_image();
					machine_filter::type const new_type(filter.get_type());
					if (machine_filter::CUSTOM == new_type)
					{
						emu_file file(ui().options().ui_path(), OPEN_FLAG_WRITE | OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_PATHS);
						if (!file.open(util::string_format("custom_%s_filter.ini", emulator_info::get_configname())))
						{
							filter.save_ini(file, 0);
							file.close();
						}
					}
					m_persistent_data.filter_data().set_current_filter_type(new_type);
					reset(reset_options::SELECT_FIRST);
				});
	}
}

} // namespace ui
