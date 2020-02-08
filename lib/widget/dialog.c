/*
   Dialog box features module for the Midnight Commander

   Copyright (C) 1994-2019
   Free Software Foundation, Inc.

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

/** \file dialog.c
 *  \brief Source: dialog box features module
 */

#include <config.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "lib/global.h"

#include "lib/tty/tty.h"
#include "lib/skin.h"
#include "lib/tty/key.h"
#include "lib/strutil.h"
#include "lib/fileloc.h"        /* MC_HISTORY_FILE */
#include "lib/event.h"          /* mc_event_raise() */
#include "lib/util.h"           /* MC_PTR_FREE */
#include "lib/mcconfig.h"       /* num_history_items_recorded */

#include "lib/widget.h"
#include "lib/widget/mouse.h"

/*** global variables ****************************************************************************/

/* Color styles for normal and error dialogs */
dlg_colors_t dialog_colors;
dlg_colors_t alarm_colors;
dlg_colors_t listbox_colors;

/* Primitive way to check if the the current dialog is our dialog */
/* This is needed by async routines like load_prompt */
GList *top_dlg = NULL;

/* A hook list for idle events */
hook_t *idle_hook = NULL;

/* If set then dialogs just clean the screen when refreshing, else */
/* they do a complete refresh, refreshing all the parts of the program */
gboolean fast_refresh = FALSE;

/* left click outside of dialog closes it */
gboolean mouse_close_dialog = FALSE;

const global_keymap_t *dialog_map = NULL;

/*** file scope macro definitions ****************************************************************/

/*** file scope type declarations ****************************************************************/

/* Control widget positions in dialog */
typedef struct
{
    int shift_x;
    int scale_x;
    int shift_y;
    int scale_y;
} widget_shift_scale_t;

/*** file scope variables ************************************************************************/

/* --------------------------------------------------------------------------------------------- */
/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */

/**
  * Read histories from the ${XDG_CACHE_HOME}/mc/history file
  */
static void
dlg_read_history (WDialog * h)
{
    char *profile;
    ev_history_load_save_t event_data;

    if (num_history_items_recorded == 0)        /* this is how to disable */
        return;

    profile = mc_config_get_full_path (MC_HISTORY_FILE);
    event_data.cfg = mc_config_init (profile, TRUE);
    event_data.receiver = NULL;

    /* create all histories in dialog */
    mc_event_raise (h->event_group, MCEVENT_HISTORY_LOAD, &event_data);

    mc_config_deinit (event_data.cfg);
    g_free (profile);
}

/* --------------------------------------------------------------------------------------------- */

static int
dlg_find_widget_callback (const void *a, const void *b)
{
    const Widget *w = CONST_WIDGET (a);
    const widget_cb_fn f = b;

    return (w->callback == f) ? 0 : 1;
}

/* --------------------------------------------------------------------------------------------- */

