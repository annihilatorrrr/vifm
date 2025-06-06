/* vifm
 * Copyright (C) 2014 xaizek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "statusbar.h"

#include <curses.h> /* mvwin() werase() */

#include <assert.h> /* assert() */
#include <stdarg.h> /* va_list va_start() va_end() */
#include <stdlib.h> /* free() */
#include <string.h> /* strcat() strchr() strcmp() strcpy() strdup() strncpy() */

#include "../cfg/config.h"
#include "../engine/mode.h"
#include "../modes/modes.h"
#include "../modes/more.h"
#include "../utils/macros.h"
#include "../utils/str.h"
#include "../utils/utf8.h"
#include "../status.h"
#include "statusline.h"
#include "ui.h"

static void vstatus_bar_messagef(int error, const char format[], va_list ap);
static void status_bar_message(const char message[], int error, int is_history);
static void truncate_with_ellipsis(const char msg[], size_t width,
		char buffer[]);

/* Message displayed on multi-line or too long status bar message. */
static const char PRESS_ENTER_MSG[] = "Press ENTER or type command to continue";

/* Last message that was printed on the status bar. */
static char *last_message;
/* Whether status bar takes up more than single line on a screen. */
static int multiline_status_bar;
/* Whether status bar is currently in a locked state. */
static int is_locked;

void
ui_sb_clear(void)
{
	(void)ui_stat_reposition(1, 0);

	werase(status_bar);
	wresize(status_bar, 1, getmaxx(stdscr) - FIELDS_WIDTH());
	mvwin(status_bar, getmaxy(stdscr) - 1, 0);
	wnoutrefresh(status_bar);

	if(curr_stats.load_stage <= 2)
	{
		multiline_status_bar = 0;
		stats_refresh_later();
		return;
	}

	if(multiline_status_bar)
	{
		multiline_status_bar = 0;
		update_screen(UT_FULL);
	}
	multiline_status_bar = 0;
}

void
ui_sb_quick_msgf(const char format[], ...)
{
	va_list ap;

	if(curr_stats.load_stage < 2 || status_bar == NULL)
	{
		return;
	}

	va_start(ap, format);

	checked_wmove(status_bar, 0, 0);
	werase(status_bar);
	vw_printw(status_bar, format, ap);
	wnoutrefresh(status_bar);
	doupdate();

	va_end(ap);
}

void
ui_sb_quick_msg_clear(void)
{
	if(curr_stats.save_msg || ui_sb_multiline())
	{
		ui_sb_msg(NULL);
	}
	else
	{
		ui_sb_quick_msgf("%s", "");
	}

	if(modes_is_cmdline_like())
	{
		/* Restore previous contents of the status bar. */
		stats_redraw_later();
	}
}

void
ui_sb_msg(const char message[])
{
	status_bar_message(message, /*error=*/0, /*is_history=*/0);
}

int
ui_sb_msg_show_history(void)
{
	char *lines;
	size_t len;
	int count;
	int t;

	lines = NULL;
	len = 0;
	count = curr_stats.msg_tail - curr_stats.msg_head;
	if(count < 0)
		count += ARRAY_LEN(curr_stats.msgs);
	t = (curr_stats.msg_head + 1) % ARRAY_LEN(curr_stats.msgs);
	while(count-- > 0)
	{
		const char *msg = curr_stats.msgs[t];
		char *new_lines = realloc(lines, len + 1 + strlen(msg) + 1);
		if(new_lines != NULL)
		{
			lines = new_lines;
			len += sprintf(lines + len, "%s%s", (len == 0) ? "": "\n", msg);
			t = (t + 1) % ARRAY_LEN(curr_stats.msgs);
		}
	}

	if(lines == NULL)
		return 0;

	status_bar_message(lines, /*error=*/0, /*is_history=*/1);

	free(lines);
	return 1;
}

void
ui_sb_msgf(const char format[], ...)
{
	va_list ap;

	va_start(ap, format);

	vstatus_bar_messagef(0, format, ap);

	va_end(ap);
}

void
ui_sb_err(const char message[])
{
	status_bar_message(message, /*error=*/1, /*is_history=*/0);
}

void
ui_sb_errf(const char message[], ...)
{
	va_list ap;

	va_start(ap, message);

	vstatus_bar_messagef(1, message, ap);

	va_end(ap);
}

static void
vstatus_bar_messagef(int error, const char format[], va_list ap)
{
	char buf[1024];

	vsnprintf(buf, sizeof(buf), format, ap);
	status_bar_message(buf, error, /*is_history=*/0);
}

