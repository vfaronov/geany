/*
 *      navqueue.c - this file is part of Geany, a fast and lightweight IDE
 *
 *      Copyright 2007 Dave Moore <wrex006(at)gmail(dot)com>
 *      Copyright 2007-2012 Enrico Tröger <enrico(dot)troeger(at)uvena(dot)de>
 *      Copyright 2007-2012 Nick Treleaven <nick(dot)treleaven(at)btinternet(dot)com>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License along
 *      with this program; if not, write to the Free Software Foundation, Inc.,
 *      51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Simple code navigation
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "navqueue.h"

#include "document.h"
#include "geanyobject.h"
#include "sciwrappers.h"
#include "toolbar.h"
#include "utils.h"

#include "gtkcompat.h"


#define MAX_NAVQUEUE_LENGTH 100


/* for the navigation history queue */
typedef struct
{
	/* The filename in which the anchor is located. */
	gchar *file;

	/* The document in which the anchor is located. This is an optimization to avoid calling
	 * document_find_by_filename often (it is linear in the number of open documents).
	 * If the document is closed, the GeanyDocument is zeroed, but the pointer stays valid. */
	GeanyDocument *doc;

	/* A unique ID that is associated with the navigation indicator in Scintilla. */
	gint id;

	/* The Scintilla position at the time when this anchor was created. It may be invalidated
	 * by changes to the document (#1480), but it's useful as fallback and for optimization. */
	gint pos;
} NavigationAnchor;

static GQueue *navigation_queue;
static guint nav_queue_pos;
static gint counter;

static GtkAction *navigation_buttons[2];



void navqueue_init(void)
{
	navigation_queue = g_queue_new();
	nav_queue_pos = 0;
	counter = 0;

	navigation_buttons[0] = toolbar_get_action_by_name("NavBack");
	navigation_buttons[1] = toolbar_get_action_by_name("NavFor");

	gtk_action_set_sensitive(navigation_buttons[0], FALSE);
	gtk_action_set_sensitive(navigation_buttons[1], FALSE);
}


void navqueue_free(void)
{
	while (! g_queue_is_empty(navigation_queue))
	{
		g_free(g_queue_pop_tail(navigation_queue));
	}
	g_queue_free(navigation_queue);
}


static void adjust_buttons(void)
{
	if (g_queue_get_length(navigation_queue) < 2)
	{
		gtk_action_set_sensitive(navigation_buttons[0], FALSE);
		gtk_action_set_sensitive(navigation_buttons[1], FALSE);
		return;
	}
	if (nav_queue_pos == 0)
	{
		gtk_action_set_sensitive(navigation_buttons[0], TRUE);
		gtk_action_set_sensitive(navigation_buttons[1], FALSE);
		return;
	}
	/* forward should be sensitive since where not at the start */
	gtk_action_set_sensitive(navigation_buttons[1], TRUE);

	/* back should be sensitive if there's a place to go back to */
	(nav_queue_pos < g_queue_get_length(navigation_queue) - 1) ?
		gtk_action_set_sensitive(navigation_buttons[0], TRUE) :
			gtk_action_set_sensitive(navigation_buttons[0], FALSE);
}


static NavigationAnchor *set_anchor(GeanyDocument *doc, gint pos)
{
	NavigationAnchor *anchor = g_new0(NavigationAnchor, 1);

	counter++;
	anchor->doc = doc;
	anchor->file = g_strdup(doc->file_name);
	anchor->id = counter;
	anchor->pos = pos;
	sci_indicator_set(doc->editor->sci, GEANY_INDICATOR_NAVIGATION);
	sci_indicator_set_value(doc->editor->sci, anchor->id);
	sci_indicator_fill(doc->editor->sci, anchor->pos, 1);
	return anchor;
}


static void refresh_anchor(NavigationAnchor *anchor)
{
	gint pos, prev;

	/* GeanyDocument is zeroed when the document is closed, and may later be reused for
	 * a different file, so make sure our doc pointer is up to date. */
	if (! DOC_VALID(anchor->doc) || ! utils_str_equal(anchor->doc->file_name, anchor->file))
		anchor->doc = document_find_by_filename(anchor->file);

	if (anchor->doc == NULL || anchor->id == 0)
		return;

	/* Now we want to update anchor->pos, which may have shifted due to changes in
	 * the document's text. The new position can be found from the navigation indicator. */

	/* First, a shortcut for the common case where text has not changed. */
	if (sci_indicator_value_at(anchor->doc->editor->sci, GEANY_INDICATOR_NAVIGATION,
			anchor->pos) == anchor->id)
		return;

	/* Iterate over all ranges of the navigation indicator in this document, looking for
	 * the range with this anchor's ID. */
	pos = 0;
	do
	{
		prev = pos;
		if (sci_indicator_value_at(anchor->doc->editor->sci, GEANY_INDICATOR_NAVIGATION, pos)
			== anchor->id)
		{
			anchor->pos = pos;
			return;
		}
		pos = sci_indicator_end(anchor->doc->editor->sci, GEANY_INDICATOR_NAVIGATION, pos);
	} while (pos > prev);

	/* There may be no indicator range with the anchor's ID, e.g. if the document has been
	 * reloaded, or the text in question has been deleted. In that case, invalidate the ID
	 * so we never have to look for it again. */
	anchor->id = 0;
}


static void clear_anchor(NavigationAnchor *anchor)
{
	refresh_anchor(anchor);
	if (anchor->doc != NULL
		&& sci_indicator_value_at(anchor->doc->editor->sci, GEANY_INDICATOR_NAVIGATION,
			anchor->pos) == anchor->id)
	{
		sci_indicator_set(anchor->doc->editor->sci, GEANY_INDICATOR_NAVIGATION);
		sci_indicator_clear(anchor->doc->editor->sci, anchor->pos, 1);
	}

	g_free(anchor->file);
	g_free(anchor);
}