static void
refresh_cmd (void)
{
#ifdef HAVE_SLANG
    tty_touch_screen ();
    mc_refresh ();
#else
    /* Use this if the refreshes fail */
    clr_scr ();
    repaint_screen ();
#endif /* HAVE_SLANG */
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
dlg_execute_cmd (WDialog * h, long command)
{
    WGroup *g = GROUP (h);
    cb_ret_t ret = MSG_HANDLED;

    if (send_message (h, NULL, MSG_ACTION, command, NULL) == MSG_HANDLED)
        return MSG_HANDLED;

    switch (command)
    {
    case CK_Ok:
        h->ret_value = B_ENTER;
        dlg_stop (h);
        break;
    case CK_Cancel:
        h->ret_value = B_CANCEL;
        dlg_stop (h);
        break;

    case CK_Up:
    case CK_Left:
        group_select_prev_widget (g);
        break;
    case CK_Down:
    case CK_Right:
        group_select_next_widget (g);
        break;

    case CK_Help:
        {
            ev_help_t event_data = { NULL, h->help_ctx };
            mc_event_raise (MCEVENT_GROUP_CORE, "help", &event_data);
        }
        break;

    case CK_Suspend:
        mc_event_raise (MCEVENT_GROUP_CORE, "suspend", NULL);
        refresh_cmd ();
        break;
    case CK_Refresh:
        refresh_cmd ();
        break;

    case CK_ScreenList:
        if (!widget_get_state (WIDGET (h), WST_MODAL))
            dialog_switch_list ();
        else
            ret = MSG_NOT_HANDLED;
        break;
    case CK_ScreenNext:
        if (!widget_get_state (WIDGET (h), WST_MODAL))
            dialog_switch_next ();
        else
            ret = MSG_NOT_HANDLED;
        break;
    case CK_ScreenPrev:
        if (!widget_get_state (WIDGET (h), WST_MODAL))
            dialog_switch_prev ();
        else
            ret = MSG_NOT_HANDLED;
        break;

    default:
        ret = MSG_NOT_HANDLED;
    }

    return ret;
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
dlg_handle_key (WDialog * h, int d_key)
{
    long command;

    command = keybind_lookup_keymap_command (dialog_map, d_key);
    if (command != CK_IgnoreKey)
        return dlg_execute_cmd (h, command);

    return MSG_NOT_HANDLED;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * This is the low-level mouse handler.
 * It receives a Gpm_Event event and translates it into a higher level protocol.
 */
static int
dlg_mouse_translator (Gpm_Event * event, Widget * w)
{
    mouse_event_t me;

    me = mouse_translate_event (w, event);

    return mouse_process_event (w, &me);
}

/* --------------------------------------------------------------------------------------------- */

static int
dlg_mouse_event (WDialog * h, Gpm_Event * event)
{
    Widget *wh = WIDGET (h);

    GList *p;

    /* close the dialog by mouse left click out of dialog area */
    if (mouse_close_dialog && (wh->pos_flags & WPOS_FULLSCREEN) == 0
        && ((event->buttons & GPM_B_LEFT) != 0) && ((event->type & GPM_DOWN) != 0)
        && !mouse_global_in_widget (event, wh))
    {
        h->ret_value = B_CANCEL;
        dlg_stop (h);
        return MOU_NORMAL;
    }

    if (wh->mouse_callback != NULL)
    {
        int mou;

        mou = dlg_mouse_translator (event, wh);
        if (mou != MOU_UNHANDLED)
            return mou;
    }

    if (GROUP (h)->widgets == NULL)
        return MOU_UNHANDLED;

    /* send the event to widgets in reverse Z-order */
    p = g_list_last (GROUP (h)->widgets);
    do
    {
        Widget *w = WIDGET (p->data);

        if (!widget_get_state (w, WST_DISABLED) && w->mouse_callback != NULL)
        {
            /* put global cursor position to the widget */
            int ret;

            ret = dlg_mouse_translator (event, w);
            if (ret != MOU_UNHANDLED)
                return ret;
        }

        p = g_list_previous (p);
    }
    while (p != NULL);

    return MOU_UNHANDLED;
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
dlg_try_hotkey (WDialog * h, int d_key)
{
    WGroup *g = GROUP (h);
    GList *hot_cur;
    Widget *current;
    cb_ret_t handled;
    int c;

    if (g->widgets == NULL)
        return MSG_NOT_HANDLED;

    if (g->current == NULL)
        g->current = g->widgets;

    /*
     * Explanation: we don't send letter hotkeys to other widgets if
     * the currently selected widget is an input line
     */

    current = WIDGET (g->current->data);

    if (widget_get_state (current, WST_DISABLED))
        return MSG_NOT_HANDLED;

    if (widget_get_options (current, WOP_IS_INPUT))
    {
        /* skip ascii control characters, anything else can valid character in
         * some encoding */
        if (d_key >= 32 && d_key < 256)
            return MSG_NOT_HANDLED;
    }

    /* If it's an alt key, send the message */
    c = d_key & ~ALT (0);
    if (d_key & ALT (0) && g_ascii_isalpha (c))
        d_key = g_ascii_tolower (c);

    handled = MSG_NOT_HANDLED;
    if (widget_get_options (current, WOP_WANT_HOTKEY))
        handled = send_message (current, NULL, MSG_HOTKEY, d_key, NULL);

    /* If not used, send hotkey to other widgets */
    if (handled == MSG_HANDLED)
        return MSG_HANDLED;

    hot_cur = group_get_widget_next_of (g->current);

    /* send it to all widgets */
    while (g->current != hot_cur && handled == MSG_NOT_HANDLED)
    {
        current = WIDGET (hot_cur->data);

        if (widget_get_options (current, WOP_WANT_HOTKEY)
            && !widget_get_state (current, WST_DISABLED))
            handled = send_message (current, NULL, MSG_HOTKEY, d_key, NULL);

        if (handled == MSG_NOT_HANDLED)
            hot_cur = group_get_widget_next_of (hot_cur);
    }

    if (handled == MSG_HANDLED)
        widget_select (WIDGET (hot_cur->data));

    return handled;
}

/* --------------------------------------------------------------------------------------------- */

static void
dlg_key_event (WDialog * h, int d_key)
{
    WGroup *g = GROUP (h);
    cb_ret_t handled;

    if (g->widgets == NULL)
        return;

    if (g->current == NULL)
        g->current = g->widgets;

    /* TAB used to cycle */
    if (!widget_get_options (WIDGET (g), WOP_WANT_TAB))
    {
        if (d_key == '\t')
        {
            group_select_next_widget (g);
            return;
        }
        else if ((d_key & ~(KEY_M_SHIFT | KEY_M_CTRL)) == '\t')
        {
            group_select_prev_widget (g);
            return;
        }
    }

    /* first can dlg_callback handle the key */
    handled = send_message (h, NULL, MSG_KEY, d_key, NULL);

    /* next try the hotkey */
    if (handled == MSG_NOT_HANDLED)
        handled = dlg_try_hotkey (h, d_key);

    if (handled == MSG_HANDLED)
        send_message (h, NULL, MSG_HOTKEY_HANDLED, 0, NULL);
    else
        /* not used - then try widget_callback */
        handled = send_message (g->current->data, NULL, MSG_KEY, d_key, NULL);

    /* not used- try to use the unhandled case */
    if (handled == MSG_NOT_HANDLED)
        handled = send_message (h, NULL, MSG_UNHANDLED_KEY, d_key, NULL);

    if (handled == MSG_NOT_HANDLED)
        handled = dlg_handle_key (h, d_key);

    (void) handled;
    send_message (h, NULL, MSG_POST_KEY, d_key, NULL);
}

/* --------------------------------------------------------------------------------------------- */

static void
frontend_dlg_run (WDialog * h)
{
    Widget *wh = WIDGET (h);
    Gpm_Event event;

    event.x = -1;

    /* close opened editors, viewers, etc */
    if (!widget_get_state (wh, WST_MODAL) && mc_global.midnight_shutdown)
    {
        send_message (h, NULL, MSG_VALIDATE, 0, NULL);
        return;
    }

    while (widget_get_state (wh, WST_ACTIVE))
    {
        int d_key;

        if (tty_got_winch ())
            dialog_change_screen_size ();

        if (is_idle ())
        {
            if (idle_hook)
                execute_hooks (idle_hook);

            while (widget_get_state (wh, WST_IDLE) && is_idle ())
                send_message (wh, NULL, MSG_IDLE, 0, NULL);

            /* Allow terminating the dialog from the idle handler */
            if (!widget_get_state (wh, WST_ACTIVE))
                break;
        }

        update_cursor (h);

        /* Clear interrupt flag */
        tty_got_interrupt ();
        d_key = tty_get_event (&event, h->mouse_status == MOU_REPEAT, TRUE);

        dlg_process_event (h, d_key, &event);

        if (widget_get_state (wh, WST_CLOSED))
            send_message (h, NULL, MSG_VALIDATE, 0, NULL);
    }
}

/* --------------------------------------------------------------------------------------------- */

static int
dlg_find_widget_by_id (gconstpointer a, gconstpointer b)
{
    const Widget *w = CONST_WIDGET (a);
    unsigned long id = GPOINTER_TO_UINT (b);

    return w->id == id ? 0 : 1;
}

/* --------------------------------------------------------------------------------------------- */
static void
dlg_widget_set_position (gpointer data, gpointer user_data)
{
    /* there are, mainly, 2 generally possible situations:
     * 1. control sticks to one side - it should be moved
     * 2. control sticks to two sides of one direction - it should be sized
     */

    Widget *c = WIDGET (data);
    Widget *wh = WIDGET (c->owner);
    const widget_shift_scale_t *wss = (const widget_shift_scale_t *) user_data;
    int x = c->x;
    int y = c->y;
    int cols = c->cols;
    int lines = c->lines;

    if ((c->pos_flags & WPOS_CENTER_HORZ) != 0)
        x = wh->x + (wh->cols - c->cols) / 2;
    else if ((c->pos_flags & WPOS_KEEP_LEFT) != 0 && (c->pos_flags & WPOS_KEEP_RIGHT) != 0)
    {
        x += wss->shift_x;
        cols += wss->scale_x;
    }
    else if ((c->pos_flags & WPOS_KEEP_LEFT) != 0)
        x += wss->shift_x;
    else if ((c->pos_flags & WPOS_KEEP_RIGHT) != 0)
        x += wss->shift_x + wss->scale_x;

    if ((c->pos_flags & WPOS_CENTER_VERT) != 0)
        y = wh->y + (wh->lines - c->lines) / 2;
    else if ((c->pos_flags & WPOS_KEEP_TOP) != 0 && (c->pos_flags & WPOS_KEEP_BOTTOM) != 0)
    {
        y += wss->shift_y;
        lines += wss->scale_y;
    }
    else if ((c->pos_flags & WPOS_KEEP_TOP) != 0)
        y += wss->shift_y;
    else if ((c->pos_flags & WPOS_KEEP_BOTTOM) != 0)
        y += wss->shift_y + wss->scale_y;

    widget_set_size (c, y, x, lines, cols);
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */

/** Clean the dialog area, draw the frame and the title */
void
dlg_default_repaint (WDialog * h)
{
    Widget *wh = WIDGET (h);

    int space;

    if (!widget_get_state (wh, WST_ACTIVE))
        return;

    space = h->compact ? 0 : 1;

    tty_setcolor (h->color[DLG_COLOR_NORMAL]);
    dlg_erase (h);
    tty_draw_box (wh->y + space, wh->x + space, wh->lines - 2 * space, wh->cols - 2 * space, FALSE);

    if (h->title != NULL)
    {
        /* TODO: truncate long title */
        tty_setcolor (h->color[DLG_COLOR_TITLE]);
        widget_gotoyx (h, space, (wh->cols - str_term_width1 (h->title)) / 2);
        tty_print_string (h->title);
    }
}

/* --------------------------------------------------------------------------------------------- */
/** this function allows to set dialog position */

void
dlg_set_position (WDialog * h, int y, int x, int lines, int cols)
{
    WGroup *g = GROUP (h);
    Widget *wh = WIDGET (h);
    widget_shift_scale_t wss;

    /* save old positions, will be used to reposition childs */
    int ox, oy, oc, ol;

    /* save old positions, will be used to reposition childs */
    ox = wh->x;
    oy = wh->y;
    oc = wh->cols;
    ol = wh->lines;

    wh->x = x;
    wh->y = y;
    wh->lines = lines;
    wh->cols = cols;

    /* dialog is empty */
    if (g->widgets == NULL)
        return;

    if (g->current == NULL)
        g->current = g->widgets;

    /* values by which controls should be moved */
    wss.shift_x = wh->x - ox;
    wss.scale_x = wh->cols - oc;
    wss.shift_y = wh->y - oy;
    wss.scale_y = wh->lines - ol;

    if (wss.shift_x != 0 || wss.shift_y != 0 || wss.scale_x != 0 || wss.scale_y != 0)
        g_list_foreach (g->widgets, dlg_widget_set_position, &wss);
}

/* --------------------------------------------------------------------------------------------- */
/** Set dialog size and position */

void
dlg_set_size (WDialog * h, int lines, int cols)
{
    int x = 0, y = 0;

    widget_adjust_position (WIDGET (h)->pos_flags, &y, &x, &lines, &cols);
    dlg_set_position (h, y, x, lines, cols);
}

/* --------------------------------------------------------------------------------------------- */
/** Default dialog callback */

cb_ret_t
dlg_default_callback (Widget * w, Widget * sender, widget_msg_t msg, int parm, void *data)
{
    WDialog *h = DIALOG (w);

    (void) sender;
    (void) parm;
    (void) data;

    switch (msg)
    {
    case MSG_DRAW:
        if (h->color != NULL)
        {
            dlg_default_repaint (h);
            return MSG_HANDLED;
        }
        return MSG_NOT_HANDLED;

    case MSG_IDLE:
        /* we don't want endless loop */
        widget_idle (w, FALSE);
        return MSG_HANDLED;

    case MSG_RESIZE:
        /* this is default resizing mechanism */
        /* the main idea of this code is to resize dialog
           according to flags (if any of flags require automatic
           resizing, like WPOS_CENTER, end after that reposition
           controls in dialog according to flags of widget) */
        dlg_set_size (h, w->lines, w->cols);
        return MSG_HANDLED;

    default:
        break;
    }

    return MSG_NOT_HANDLED;
}

/* --------------------------------------------------------------------------------------------- */

WDialog *
dlg_create (gboolean modal, int y1, int x1, int lines, int cols, widget_pos_flags_t pos_flags,
            gboolean compact, const int *colors, widget_cb_fn callback,
            widget_mouse_cb_fn mouse_callback, const char *help_ctx, const char *title)
{
    WDialog *new_d;
    Widget *w;

    new_d = g_new0 (WDialog, 1);
    w = WIDGET (new_d);
    widget_adjust_position (pos_flags, &y1, &x1, &lines, &cols);
    widget_init (w, y1, x1, lines, cols, (callback != NULL) ? callback : dlg_default_callback,
                 mouse_callback);
    w->pos_flags = pos_flags;
    w->options |= WOP_SELECTABLE | WOP_TOP_SELECT;

    w->state |= WST_CONSTRUCT | WST_FOCUSED;
    if (modal)
        w->state |= WST_MODAL;

    new_d->color = colors;
    new_d->help_ctx = help_ctx;
    new_d->compact = compact;
    new_d->data = NULL;

    new_d->mouse_status = MOU_UNHANDLED;

    dlg_set_title (new_d, title);

    /* unique name of event group for this dialog */
    new_d->event_group = g_strdup_printf ("%s_%p", MCEVENT_GROUP_DIALOG, (void *) new_d);

    return new_d;
}

/* --------------------------------------------------------------------------------------------- */

void
dlg_set_default_colors (void)
{
    dialog_colors[DLG_COLOR_NORMAL] = COLOR_NORMAL;
    dialog_colors[DLG_COLOR_FOCUS] = COLOR_FOCUS;
    dialog_colors[DLG_COLOR_HOT_NORMAL] = COLOR_HOT_NORMAL;
    dialog_colors[DLG_COLOR_HOT_FOCUS] = COLOR_HOT_FOCUS;
    dialog_colors[DLG_COLOR_TITLE] = COLOR_TITLE;

    alarm_colors[DLG_COLOR_NORMAL] = ERROR_COLOR;
    alarm_colors[DLG_COLOR_FOCUS] = ERROR_FOCUS;
    alarm_colors[DLG_COLOR_HOT_NORMAL] = ERROR_HOT_NORMAL;
    alarm_colors[DLG_COLOR_HOT_FOCUS] = ERROR_HOT_FOCUS;
    alarm_colors[DLG_COLOR_TITLE] = ERROR_TITLE;

    listbox_colors[DLG_COLOR_NORMAL] = PMENU_ENTRY_COLOR;
    listbox_colors[DLG_COLOR_FOCUS] = PMENU_SELECTED_COLOR;
    listbox_colors[DLG_COLOR_HOT_NORMAL] = PMENU_ENTRY_COLOR;
    listbox_colors[DLG_COLOR_HOT_FOCUS] = PMENU_SELECTED_COLOR;
    listbox_colors[DLG_COLOR_TITLE] = PMENU_TITLE_COLOR;
}

/* --------------------------------------------------------------------------------------------- */

void
dlg_erase (WDialog * h)
{
    Widget *wh = WIDGET (h);

    if (wh != NULL && widget_get_state (wh, WST_ACTIVE))
        tty_fill_region (wh->y, wh->x, wh->lines, wh->cols, ' ');
}

/* --------------------------------------------------------------------------------------------- */

void
do_refresh (void)
{
    GList *d = top_dlg;

    if (fast_refresh)
    {
        if (d != NULL)
            dlg_draw (DIALOG (d->data));
    }
    else
    {
        /* Search first fullscreen dialog */
        for (; d != NULL; d = g_list_next (d))
            if ((WIDGET (d->data)->pos_flags & WPOS_FULLSCREEN) != 0)
                break;
        /* back to top dialog */
        for (; d != NULL; d = g_list_previous (d))
            dlg_draw (DIALOG (d->data));
    }
}

/* --------------------------------------------------------------------------------------------- */
/** Find the widget with the given callback in the dialog h */

Widget *
find_widget_type (const WDialog * h, widget_cb_fn callback)
{
    GList *w;

    w = g_list_find_custom (CONST_GROUP (h)->widgets, (gconstpointer) callback,
                            dlg_find_widget_callback);

    return (w == NULL) ? NULL : WIDGET (w->data);
}

/* --------------------------------------------------------------------------------------------- */

GList *
dlg_find (const WDialog * h, const Widget * w)
{
    const WGroup *g = CONST_GROUP (h);

    return (w->owner == NULL || w->owner != g) ? NULL : g_list_find (g->widgets, w);
}

/* --------------------------------------------------------------------------------------------- */
/** Find the widget with the given id */

Widget *
dlg_find_by_id (const WDialog * h, unsigned long id)
{
    GList *w;

    w = g_list_find_custom (CONST_GROUP (h)->widgets, GUINT_TO_POINTER (id), dlg_find_widget_by_id);
    return w != NULL ? WIDGET (w->data) : NULL;
}

/* --------------------------------------------------------------------------------------------- */

void
update_cursor (WDialog * h)
{
    WGroup *g = GROUP (h);
    GList *p = g->current;

    if (p != NULL && widget_get_state (WIDGET (h), WST_ACTIVE))
    {
        Widget *w = WIDGET (p->data);

        if (!widget_get_state (w, WST_DISABLED) && widget_get_options (w, WOP_WANT_CURSOR))
            send_message (w, NULL, MSG_CURSOR, 0, NULL);
        else
            do
            {
                p = group_get_widget_next_of (p);
                if (p == g->current)
                    break;

                w = WIDGET (p->data);

                if (!widget_get_state (w, WST_DISABLED) && widget_get_options (w, WOP_WANT_CURSOR)
                    && send_message (w, NULL, MSG_CURSOR, 0, NULL) == MSG_HANDLED)
                    break;
            }
            while (TRUE);
    }
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Redraw the widgets in reverse order, leaving the current widget
 * as the last one
 */

void
dlg_draw (WDialog * h)
{
    WGroup *g = GROUP (h);

    if (!widget_get_state (WIDGET (h), WST_ACTIVE))
        return;

    if (g->winch_pending)
    {
        g->winch_pending = FALSE;
        send_message (h, NULL, MSG_RESIZE, 0, NULL);
    }

    send_message (h, NULL, MSG_DRAW, 0, NULL);
    group_send_broadcast_msg (g, MSG_DRAW);
    update_cursor (h);
}

/* --------------------------------------------------------------------------------------------- */

void
dlg_stop (WDialog * h)
{
    widget_set_state (WIDGET (h), WST_CLOSED, TRUE);
}

/* --------------------------------------------------------------------------------------------- */
/** Init the process */

void
dlg_init (WDialog * h)
{
    WGroup *g = GROUP (h);
    Widget *wh = WIDGET (h);

    if (top_dlg != NULL && widget_get_state (WIDGET (top_dlg->data), WST_MODAL))
        widget_set_state (wh, WST_MODAL, TRUE);

    /* add dialog to the stack */
    top_dlg = g_list_prepend (top_dlg, h);

    /* Initialize dialog manager and widgets */
    if (widget_get_state (wh, WST_CONSTRUCT))
    {
        if (!widget_get_state (wh, WST_MODAL))
            dialog_switch_add (h);

        send_message (h, NULL, MSG_INIT, 0, NULL);
        group_send_broadcast_msg (g, MSG_INIT);
        dlg_read_history (h);
    }

    /* Select the first widget that takes focus */
    while (g->current != NULL && !widget_get_options (WIDGET (g->current->data), WOP_SELECTABLE)
           && !widget_get_state (WIDGET (g->current->data), WST_DISABLED))
        group_set_current_widget_next (g);

    widget_set_state (wh, WST_ACTIVE, TRUE);
    dlg_draw (h);
    /* focus found widget */
    if (g->current != NULL)
        widget_set_state (WIDGET (g->current->data), WST_FOCUSED, TRUE);

    h->ret_value = 0;
}

/* --------------------------------------------------------------------------------------------- */

void
dlg_process_event (WDialog * h, int key, Gpm_Event * event)
{
    switch (key)
    {
    case EV_NONE:
        if (tty_got_interrupt ())
            dlg_execute_cmd (h, CK_Cancel);
        break;

    case EV_MOUSE:
        h->mouse_status = dlg_mouse_event (h, event);
        break;

    default:
        dlg_key_event (h, key);
        break;
    }
}

/* --------------------------------------------------------------------------------------------- */
/** Shutdown the dlg_run */

void
dlg_run_done (WDialog * h)
{
    top_dlg = g_list_remove (top_dlg, h);

    if (widget_get_state (WIDGET (h), WST_CLOSED))
    {
        send_message (h, GROUP (h)->current == NULL ? NULL : WIDGET (GROUP (h)->current->data),
                      MSG_END, 0, NULL);
        if (!widget_get_state (WIDGET (h), WST_MODAL))
            dialog_switch_remove (h);
    }
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Standard run dialog routine
 * We have to keep this routine small so that we can duplicate it's
 * behavior on complex routines like the file routines, this way,
 * they can call the dlg_process_event without rewriting all the code
 */

int
dlg_run (WDialog * h)
{
    dlg_init (h);
    frontend_dlg_run (h);
    dlg_run_done (h);
    return h->ret_value;
}

/* --------------------------------------------------------------------------------------------- */

void
dlg_destroy (WDialog * h)
{
    WGroup *g = GROUP (h);

    /* if some widgets have history, save all history at one moment here */
    dlg_save_history (h);
    g_list_foreach (g->widgets, (GFunc) widget_destroy, NULL);
    g_list_free (g->widgets);
    mc_event_group_del (h->event_group);
    g_free (h->event_group);
    g_free (h->title);
    g_free (h);

    do_refresh ();
}

/* --------------------------------------------------------------------------------------------- */

/**
  * Write history to the ${XDG_CACHE_HOME}/mc/history file
  */
void
dlg_save_history (WDialog * h)
{
    char *profile;
    int i;

    if (num_history_items_recorded == 0)        /* this is how to disable */
        return;

    profile = mc_config_get_full_path (MC_HISTORY_FILE);
    i = open (profile, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (i != -1)
        close (i);

    /* Make sure the history is only readable by the user */
    if (chmod (profile, S_IRUSR | S_IWUSR) != -1 || errno == ENOENT)
    {
        ev_history_load_save_t event_data;

        event_data.cfg = mc_config_init (profile, FALSE);
        event_data.receiver = NULL;

        /* get all histories in dialog */
        mc_event_raise (h->event_group, MCEVENT_HISTORY_SAVE, &event_data);

        mc_config_save_file (event_data.cfg, NULL);
        mc_config_deinit (event_data.cfg);
    }

    g_free (profile);
}

/* --------------------------------------------------------------------------------------------- */

void
dlg_set_title (WDialog * h, const char *title)
{
    MC_PTR_FREE (h->title);

    /* Strip existing spaces, add one space before and after the title */
    if (title != NULL && title[0] != '\0')
    {
        char *t;

        t = g_strstrip (g_strdup (title));
        if (t[0] != '\0')
            h->title = g_strdup_printf (" %s ", t);
        g_free (t);
    }
}

/* --------------------------------------------------------------------------------------------- */

char *
dlg_get_title (const WDialog * h, size_t len)
{
    char *t;

    if (h == NULL)
        abort ();

    if (h->get_title != NULL)
        t = h->get_title (h, len);
    else
        t = g_strdup ("");

    return t;
}

/* --------------------------------------------------------------------------------------------- */