/* Displays an informational or an error message.  If is_history is non-zero,
 * the message is not truncated and is not added to history. */
static void
status_bar_message(const char msg[], int error, int is_history)
{
	/* TODO: Refactor this function status_bar_message() */

	static int err;

	if(msg != NULL)
	{
		if(replace_string(&last_message, msg))
		{
			return;
		}

		err = error;

		if(!is_history)
		{
			stats_save_msg(last_message);
		}
	}
	else
	{
		msg = last_message;
	}

	/* We bail out here instead of right at the top to record the message to make
	 * it accessible in tests. */
	if(curr_stats.load_stage <= 0)
	{
		return;
	}

	if(msg == NULL || is_locked)
	{
		return;
	}

	int max_width = getmaxx(stdscr);
	int msg_lines = count_lines(msg, max_width);

	int output_lines = msg_lines;
	if(msg_lines > 1 || utf8_strsw(msg) > (size_t)getmaxx(status_bar))
	{
		/* Need one more line to display PRESS_ENTER_MSG. */
		++output_lines;
	}

	const char *output_msg = msg;
	char truncated_msg[2048];

	if(output_lines > 1)
	{
		if(cfg.trunc_normal_sb_msgs && !err && !is_history && msg_lines == 1)
		{
			truncate_with_ellipsis(msg, max_width - FIELDS_WIDTH(), truncated_msg);
			output_msg = truncated_msg;
			output_lines = 1;
		}
		else
		{
			int extra = DIV_ROUND_UP(ARRAY_LEN(PRESS_ENTER_MSG) - 1, max_width) - 1;
			output_lines += extra;
		}
	}

	if(output_lines > getmaxy(stdscr))
	{
		modmore_enter(msg);
		return;
	}

	(void)ui_stat_reposition(output_lines, 0);
	mvwin(status_bar, getmaxy(stdscr) - output_lines, 0);
	if(output_lines == 1)
	{
		wresize(status_bar, output_lines, max_width - FIELDS_WIDTH());
	}
	else
	{
		wresize(status_bar, output_lines, max_width);
	}
	checked_wmove(status_bar, 0, 0);

	if(err)
	{
		col_attr_t col = cfg.cs.color[CMD_LINE_COLOR];
		cs_mix_colors(&col, &cfg.cs.color[ERROR_MSG_COLOR]);
		ui_set_attr(status_bar, &col, -1);
	}
	else
	{
		ui_set_attr(status_bar, &cfg.cs.color[CMD_LINE_COLOR],
				cfg.cs.pair[CMD_LINE_COLOR]);
	}
	werase(status_bar);

	wprint(status_bar, output_msg);
	multiline_status_bar = output_lines > 1;
	if(multiline_status_bar)
	{
		checked_wmove(status_bar,
				output_lines - DIV_ROUND_UP(ARRAY_LEN(PRESS_ENTER_MSG), max_width), 0);
		wclrtoeol(status_bar);
		if(output_lines < msg_lines)
			wprintw(status_bar, "%d of %d lines.  ", output_lines, msg_lines);
		wprintw(status_bar, "%s", PRESS_ENTER_MSG);
	}

	ui_drop_attr(status_bar);

	update_all_windows();
	/* This is needed because update_all_windows() doesn't call doupdate() if
	 * curr_stats.load_stage == 1. */
	doupdate();
}

/* Truncates the msg to the width by placing ellipsis in the middle and writes
 * result into the buffer. */
static void
truncate_with_ellipsis(const char msg[], size_t width, char buffer[])
{
	const size_t screen_width = utf8_strsw(msg);
	const size_t ell_width = utf8_strsw(curr_stats.ellipsis);
	const size_t screen_left_width = (width - ell_width)/2;
	const size_t screen_right_width = (width - ell_width) - screen_left_width;
	const size_t left = utf8_nstrsnlen(msg, screen_left_width);
	const size_t right = utf8_nstrsnlen(msg, screen_width - screen_right_width);
	strncpy(buffer, msg, left);
	strcpy(buffer + left, curr_stats.ellipsis);
	strcat(buffer + left, msg + right);
	assert(utf8_strsw(buffer) == width);
}

int
ui_sb_multiline(void)
{
	return multiline_status_bar;
}

const char *
ui_sb_last(void)
{
	return last_message;
}

void
ui_sb_lock(void)
{
	assert(!is_locked && "Can't lock status bar that's already locked.");
	is_locked = 1;
}

void
ui_sb_unlock(void)
{
	assert(is_locked && "Can't unlock status bar that's not locked.");
	is_locked = 0;
}

int
ui_sb_locked(void)
{
	return is_locked;
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