static gboolean
queue_pos_matches(guint queue_pos, GeanyDocument *doc, gint pos)
{
	if (queue_pos < g_queue_get_length(navigation_queue))
	{
		NavigationAnchor *anchor = g_queue_peek_nth(navigation_queue, queue_pos);

		if (utils_str_equal(anchor->file, doc->file_name))
		{
			refresh_anchor(anchor);
			return anchor->pos == pos;
		}
	}
	return FALSE;
}


void navqueue_add_position(GeanyDocument *doc, gint pos)
{
	NavigationAnchor *anchor;

	if (doc->file_name == NULL)
		return;

	if (queue_pos_matches(nav_queue_pos, doc, pos))
		return;	/* prevent duplicates */

	/* if we've jumped to a new position from inside the queue rather than going forward */
	while (nav_queue_pos > 0)
	{
		clear_anchor(g_queue_pop_head(navigation_queue));
		nav_queue_pos--;
	}

	anchor = set_anchor(doc, pos);
	g_queue_push_head(navigation_queue, anchor);

	/* Avoid accumulating too many indicator positions, so refresh_anchor stays fast. */
	while (g_queue_get_length(navigation_queue) > MAX_NAVQUEUE_LENGTH)
		clear_anchor(g_queue_pop_tail(navigation_queue));

	adjust_buttons();
}


/**
 *  Adds old file position and new file position to the navqueue, then goes to the new position.
 *
 *  @param old_doc The document of the previous position, if set as invalid (@c NULL) then no old
 *         position is set
 *  @param new_doc The document of the new position, must be valid.
 *  @param line the line number of the new position. It is counted with 1 as the first line, not 0.
 *
 *  @return @c TRUE if the cursor has changed the position to @a line or @c FALSE otherwise.
 **/
GEANY_API_SYMBOL
gboolean navqueue_goto_line(GeanyDocument *old_doc, GeanyDocument *new_doc, gint line)
{
	gint pos;

	g_return_val_if_fail(old_doc == NULL || old_doc->is_valid, FALSE);
	g_return_val_if_fail(DOC_VALID(new_doc), FALSE);
	g_return_val_if_fail(line >= 1, FALSE);

	pos = sci_get_position_from_line(new_doc->editor->sci, line - 1);

	/* first add old file position */
	if (old_doc != NULL)
	{
		gint cur_pos = sci_get_current_position(old_doc->editor->sci);

		navqueue_add_position(old_doc, cur_pos);
	}

	/* now add new file position */
	navqueue_add_position(new_doc, pos);

	return editor_goto_pos(new_doc->editor, pos, TRUE);
}


static gboolean goto_anchor(NavigationAnchor *anchor)
{
	refresh_anchor(anchor);
	if (anchor->doc == NULL)
		return FALSE;
	return editor_goto_pos(anchor->doc->editor, anchor->pos, TRUE);
}


void navqueue_go_back(void)
{
	NavigationAnchor *prev;
	GeanyDocument *doc = document_get_current();

	/* If the navqueue is currently at some position A, but the actual cursor is at some other
	 * place B, we should add B to the navqueue, so that (1) we go back to A, not to the next
	 * item in the queue; and (2) we can later restore B by going forward.
	 * (If A = B, navqueue_add_position will ignore it.) */
	if (doc)
		navqueue_add_position(doc, sci_get_current_position(doc->editor->sci));
	else
		/* see also https://github.com/geany/geany/pull/1537 */
		g_warning("Attempted navigation when nothing is open");

	/* return if theres no place to go back to */
	if (g_queue_is_empty(navigation_queue) ||
		nav_queue_pos >= g_queue_get_length(navigation_queue) - 1)
		return;

	/* jump back */
	prev = g_queue_peek_nth(navigation_queue, nav_queue_pos + 1);
	if (goto_anchor(prev))
	{
		nav_queue_pos++;
	}
	else
	{
		/** TODO: add option to re open the file */
		clear_anchor(g_queue_pop_nth(navigation_queue, nav_queue_pos + 1));
	}
	adjust_buttons();
}


void navqueue_go_forward(void)
{
	NavigationAnchor *next;

	if (nav_queue_pos < 1 ||
		nav_queue_pos >= g_queue_get_length(navigation_queue))
		return;

	/* jump forward */
	next = g_queue_peek_nth(navigation_queue, nav_queue_pos - 1);
	if (goto_anchor(next))
	{
		nav_queue_pos--;
	}
	else
	{
		/** TODO: add option to re open the file */
		clear_anchor(g_queue_pop_nth(navigation_queue, nav_queue_pos - 1));
	}

	adjust_buttons();
}


static gint find_by_filename(gconstpointer a, gconstpointer b)
{
	if (utils_str_equal(((const NavigationAnchor*)a)->file, (const gchar*) b))
		return 0;
	else
		return 1;
}


/* Remove all elements with the given filename */
void navqueue_remove_file(const gchar *filename)
{
	GList *match;

	if (filename == NULL)
		return;

	while ((match = g_queue_find_custom(navigation_queue, filename, find_by_filename)))
	{
		clear_anchor(match->data);
		g_queue_delete_link(navigation_queue, match);
	}

	if (nav_queue_pos >= g_queue_get_length(navigation_queue))
		/* TODO: Should do something smarter (i.e. shift the position towards the head
		 * until it's valid), but not sure how to do that well given the GQueue primitives. */
		nav_queue_pos = 0;

	adjust_buttons();
}
