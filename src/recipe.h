/*
    This file is part of Restraint.

    Restraint is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Restraint is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Restraint.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef _RESTRAINT_RECIPE_H
#define _RESTRAINT_RECIPE_H

#include <glib.h>
#include <libsoup/soup.h>
#include <libxml/tree.h>

#define RECIPE_FETCH_INTERVAL 10
#define RECIPE_FETCH_RETRIES 12

extern SoupSession *soup_session;

typedef enum {
    RECIPE_IDLE,
    RECIPE_FETCH,
    RECIPE_FETCHING,
    RECIPE_PARSE,
    RECIPE_RUN,
    RECIPE_RUNNING,
    RECIPE_FAIL,
    RECIPE_COMPLETE,
} RecipeSetupState;

typedef struct {
    gchar *recipe_id;
    gchar *job_id;
    gchar *recipe_set_id;
    gchar *osdistro;
    gchar *osmajor;
    gchar *osvariant;
    gchar *osarch;
    gchar *owner;
    gchar *base_path;
    GList *tasks; // list of Task *
    GList *parallel_tasks; // list of Task *
    GList *pre_tasks; // list of Pre_Task *
    GList *post_tasks; // list of Post_Task *
    GList *params; // list of Params
    GList *roles; // list of Roles
    SoupURI *recipe_uri;
} Recipe;

#define RESTRAINT_RECIPE_PARSE_ERROR restraint_recipe_parse_error_quark()
GQuark restraint_recipe_parse_error_quark(void);
typedef enum {
    RESTRAINT_RECIPE_PARSE_ERROR_UNRECOGNISED, /* xml structure not as we expected */
} RestraintRecipeParseError;

gboolean recipe_handler (gpointer user_data);
void restraint_recipe_parse_stream (GInputStream *stream, gpointer user_data);
void restraint_recipe_update_roles(Recipe *recipe, xmlDoc *doc, GError **error);
void restraint_recipe_free(Recipe *recipe);
void recipe_handler_finish (gpointer user_data);
gboolean recipe_wait_on_beaker (const gchar *recipe_url, const gchar *state_tag);
gboolean load_eval();
guint test_round;
void thread_loop_stop(gpointer user_data);

#endif
