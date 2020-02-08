/*
   Widget group features module for the Midnight Commander

   Copyright (C) 2020
   The Free Software Foundation, Inc.

   Written by:
   Andrew Borodin <aborodin@vmail.ru>, 2013

   This file is part of the Midnight Commander.

   The Midnight Commander is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the License,
   or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/** \file group.c
 *  \brief Source: widget group features module
 */

#include <config.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "lib/global.h"

#include "lib/widget.h"

/*** global variables ****************************************************************************/

/*** file scope macro definitions ****************************************************************/

/*** file scope type declarations ****************************************************************/

/*** file scope variables ************************************************************************/

/* --------------------------------------------------------------------------------------------- */
/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */

static GList *
group_get_next_or_prev_of (GList * list, gboolean next)
{
    GList *l = NULL;

    if (list != NULL)
    {
        WGroup *owner = WIDGET (list->data)->owner;

        if (owner != NULL)
        {
            if (next)
            {
                l = g_list_next (list);
                if (l == NULL)
                    l = owner->widgets;
            }
            else
            {
                l = g_list_previous (list);
                if (l == NULL)
                    l = g_list_last (owner->widgets);
            }
        }
    }

    return l;
}

/* --------------------------------------------------------------------------------------------- */

static void
group_select_next_or_prev (WGroup * g, gboolean next)
{
    if (g->widgets != NULL && g->current != NULL)
    {
        GList *l = g->current;
        Widget *w;

        do
        {
            l = group_get_next_or_prev_of (l, next);
            w = WIDGET (l->data);
        }
        while ((widget_get_state (w, WST_DISABLED) || !widget_get_options (w, WOP_SELECTABLE))
               && l != g->current);

        widget_select (l->data);
    }
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */

/**
 * Insert widget to group before specified widget with specified positioning.
 * Make the inserted widget current.
 *
 * @param g WGroup object
 * @param w widget to be added
 * @pos positioning flags
 * @param before add @w before this widget
 *
 * @return widget ID
 */

unsigned long
group_add_widget_autopos (WGroup * g, void *w, widget_pos_flags_t pos_flags, const void *before)
{
    Widget *wg = WIDGET (g);
    Widget *ww = WIDGET (w);
    GList *new_current;

    /* Don't accept NULL widget. This shouldn't happen */
    assert (ww != NULL);

    if ((pos_flags & WPOS_CENTER_HORZ) != 0)
        ww->x = (wg->cols - ww->cols) / 2;
    ww->x += wg->x;

    if ((pos_flags & WPOS_CENTER_VERT) != 0)
        ww->y = (wg->lines - ww->lines) / 2;
    ww->y += wg->y;

    ww->owner = g;
    ww->pos_flags = pos_flags;
    ww->id = g->widget_id++;

    if (g->widgets == NULL || before == NULL)
    {
        g->widgets = g_list_append (g->widgets, ww);
        new_current = g_list_last (g->widgets);
    }
    else
    {
        GList *b;

        b = g_list_find (g->widgets, before);

        /* don't accept widget not from group. This shouldn't happen */
        assert (b != NULL);

        b = g_list_next (b);
        g->widgets = g_list_insert_before (g->widgets, b, ww);
        if (b != NULL)
            new_current = g_list_previous (b);
        else
            new_current = g_list_last (g->widgets);
    }

    /* widget has been added at runtime */
    if (widget_get_state (wg, WST_ACTIVE))
    {
        send_message (ww, NULL, MSG_INIT, 0, NULL);
        widget_select (ww);
    }
    else
        g->current = new_current;

    return ww->id;
}

/* --------------------------------------------------------------------------------------------- */

/**
 * Remove widget from group.
 *
 * @param w Widget object
 */
void
group_remove_widget (void *w)
{
    WGroup *g;
    GList *d;

    /* Don't accept NULL widget. This shouldn't happen */
    assert (w != NULL);

    g = WIDGET (w)->owner;

    d = g_list_find (g->widgets, w);
    if (d == g->current)
        group_set_current_widget_next (g);

    g->widgets = g_list_delete_link (g->widgets, d);
    if (g->widgets == NULL)
        g->current = NULL;

    /* widget has been deleted at runtime */
    if (widget_get_state (WIDGET (g), WST_ACTIVE))
    {
        dlg_draw (DIALOG (g));          /* FIXME */
        group_select_current_widget (g);
    }

    WIDGET (w)->owner = NULL;
}

/* --------------------------------------------------------------------------------------------- */

/**
 * Switch current widget to widget after current in group.
 *
 * @param g WGroup object
 */

void
group_set_current_widget_next (WGroup * g)
{
    g->current = group_get_next_or_prev_of (g->current, TRUE);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Switch current widget to widget before current in group.
 *
 * @param g WGroup object
 */

void
group_set_current_widget_prev (WGroup * g)
{
    g->current = group_get_next_or_prev_of (g->current, FALSE);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Get widget that is after specified widget in group.
 *
 * @param w widget holder
 *
 * @return widget that is after "w" or NULL if "w" is NULL or widget doesn't have owner
 */

GList *
group_get_widget_next_of (GList * w)
{
    return group_get_next_or_prev_of (w, TRUE);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Get widget that is before specified widget in group.
 *
 * @param w widget holder
 *
 * @return widget that is before "w" or NULL if "w" is NULL or widget doesn't have owner
 */

GList *
group_get_widget_prev_of (GList * w)
{
    return group_get_next_or_prev_of (w, FALSE);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Try to select next widget in the Z order.
 *
 * @param g WGroup object
 */

void
group_select_next_widget (WGroup * g)
{
    group_select_next_or_prev (g, TRUE);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Try to select previous widget in the Z order.
 *
 * @param g WGroup object
 */

void
group_select_prev_widget (WGroup * g)
{
    group_select_next_or_prev (g, FALSE);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Find the widget with the specified ID in the group and select it
 *
 * @param g WGroup object
 * @param id widget ID
 */

void
group_select_widget_by_id (const WGroup * g, unsigned long id)
{
    Widget *w;

    w = dlg_find_by_id (DIALOG (g), id);
    if (w != NULL)
        widget_select (w);
}

/* --------------------------------------------------------------------------------------------- */
