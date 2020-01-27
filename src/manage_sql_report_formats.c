/* Copyright (C) 2020 Greenbone Networks GmbH
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file manage_sql_report_formats.c
 * @brief GVM management layer: Report format SQL
 *
 * The report format SQL for the GVM management layer.
 */

#include "manage_sql_report_formats.h"
#include "manage_acl.h"
#include "manage_report_formats.h"
#include "sql.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <grp.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gvm/base/proctitle.h>
#include <gvm/util/uuidutils.h>
#include <gvm/util/fileutils.h>

#undef G_LOG_DOMAIN
/**
 * @brief GLib log domain.
 */
#define G_LOG_DOMAIN "md manage"


/* Non-SQL internals defined in manage_report_formats.c. */

gchar *
predefined_report_format_dir (const gchar *);


/* Static headers. */

static int
validate_param_value (report_format_t, report_format_param_t param, const char *,
                      const char *);

static void
set_report_format_name (report_format_t, const char *);

static void
set_report_format_summary (report_format_t, const char *);

static void
set_report_format_active (report_format_t, int);

static int
set_report_format_param (report_format_t, const char *, const char *);


/* Helpers. */

/**
 * @brief Return the name of the sysconf GnuPG home directory
 *
 * Returns the name of the GnuPG home directory to use when checking
 * signatures.  It is the directory openvas/gnupg under the sysconfdir
 * that was set by configure (usually $prefix/etc).
 *
 * @return Static name of the Sysconf GnuPG home directory.
 */
static const char *
get_sysconf_gpghome ()
{
  static char *name;

  if (!name)
    name = g_build_filename (GVM_SYSCONF_DIR, "gnupg", NULL);

  return name;
}

/**
 * @brief Return the name of the trusted keys file name.
 *
 * We currently use the name pubring.gpg to be compatible with
 * previous installations.  That file should best be installed
 * read-only so that it is not accidentally accessed while we are
 * running a verification.  All files in that keyring are assumed to
 * be fully trustworthy.
 *
 * @return Static file name.
 */
static const char *
get_trustedkeys_name ()
{
  static char *name;

  if (!name)
    name = g_build_filename (get_sysconf_gpghome (), "pubring.gpg", NULL);

  return name;
}


/* Predefined resources.
 *
 * These are only used by report formats, and the concept is likely to change
 * when predefined report formats are defined by the feed. */

/**
 * @brief Return whether a resource is predefined.
 *
 * @param[in]  type      Type of resource.
 * @param[in]  resource  Resource.
 *
 * @return 1 if predefined, else 0.
 */
int
resource_predefined (const gchar *type, resource_t resource)
{
  assert (valid_type (type));
  return sql_int ("SELECT EXISTS (SELECT * FROM resources_predefined"
                  "               WHERE resource_type = '%s'"
                  "               AND resource = %llu);",
                  type,
                  resource);
}

/**
 * @brief Mark a resource as predefined.
 *
 * Currently only report formats use this.
 *
 * @param[in]  type      Resource type.
 * @param[in]  resource  Resource.
 * @param[in]  enable    If true mark as predefined, else remove mark.
 */
static void
resource_set_predefined (const gchar *type, resource_t resource, int enable)
{
  assert (valid_type (type));

  sql ("DELETE FROM resources_predefined"
       " WHERE resource_type = '%s'"
       " AND resource = %llu;",
       type,
       resource);

  if (enable)
    sql ("INSERT into resources_predefined (resource_type, resource)"
         " VALUES ('%s', %llu);",
         type,
         resource);
}


/* Signature utils. */

/**
 * @brief Execute gpg to verify an installer signature.
 *
 * @param[in]  installer       Installer.
 * @param[in]  installer_size  Size of installer.
 * @param[in]  signature       Installer signature.
 * @param[in]  signature_size  Size of installer signature.
 * @param[out] trust           Trust value.
 *
 * @return 0 success, -1 error.
 */
static int
verify_signature (const gchar *installer, gsize installer_size,
                  const gchar *signature, gsize signature_size,
                  int *trust)
{
  gchar **cmd;
  gint exit_status;
  int ret = 0, installer_fd, signature_fd;
  gchar *standard_out = NULL;
  gchar *standard_err = NULL;
  char installer_file[] = "/tmp/gvmd-installer-XXXXXX";
  char signature_file[] = "/tmp/gvmd-signature-XXXXXX";
  GError *error = NULL;

  installer_fd = mkstemp (installer_file);
  if (installer_fd == -1)
    return -1;

  g_file_set_contents (installer_file, installer, installer_size, &error);
  if (error)
    {
      g_warning ("%s", error->message);
      g_error_free (error);
      close (installer_fd);
      return -1;
    }

  signature_fd = mkstemp (signature_file);
  if (signature_fd == -1)
    {
      close (installer_fd);
      return -1;
    }

  g_file_set_contents (signature_file, signature, signature_size, &error);
  if (error)
    {
      g_warning ("%s", error->message);
      g_error_free (error);
      close (installer_fd);
      close (signature_fd);
      return -1;
    }

  cmd = (gchar **) g_malloc (10 * sizeof (gchar *));

  cmd[0] = g_strdup ("gpgv");
  cmd[1] = g_strdup ("--homedir");
  cmd[2] = g_strdup (get_sysconf_gpghome ());
  cmd[3] = g_strdup ("--quiet");
  cmd[4] = g_strdup ("--keyring");
  cmd[5] = g_strdup (get_trustedkeys_name ());
  cmd[6] = g_strdup ("--");
  cmd[7] = g_strdup (signature_file);
  cmd[8] = g_strdup (installer_file);
  cmd[9] = NULL;
  g_debug ("%s: Spawning in /tmp/: %s %s %s %s %s %s %s %s %s",
           __func__,
           cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5],
           cmd[6], cmd[7], cmd[8]);
  if ((g_spawn_sync ("/tmp/",
                     cmd,
                     NULL,                 /* Environment. */
                     G_SPAWN_SEARCH_PATH,
                     NULL,                 /* Setup func. */
                     NULL,
                     &standard_out,
                     &standard_err,
                     &exit_status,
                     NULL) == FALSE)
      || (WIFEXITED (exit_status) == 0)
      || WEXITSTATUS (exit_status))
    {
      if (WEXITSTATUS (exit_status) == 1)
        *trust = TRUST_NO;
      else
        {
#if 0
          g_debug ("%s: failed to run gpgv(%s): %d (WIF %i, WEX %i)",
                   __func__, get_trustedkeys_name (),
                   exit_status,
                   WIFEXITED (exit_status),
                   WEXITSTATUS (exit_status));
          g_debug ("%s: stdout: %s", __func__, standard_out);
          g_debug ("%s: stderr: %s", __func__, standard_err);
          ret = -1;
#endif
          /* This can be caused by the contents of the signature file, so
           * always return success. */
          *trust = TRUST_UNKNOWN;
        }
    }
  else
    *trust = TRUST_YES;

  g_free (cmd[0]);
  g_free (cmd[1]);
  g_free (cmd[2]);
  g_free (cmd[3]);
  g_free (cmd[4]);
  g_free (cmd[5]);
  g_free (cmd[6]);
  g_free (cmd[7]);
  g_free (cmd[8]);
  g_free (cmd);
  g_free (standard_out);
  g_free (standard_err);
  close (installer_fd);
  close (signature_fd);
  g_remove (installer_file);
  g_remove (signature_file);

  return ret;
}

/**
 * @brief Find a signature in a feed.
 *
 * @param[in]   location            Feed directory to search for signature.
 * @param[in]   installer_filename  Installer filename.
 * @param[out]  signature           Freshly allocated installer signature.
 * @param[out]  signature_size      Size of installer signature.
 * @param[out]  uuid                Address for basename of linked signature
 *                                  when the signature was found in the private
 *                                  directory, if desired, else NULL.  Private
 *                                  directory is only checked if this is given.
 *
 * @return 0 success, -1 error.
 */
static int
find_signature (const gchar *location, const gchar *installer_filename,
                gchar **signature, gsize *signature_size, gchar **uuid)
{
  gchar *installer_basename;

  installer_basename = g_path_get_basename (installer_filename);

  if (uuid)
    *uuid = NULL;

  if (strlen (installer_basename))
    {
      gchar *signature_filename, *signature_basename;
      GError *error = NULL;

      signature_basename  = g_strdup_printf ("%s.asc", installer_basename);
      g_free (installer_basename);
      signature_filename = g_build_filename (GVM_NVT_DIR,
                                             location,
                                             signature_basename,
                                             NULL);
      g_debug ("signature_filename: %s", signature_filename);

      g_file_get_contents (signature_filename, signature, signature_size,
                           &error);
      if (error)
        {
          if (uuid && (error->code == G_FILE_ERROR_NOENT))
            {
              char *real;
              gchar *real_basename;
              gchar **split;

              g_error_free (error);
              error = NULL;
              signature_filename = g_build_filename (GVMD_STATE_DIR,
                                                     "signatures",
                                                     location,
                                                     signature_basename,
                                                     NULL);
              g_debug ("signature_filename (private): %s", signature_filename);
              g_free (signature_basename);
              g_file_get_contents (signature_filename, signature, signature_size,
                                   &error);
              if (error)
                {
                  g_free (signature_filename);
                  g_error_free (error);
                  return -1;
                }

              real = realpath (signature_filename, NULL);
              g_free (signature_filename);
              g_debug ("real pathname: %s", real);
              if (real == NULL)
                return -1;
              real_basename = g_path_get_basename (real);
              split = g_strsplit (real_basename, ".", 2);
              if (*split)
                *uuid = g_strdup (*split);
              else
                *uuid = g_strdup (real_basename);
              g_debug ("*uuid: %s", *uuid);
              g_free (real_basename);
              g_strfreev (split);
              free (real);
              return 0;
            }
          else
            {
              g_debug ("%s: failed to read %s: %s", __func__,
                       signature_filename, error->message);
              g_free (signature_filename);
            }

          g_free (signature_basename);
          g_error_free (error);
          return -1;
        }
      g_free (signature_basename);
      return 0;
    }

  g_free (installer_basename);
  return -1;
}


/* Report formats. */

/**
 * @brief Possible port types.
 */
typedef enum
{
  REPORT_FORMAT_FLAG_ACTIVE = 1
} report_format_flag_t;

/**
 * @brief Get trash directory of a report format.
 *
 * @param[in]  report_format_id  UUID of report format.  NULL for the
 *             base dir that holds the report format trash.
 *
 * @return Freshly allocated trash dir.
 */
static gchar *
report_format_trash_dir (const gchar *report_format_id)
{
  if (report_format_id)
    return g_build_filename (GVMD_STATE_DIR,
                             "report_formats_trash",
                             report_format_id,
                             NULL);

  return g_build_filename (GVMD_STATE_DIR,
                           "report_formats_trash",
                           NULL);
}

/**
 * @brief Find a report format given a name.
 *
 * @param[in]   name           Name of report_format.
 * @param[out]  report_format  Report format return, 0 if successfully failed to
 *                             find report_format.
 *
 * @return FALSE on success (including if failed to find report format), TRUE
 *         on error.
 */
gboolean
lookup_report_format (const char* name, report_format_t* report_format)
{
  iterator_t report_formats;
  gchar *quoted_name;

  assert (report_format);

  *report_format = 0;
  quoted_name = sql_quote (name);
  init_iterator (&report_formats,
                 "SELECT id, uuid FROM report_formats"
                 " WHERE name = '%s'"
                 " AND CAST (flags & %llu AS boolean)"
                 " ORDER BY (CASE WHEN " ACL_USER_OWNS () " THEN 0"
                 "                WHEN owner is NULL THEN 1"
                 "                ELSE 2"
                 "           END);",
                 quoted_name,
                 (long long int) REPORT_FORMAT_FLAG_ACTIVE,
                 current_credentials.uuid);
  g_free (quoted_name);
  while (next (&report_formats))
    {
      const char *uuid;

      uuid = iterator_string (&report_formats, 1);
      if (uuid
          && acl_user_has_access_uuid ("report_format",
                                       uuid,
                                       "get_report_formats",
                                       0))
        {
          *report_format = iterator_int64 (&report_formats, 0);
          break;
        }
    }
  cleanup_iterator (&report_formats);

  return FALSE;
}

/**
 * @brief Compare files for create_report_format.
 *
 * @param[in]  one  First.
 * @param[in]  two  Second.
 *
 * @return Less than, equal to, or greater than zero if one is found to be
 *         less than, to match, or be greater than two.
 */
static gint
compare_files (gconstpointer one, gconstpointer two)
{
  gchar *file_one, *file_two;
  file_one = *((gchar**) one);
  file_two = *((gchar**) two);
  if (file_one == NULL)
    {
      if (file_two == NULL)
        return 0;
      return 1;
    }
  else if (file_two == NULL)
    return -1;
  return strcoll (file_one, file_two);
}

/**
 * @brief Create a report format.
 *
 * @param[in]   uuid           UUID of format.
 * @param[in]   name           Name of format.
 * @param[in]   content_type   Content type of format.
 * @param[in]   extension      File extension of format.
 * @param[in]   summary        Summary of format.
 * @param[in]   description    Description of format.
 * @param[in]   global         Whether the report is global.
 * @param[in]   files          Array of memory.  Each item is a file name
 *                             string, a terminating NULL, the file contents
 *                             in base64 and a terminating NULL.
 * @param[in]   params         Array of params.
 * @param[in]   params_options Array.  Each item is an array corresponding to
 *                             params.  Each item of an inner array is a string,
 *                             the text of an option in a selection.
 * @param[in]   signature      Signature.
 * @param[out]  report_format  Created report format.
 *
 * @return 0 success, 1 report format exists, 2 empty file name, 3 param value
 *         validation failed, 4 param value validation failed, 5 param default
 *         missing, 6 param min or max out of range, 7 param type missing,
 *         8 duplicate param name, 9 bogus param type name, 99 permission
 *         denied, -1 error.
 */
int
create_report_format (const char *uuid, const char *name,
                      const char *content_type, const char *extension,
                      const char *summary, const char *description, int global,
                      array_t *files, array_t *params, array_t *params_options,
                      const char *signature, report_format_t *report_format)
{
  gchar *quoted_name, *quoted_summary, *quoted_description, *quoted_extension;
  gchar *quoted_content_type, *quoted_signature, *file_name, *dir;
  gchar *candidate_name, *new_uuid, *uuid_actual;
  report_format_t report_format_rowid;
  int index, num;
  gchar *format_signature = NULL;
  gsize format_signature_size;
  int format_trust = TRUST_UNKNOWN;
  create_report_format_param_t *param;

  assert (current_credentials.uuid);
  assert (uuid);
  assert (name);
  assert (files);
  assert (params);

  /* Verify the signature. */

  if ((find_signature ("report_formats", uuid, &format_signature,
                       &format_signature_size, &uuid_actual)
       == 0)
      || signature)
    {
      char *locale;
      GString *format;

      format = g_string_new ("");

      g_string_append_printf (format,
                              "%s%s%s%i",
                              uuid_actual ? uuid_actual : uuid,
                              extension,
                              content_type,
                              global & 1);

      index = 0;
      locale = setlocale (LC_ALL, "C");
      g_ptr_array_sort (files, compare_files);
      setlocale (LC_ALL, locale);
      while ((file_name = (gchar*) g_ptr_array_index (files, index++)))
        g_string_append_printf (format,
                                "%s%s",
                                file_name,
                                file_name + strlen (file_name) + 1);

      index = 0;
      while ((param
               = (create_report_format_param_t*) g_ptr_array_index (params,
                                                                    index++)))
        {
          g_string_append_printf (format,
                                  "%s%s",
                                  param->name,
                                  param->type);

          if (param->type_min)
            {
              long long int min;
              min = strtoll (param->type_min, NULL, 0);
              if (min == LLONG_MIN)
                return 6;
              g_string_append_printf (format, "%lli", min);
            }

          if (param->type_max)
            {
              long long int max;
              max = strtoll (param->type_max, NULL, 0);
              if (max == LLONG_MAX)
                return 6;
              g_string_append_printf (format, "%lli", max);
            }

          g_string_append_printf (format,
                                  "%s",
                                  param->fallback);

          {
            array_t *options;
            int option_index;
            gchar *option_value;

            options = (array_t*) g_ptr_array_index (params_options, index - 1);
            if (options == NULL)
              return -1;
            option_index = 0;
            while ((option_value = (gchar*) g_ptr_array_index (options,
                                                               option_index++)))
              g_string_append_printf (format, "%s", option_value);
          }
        }

      g_string_append_printf (format, "\n");

      if (format_signature)
        signature = (const char*) format_signature;

      if (verify_signature (format->str, format->len, signature,
                            strlen (signature), &format_trust))
        {
          g_free (format_signature);
          g_string_free (format, TRUE);
          return -1;
        }
      g_string_free (format, TRUE);
    }

  sql_begin_immediate ();

  if (acl_user_may ("create_report_format") == 0)
    {
      sql_rollback ();
      return 99;
    }

  if (global && acl_user_can_everything (current_credentials.uuid) == 0)
    {
      sql_rollback ();
      return 99;
    }

  if (sql_int ("SELECT COUNT(*) FROM report_formats WHERE uuid = '%s';",
               uuid)
      || sql_int ("SELECT COUNT(*) FROM report_formats_trash"
                  " WHERE original_uuid = '%s';",
                  uuid))
    {
      gchar *base, *new, *old, *path;
      char *real_old;

      /* Make a new UUID, because a report format exists with the given UUID. */

      new_uuid = gvm_uuid_make ();
      if (new_uuid == NULL)
        {
          sql_rollback ();
          return -1;
        }

      /* Setup a private/report_formats/ link to the signature of the existing
       * report format in the feed.  This allows the signature to be shared. */

      base = g_strdup_printf ("%s.asc", uuid);
      old = g_build_filename (GVM_NVT_DIR, "report_formats", base, NULL);
      real_old = realpath (old, NULL);
      if (real_old)
        {
          /* Signature exists in regular directory. */

          g_free (old);
          old = g_strdup (real_old);
          free (real_old);
        }
      else
        {
          struct stat state;

          /* Signature may be in private directory. */

          g_free (old);
          old = g_build_filename (GVMD_STATE_DIR,
                                  "signatures",
                                  "report_formats",
                                  base,
                                  NULL);
          if (lstat (old, &state))
            {
              /* No.  Signature may not exist in the feed yet. */
              g_free (old);
              old = g_build_filename (GVM_NVT_DIR, "report_formats", base,
                                      NULL);
              g_debug ("using standard old: %s", old);
            }
          else
            {
              int count;

              /* Yes.  Use the path it links to. */

              real_old = g_malloc (state.st_size + 1);
              count = readlink (old, real_old, state.st_size + 1);
              if (count < 0 || count > state.st_size)
                {
                  g_free (real_old);
                  g_free (old);
                  g_warning ("%s: readlink failed", __func__);
                  sql_rollback ();
                  return -1;
                }

              real_old[state.st_size] = '\0';
              g_free (old);
              old = real_old;
              g_debug ("using linked old: %s", old);
            }
        }
      g_free (base);

      path = g_build_filename (GVMD_STATE_DIR,
                               "signatures", "report_formats", NULL);

      if (g_mkdir_with_parents (path, 0755 /* "rwxr-xr-x" */))
        {
          g_warning ("%s: failed to create dir %s: %s",
                     __func__, path, strerror (errno));
          g_free (old);
          g_free (path);
          sql_rollback ();
          return -1;
        }

      base = g_strdup_printf ("%s.asc", new_uuid);
      new = g_build_filename (path, base, NULL);
      g_free (path);
      g_free (base);
      if (symlink (old, new))
        {
          g_free (old);
          g_free (new);
          g_warning ("%s: symlink failed: %s", __func__, strerror (errno));
          sql_rollback ();
          return -1;
        }
    }
  else
    new_uuid = NULL;

  candidate_name = g_strdup (name);
  quoted_name = sql_quote (candidate_name);

  num = 1;
  while (1)
    {
      if (!resource_with_name_exists (quoted_name, "report_format", 0))
        break;
      g_free (candidate_name);
      g_free (quoted_name);
      candidate_name = g_strdup_printf ("%s %u", name, ++num);
      quoted_name = sql_quote (candidate_name);
    }
  g_free (candidate_name);

  /* Write files to disk. */

  assert (global == 0);
  if (global)
    dir = predefined_report_format_dir (new_uuid ? new_uuid : uuid);
  else
    {
      assert (current_credentials.uuid);
      dir = g_build_filename (GVMD_STATE_DIR,
                              "report_formats",
                              current_credentials.uuid,
                              new_uuid ? new_uuid : uuid,
                              NULL);
    }

  if (g_file_test (dir, G_FILE_TEST_EXISTS) && gvm_file_remove_recurse (dir))
    {
      g_warning ("%s: failed to remove dir %s", __func__, dir);
      g_free (dir);
      g_free (quoted_name);
      g_free (new_uuid);
      sql_rollback ();
      return -1;
    }

  if (g_mkdir_with_parents (dir, 0755 /* "rwxr-xr-x" */))
    {
      g_warning ("%s: failed to create dir %s: %s",
                 __func__, dir, strerror (errno));
      g_free (dir);
      g_free (quoted_name);
      g_free (new_uuid);
      sql_rollback ();
      return -1;
    }

  if (global == 0)
    {
      gchar *report_dir;

      /* glib seems to apply the mode to the first dir only. */

      report_dir = g_build_filename (GVMD_STATE_DIR,
                                     "report_formats",
                                     current_credentials.uuid,
                                     NULL);

      if (chmod (report_dir, 0755 /* rwxr-xr-x */))
        {
          g_warning ("%s: chmod failed: %s",
                     __func__,
                     strerror (errno));
          g_free (dir);
          g_free (report_dir);
          g_free (quoted_name);
          g_free (new_uuid);
          sql_rollback ();
          return -1;
        }

      g_free (report_dir);
    }

  /* glib seems to apply the mode to the first dir only. */
  if (chmod (dir, 0755 /* rwxr-xr-x */))
    {
      g_warning ("%s: chmod failed: %s",
                 __func__,
                 strerror (errno));
      g_free (dir);
      g_free (quoted_name);
      g_free (new_uuid);
      sql_rollback ();
      return -1;
    }

  index = 0;
  while ((file_name = (gchar*) g_ptr_array_index (files, index++)))
    {
      gchar *contents, *file, *full_file_name;
      gsize contents_size;
      GError *error;
      int ret;

      if (strlen (file_name) == 0)
        {
          gvm_file_remove_recurse (dir);
          g_free (dir);
          g_free (quoted_name);
          g_free (new_uuid);
          sql_rollback ();
          return 2;
        }

      file = file_name + strlen (file_name) + 1;
      if (strlen (file))
        contents = (gchar*) g_base64_decode (file, &contents_size);
      else
        {
          contents = g_strdup ("");
          contents_size = 0;
        }

      full_file_name = g_build_filename (dir, file_name, NULL);

      error = NULL;
      g_file_set_contents (full_file_name, contents, contents_size, &error);
      g_free (contents);
      if (error)
        {
          g_warning ("%s: %s", __func__, error->message);
          g_error_free (error);
          gvm_file_remove_recurse (dir);
          g_free (full_file_name);
          g_free (dir);
          g_free (quoted_name);
          g_free (new_uuid);
          sql_rollback ();
          return -1;
        }

      if (strcmp (file_name, "generate") == 0)
        ret = chmod (full_file_name, 0755 /* rwxr-xr-x */);
      else
        ret = chmod (full_file_name, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
      if (ret)
        {
          g_warning ("%s: chmod failed: %s",
                     __func__,
                     strerror (errno));
          gvm_file_remove_recurse (dir);
          g_free (full_file_name);
          g_free (dir);
          g_free (quoted_name);
          g_free (new_uuid);
          sql_rollback ();
          return -1;
        }

      g_free (full_file_name);
    }

  /* Add format to database. */

  quoted_summary = summary ? sql_quote (summary) : NULL;
  quoted_description = description ? sql_quote (description) : NULL;
  quoted_extension = extension ? sql_quote (extension) : NULL;
  quoted_content_type = content_type ? sql_quote (content_type) : NULL;
  quoted_signature = signature ? sql_quote (signature) : NULL;
  g_free (format_signature);

  if (global)
    sql ("INSERT INTO report_formats"
         " (uuid, name, owner, summary, description, extension, content_type,"
         "  signature, trust, trust_time, flags, creation_time,"
         "  modification_time)"
         " VALUES ('%s', '%s', NULL, '%s', '%s', '%s', '%s', '%s', %i, %i, 0,"
         "         m_now (), m_now ());",
         new_uuid ? new_uuid : uuid,
         quoted_name,
         quoted_summary ? quoted_summary : "",
         quoted_description ? quoted_description : "",
         quoted_extension ? quoted_extension : "",
         quoted_content_type ? quoted_content_type : "",
         quoted_signature ? quoted_signature : "",
         format_trust,
         time (NULL));
  else
    sql ("INSERT INTO report_formats"
         " (uuid, name, owner, summary, description, extension, content_type,"
         "  signature, trust, trust_time, flags, creation_time,"
         "  modification_time)"
         " VALUES ('%s', '%s',"
         " (SELECT id FROM users WHERE users.uuid = '%s'),"
         " '%s', '%s', '%s', '%s', '%s', %i, %i, 0, m_now (), m_now ());",
         new_uuid ? new_uuid : uuid,
         quoted_name,
         current_credentials.uuid,
         quoted_summary ? quoted_summary : "",
         quoted_description ? quoted_description : "",
         quoted_extension ? quoted_extension : "",
         quoted_content_type ? quoted_content_type : "",
         quoted_signature ? quoted_signature : "",
         format_trust,
         time (NULL));

  g_free (new_uuid);
  g_free (quoted_summary);
  g_free (quoted_description);
  g_free (quoted_extension);
  g_free (quoted_content_type);
  g_free (quoted_signature);
  g_free (quoted_name);

  /* Add params to database. */

  report_format_rowid = sql_last_insert_id ();
  index = 0;
  while ((param = (create_report_format_param_t*) g_ptr_array_index (params,
                                                                     index++)))
    {
      gchar *quoted_param_name, *quoted_param_value, *quoted_param_fallback;
      rowid_t param_rowid;
      long long int min, max;

      if (param->type == NULL)
        {
          gvm_file_remove_recurse (dir);
          g_free (dir);
          sql_rollback ();
          return 7;
        }

      if (report_format_param_type_from_name (param->type)
          == REPORT_FORMAT_PARAM_TYPE_ERROR)
        {
          gvm_file_remove_recurse (dir);
          g_free (dir);
          sql_rollback ();
          return 9;
        }

      /* Param min and max are optional.  LLONG_MIN and LLONG_MAX mark in the db
       * that they were missing, so if the user gives LLONG_MIN or LLONG_MAX it
       * is an error.  This ensures that GPG verification works, because the
       * verification knows when to leave out min and max. */

      if (param->type_min)
        {
          min = strtoll (param->type_min, NULL, 0);
          if (min == LLONG_MIN)
            {
              gvm_file_remove_recurse (dir);
              g_free (dir);
              sql_rollback ();
              return 6;
            }
        }
      else
        min = LLONG_MIN;

      if (param->type_max)
        {
          max = strtoll (param->type_max, NULL, 0);
          if (max == LLONG_MAX)
            {
              gvm_file_remove_recurse (dir);
              g_free (dir);
              sql_rollback ();
              return 6;
            }
        }
      else
        max = LLONG_MAX;

      if (param->fallback == NULL)
        {
          gvm_file_remove_recurse (dir);
          g_free (dir);
          sql_rollback ();
          return 5;
        }

      quoted_param_name = sql_quote (param->name);

      if (sql_int ("SELECT count(*) FROM report_format_params"
                   " WHERE name = '%s' AND report_format = %llu;",
                   quoted_param_name,
                   report_format_rowid))
        {
          g_free (quoted_param_name);
          gvm_file_remove_recurse (dir);
          g_free (dir);
          sql_rollback ();
          return 8;
        }

      quoted_param_value = sql_quote (param->value);
      quoted_param_fallback = sql_quote (param->fallback);

      sql ("INSERT INTO report_format_params"
           " (report_format, name, type, value, type_min, type_max, type_regex,"
           "  fallback)"
           " VALUES (%llu, '%s', %u, '%s', %lli, %lli, '', '%s');",
           report_format_rowid,
           quoted_param_name,
           report_format_param_type_from_name (param->type),
           quoted_param_value,
           min,
           max,
           quoted_param_fallback);

      g_free (quoted_param_name);
      g_free (quoted_param_value);
      g_free (quoted_param_fallback);

      param_rowid = sql_last_insert_id ();

      {
        array_t *options;
        int option_index;
        gchar *option_value;

        options = (array_t*) g_ptr_array_index (params_options, index - 1);
        if (options == NULL)
          {
            g_warning ("%s: options was NULL", __func__);
            gvm_file_remove_recurse (dir);
            g_free (dir);
            sql_rollback ();
            return -1;
          }
        option_index = 0;
        while ((option_value = (gchar*) g_ptr_array_index (options,
                                                           option_index++)))
          {
            gchar *quoted_option_value = sql_quote (option_value);
            sql ("INSERT INTO report_format_param_options"
                 " (report_format_param, value)"
                 " VALUES (%llu, '%s');",
                 param_rowid,
                 quoted_option_value);
            g_free (quoted_option_value);
          }
      }

      if (validate_param_value (report_format_rowid, param_rowid, param->name,
                                param->value))
        {
          gvm_file_remove_recurse (dir);
          g_free (dir);
          sql_rollback ();
          return 3;
        }

      if (validate_param_value (report_format_rowid, param_rowid, param->name,
                                param->fallback))
        {
          gvm_file_remove_recurse (dir);
          g_free (dir);
          sql_rollback ();
          return 4;
        }
    }

  if (report_format)
    *report_format = report_format_rowid;

  g_free (dir);

  sql_commit ();

  return 0;
}

/**
 * @brief Create Report Format from an existing Report Format.
 *
 * @param[in]  name                 Name of new Report Format. NULL to copy
 *                                  from existing.
 * @param[in]  source_uuid          UUID of existing Report Format.
 * @param[out] new_report_format    New Report Format.
 *
 * @return 0 success, 1 Report Format exists already, 2 failed to find existing
 *         Report Format, 99 permission denied, -1 error.
 */
int
copy_report_format (const char* name, const char* source_uuid,
                    report_format_t* new_report_format)
{
  report_format_t new, old;
  gchar *copy_uuid, *source_dir, *copy_dir;
  gchar *tmp_dir;
  int predefined, ret;

  assert (current_credentials.uuid);

  sql_begin_immediate ();

  ret = copy_resource_lock ("report_format", name, NULL, source_uuid,
                            "extension, content_type, summary, description,"
                            " signature, trust, trust_time, flags",
                            1, &new, &old);
  if (ret)
    {
      sql_rollback ();
      return ret;
    }

  if (report_format_predefined (old))
    sql ("UPDATE report_formats SET trust = %i, trust_time = %i"
         " WHERE id = %llu;",
         TRUST_YES,
         time (NULL),
         new);

  /* Copy report format parameters. */

  sql ("INSERT INTO report_format_params "
       " (report_format, name, type, value, type_min, type_max,"
       "  type_regex, fallback)"
       " SELECT %llu, name, type, value, type_min, type_max,"
       "  type_regex, fallback"
       "  FROM report_format_params WHERE report_format = %llu;",
       new,
       old);

  /* Copy files on disk. */

  predefined = report_format_predefined (old);
  if (predefined)
    source_dir = predefined_report_format_dir (source_uuid);
  else
    {
      gchar *owner_uuid;
      owner_uuid = report_format_owner_uuid (old);
      assert (owner_uuid);
      source_dir = g_build_filename (GVMD_STATE_DIR,
                                     "report_formats",
                                     owner_uuid,
                                     source_uuid,
                                     NULL);
      g_free (owner_uuid);
    }

  /* Check that the source directory exists. */

  if (!g_file_test (source_dir, G_FILE_TEST_EXISTS))
    {
      g_warning ("%s: report format directory %s not found",
                 __func__, source_dir);
      g_free (source_dir);
      sql_rollback ();
      return -1;
    }

  copy_uuid = report_format_uuid (new);
  if (copy_uuid == NULL)
    {
      sql_rollback ();
      return -1;
    }

  /* Prepare directory to copy into. */

  copy_dir = g_build_filename (GVMD_STATE_DIR,
                               "report_formats",
                               current_credentials.uuid,
                               copy_uuid,
                               NULL);

  if (g_file_test (copy_dir, G_FILE_TEST_EXISTS)
      && gvm_file_remove_recurse (copy_dir))
    {
      g_warning ("%s: failed to remove dir %s", __func__, copy_dir);
      g_free (source_dir);
      g_free (copy_dir);
      g_free (copy_uuid);
      sql_rollback ();
      return -1;
    }

  if (g_mkdir_with_parents (copy_dir, 0755 /* "rwxr-xr-x" */))
    {
      g_warning ("%s: failed to create dir %s", __func__, copy_dir);
      g_free (source_dir);
      g_free (copy_dir);
      g_free (copy_uuid);
      sql_rollback ();
      return -1;
    }

  /* Correct permissions as glib doesn't seem to do so. */

  tmp_dir = g_build_filename (GVMD_STATE_DIR,
                              "report_formats",
                              current_credentials.uuid,
                              NULL);

  if (chmod (tmp_dir, 0755 /* rwxr-xr-x */))
    {
      g_warning ("%s: chmod %s failed: %s",
                 __func__,
                 tmp_dir,
                 strerror (errno));
      g_free (source_dir);
      g_free (copy_dir);
      g_free (copy_uuid);
      g_free (tmp_dir);
      sql_rollback ();
      return -1;
    }
  g_free (tmp_dir);

  tmp_dir = g_build_filename (GVMD_STATE_DIR,
                              "report_formats",
                              current_credentials.uuid,
                              copy_uuid,
                              NULL);

  if (chmod (tmp_dir, 0755 /* rwxr-xr-x */))
    {
      g_warning ("%s: chmod %s failed: %s",
                 __func__,
                 tmp_dir,
                 strerror (errno));
      g_free (source_dir);
      g_free (copy_dir);
      g_free (copy_uuid);
      g_free (tmp_dir);
      sql_rollback ();
      return -1;
    }
  g_free (tmp_dir);
  g_free (copy_uuid);

  /* Copy files into new directory. */
  {
    GDir *directory;
    GError *error;

    error = NULL;
    directory = g_dir_open (source_dir, 0, &error);
    if (directory == NULL)
      {
        if (error)
          {
            g_warning ("g_dir_open(%s) failed - %s",
                       source_dir, error->message);
            g_error_free (error);
          }
        g_free (source_dir);
        g_free (copy_dir);
        sql_rollback ();
        return -1;
      }
    else
      {
        gchar *source_file, *copy_file;
        const gchar *filename;

        filename = g_dir_read_name (directory);
        while (filename)
          {
            source_file = g_build_filename (source_dir, filename, NULL);
            copy_file = g_build_filename (copy_dir, filename, NULL);

            if (gvm_file_copy (source_file, copy_file) == FALSE)
              {
                g_warning ("%s: copy of %s to %s failed",
                           __func__, source_file, copy_file);
                g_free (source_file);
                g_free (copy_file);
                g_free (source_dir);
                g_free (copy_dir);
                sql_rollback ();
                return -1;
              }
            g_free (source_file);
            g_free (copy_file);
            filename = g_dir_read_name (directory);
          }
      }
  }

  sql_commit ();
  g_free (source_dir);
  g_free (copy_dir);
  if (new_report_format) *new_report_format = new;
  return 0;
}

/**
 * @brief Modify a report format.
 *
 * @param[in]  report_format_id  UUID of report format.
 * @param[in]  name              Name of report format.
 * @param[in]  summary           Summary of report format.
 * @param[in]  active            Active flag.
 * @param[in]  param_name        Parameter to modify.
 * @param[in]  param_value       Value of parameter.
 * @param[in]  predefined        Predefined flag.
 *
 * @return 0 success, 1 failed to find report format, 2 report_format_id
 * required, 3 failed to find report format parameter, 4 parameter value
 * validation failed, 5 error in predefined, 99 permission denied, -1 internal
 * error.
 */
int
modify_report_format (const char *report_format_id, const char *name,
                      const char *summary, const char *active,
                      const char *param_name, const char *param_value,
                      const char *predefined)
{
  report_format_t report_format;
  int ret = 0;

  if (report_format_id == NULL)
    return 2;

  if (predefined && strcmp (predefined, "0") && strcmp (predefined, "1"))
    return 5;

  sql_begin_immediate ();

  assert (current_credentials.uuid);

  if (acl_user_may ("modify_report_format") == 0)
    {
      sql_rollback ();
      return 99;
    }

  report_format = 0;
  if (find_report_format_with_permission (report_format_id, &report_format,
                                          "modify_report_format"))
    {
      sql_rollback ();
      return -1;
    }

  if (report_format == 0)
    {
      sql_rollback ();
      return 1;
    }

  /* It is only possible to modify predefined report formats from the command
   * line. */
  if (current_credentials.uuid == NULL
      && report_format_predefined (report_format))
    {
      sql_rollback ();
      return 99;
    }

  /* Update values */
  if (name)
    set_report_format_name (report_format, name);

  if (summary)
    set_report_format_summary (report_format, summary);

  if (active)
    set_report_format_active (report_format, strcmp (active, "0"));

  if (predefined)
    resource_set_predefined ("report_format", report_format,
                             strcmp (predefined, "0"));

  sql_commit ();

  /* Update format params if set */
  if (param_name)
    {
      ret = set_report_format_param (report_format, param_name, param_value);
      if (ret == 1)
        ret = 3;
      if (ret == 2)
        ret = 4;
    }

  return ret;
}

/**
 * @brief Move a report format directory.
 *
 * @param[in]  dir      Old dir.
 * @param[in]  new_dir  New dir.
 *
 * @return 0 success, -1 error.
 */
static int
move_report_format_dir (const char *dir, const char *new_dir)
{
  if (g_file_test (dir, G_FILE_TEST_EXISTS)
      && gvm_file_check_is_dir (dir))
    {
      if (rename (dir, new_dir))
        {
          GError *error;
          GDir *directory;
          const gchar *entry;

          if (errno == EXDEV)
            {
              /* Across devices, move by hand. */

              if (g_mkdir_with_parents (new_dir, 0755 /* "rwxr-xr-x" */))
                {
                  g_warning ("%s: failed to create dir %s", __func__,
                             new_dir);
                  return -1;
                }

              error = NULL;
              directory = g_dir_open (dir, 0, &error);

              if (directory == NULL)
                {
                  g_warning ("%s: failed to g_dir_open %s: %s",
                             __func__, dir, error->message);
                  g_error_free (error);
                  return -1;
                }

              entry = NULL;
              while ((entry = g_dir_read_name (directory)))
                {
                  gchar *entry_path, *new_path;
                  entry_path = g_build_filename (dir, entry, NULL);
                  new_path = g_build_filename (new_dir, entry, NULL);
                  if (gvm_file_move (entry_path, new_path) == FALSE)
                    {
                      g_warning ("%s: failed to move %s to %s",
                                 __func__, entry_path, new_path);
                      g_free (entry_path);
                      g_free (new_path);
                      g_dir_close (directory);
                      return -1;
                    }
                  g_free (entry_path);
                  g_free (new_path);
                }

              g_dir_close (directory);

              gvm_file_remove_recurse (dir);
            }
          else
            {
              g_warning ("%s: rename %s to %s: %s",
                         __func__, dir, new_dir, strerror (errno));
              return -1;
            }
        }
    }
  else
    {
      g_warning ("%s: report dir missing: %s",
                 __func__, dir);
      return -1;
    }
  return 0;
}

/**
 * @brief Delete a report format from the db.
 *
 * @param[in]  report_format  Report format.
 */
static void
delete_report_format_rows (report_format_t report_format)
{
  sql ("DELETE FROM report_format_param_options WHERE report_format_param"
       " IN (SELECT id from report_format_params WHERE report_format = %llu);",
       report_format);
  sql ("DELETE FROM report_format_params WHERE report_format = %llu;",
       report_format);
  sql ("DELETE FROM report_formats WHERE id = %llu;", report_format);
}

/**
 * @brief Delete a report format.
 *
 * @param[in]  report_format_id  UUID of Report format.
 * @param[in]  ultimate          Whether to remove entirely, or to trashcan.
 *
 * @return 0 success, 1 report format in use, 2 failed to find report format,
 *         3 predefined report format, 99 permission denied, -1 error.
 */
int
delete_report_format (const char *report_format_id, int ultimate)
{
  gchar *dir;
  char *owner_uuid;
  report_format_t report_format, trash_report_format;

  /* This is complicated in two ways
   *
   *   - the UUID of a report format is the same every time it is
   *     imported, so to prevent multiple deletes from producing
   *     duplicate UUIDs in the trashcan, each report format in the
   *     trashcan gets a new UUID,
   *
   *   - the report format has information on disk on top of the
   *     info in the db, so the disk information has to be held
   *     in a special trashcan directory. */

  sql_begin_immediate ();

  if (acl_user_may ("delete_report_format") == 0)
    {
      sql_rollback ();
      return 99;
    }

  /* Look in the "real" table. */

  if (find_report_format_with_permission (report_format_id, &report_format,
                                          "delete_report_format"))
    {
      sql_rollback ();
      return -1;
    }

  if (report_format == 0)
    {
      gchar *report_format_string, *base;

      /* Look in the trashcan. */

      if (find_trash ("report_format", report_format_id, &report_format))
        {
          sql_rollback ();
          return -1;
        }
      if (report_format == 0)
        {
          sql_rollback ();
          return 2;
        }
      if (ultimate == 0)
        {
          /* It's already in the trashcan. */
          sql_commit ();
          return 0;
        }

      /* Check if it's in use by a trash alert. */

      if (trash_report_format_in_use (report_format))
        {
          sql_rollback ();
          return 1;
        }

      /* Remove entirely. */

      permissions_set_orphans ("report_format", report_format, LOCATION_TRASH);
      tags_remove_resource ("report_format", report_format, LOCATION_TRASH);

      base = sql_string ("SELECT original_uuid || '.asc'"
                         " FROM report_formats_trash"
                         " WHERE id = %llu;",
                         report_format);
      sql ("DELETE FROM report_format_param_options_trash"
           " WHERE report_format_param"
           " IN (SELECT id from report_format_params_trash"
           "     WHERE report_format = %llu);",
           report_format);
      sql ("DELETE FROM report_format_params_trash WHERE report_format = %llu;",
           report_format);
      sql ("DELETE FROM report_formats_trash WHERE id = %llu;",
           report_format);

      /* Remove the dirs last, in case any SQL rolls back. */

      /* Trash files. */
      report_format_string = g_strdup_printf ("%llu", report_format);
      dir = report_format_trash_dir (report_format_string);
      g_free (report_format_string);
      if (g_file_test (dir, G_FILE_TEST_EXISTS) && gvm_file_remove_recurse (dir))
        {
          g_free (dir);
          g_free (base);
          sql_rollback ();
          return -1;
        }
      g_free (dir);

      /* Links to the feed signatures. */
      dir = g_build_filename (GVMD_STATE_DIR, "signatures",
                              "report_formats", base, NULL);
      g_free (base);
      unlink (dir);
      g_free (dir);
      sql_commit ();

      return 0;
    }

  if (report_format_predefined (report_format))
    {
      sql_rollback ();
      return 3;
    }

  owner_uuid = report_format_owner_uuid (report_format);
  dir = g_build_filename (GVMD_STATE_DIR,
                          "report_formats",
                          owner_uuid,
                          report_format_id,
                          NULL);
  free (owner_uuid);

  if (ultimate)
    {
      permissions_set_orphans ("report_format", report_format, LOCATION_TABLE);
      tags_remove_resource ("report_format", report_format, LOCATION_TABLE);

      /* Check if it's in use by a trash or regular alert. */

      if (sql_int ("SELECT count(*) FROM alert_method_data_trash"
                   " WHERE data = (SELECT uuid FROM report_formats"
                   "               WHERE id = %llu)"
                   " AND (name = 'notice_attach_format'"
                   "      OR name = 'notice_report_format');",
                   report_format))
        {
          g_free (dir);
          sql_rollback ();
          return 1;
        }

      if (report_format_in_use (report_format))
        {
          g_free (dir);
          sql_rollback ();
          return 1;
        }

      /* Remove directory. */

      if (g_file_test (dir, G_FILE_TEST_EXISTS) && gvm_file_remove_recurse (dir))
        {
          g_free (dir);
          sql_rollback ();
          return -1;
        }

      /* Remove from "real" tables. */

      delete_report_format_rows (report_format);
    }
  else
    {
      iterator_t params;
      gchar *trash_dir, *new_dir, *report_format_string;

      /* Check if it's in use by a regular alert. */

      if (report_format_in_use (report_format))
        {
          g_free (dir);
          sql_rollback ();
          return 1;
        }

      /* Move to trash. */

      trash_dir = report_format_trash_dir (NULL);
      if (g_mkdir_with_parents (trash_dir, 0755 /* "rwxr-xr-x" */))
        {
          g_warning ("%s: failed to create dir %s", __func__, trash_dir);
          g_free (trash_dir);
          sql_rollback ();
          return -1;
        }
      g_free (trash_dir);

      sql ("INSERT INTO report_formats_trash"
           " (uuid, owner, name, extension, content_type, summary,"
           "  description, signature, trust, trust_time, flags, original_uuid,"
           "  creation_time, modification_time)"
           " SELECT"
           "  make_uuid (), owner, name, extension, content_type, summary,"
           "  description, signature, trust, trust_time, flags, uuid,"
           "  creation_time, modification_time"
           " FROM report_formats"
           " WHERE id = %llu;",
           report_format);

      trash_report_format = sql_last_insert_id ();

      init_report_format_param_iterator (&params, report_format, 0, 1, NULL);
      while (next (&params))
        {
          report_format_param_t param, trash_param;

          param = report_format_param_iterator_param (&params);

          sql ("INSERT INTO report_format_params_trash"
               " (report_format, name, type, value, type_min, type_max,"
               "  type_regex, fallback)"
               " SELECT"
               "  %llu, name, type, value, type_min, type_max,"
               "  type_regex, fallback"
               " FROM report_format_params"
               " WHERE id = %llu;",
               trash_report_format,
               param);

          trash_param = sql_last_insert_id ();

          sql ("INSERT INTO report_format_param_options_trash"
               " (report_format_param, value)"
               " SELECT %llu, value"
               " FROM report_format_param_options"
               " WHERE report_format_param = %llu;",
               trash_param,
               param);
        }
      cleanup_iterator (&params);

      permissions_set_locations ("report_format", report_format,
                                 trash_report_format, LOCATION_TRASH);
      tags_set_locations ("report_format", report_format,
                          trash_report_format, LOCATION_TRASH);

      /* Remove from "real" tables. */

      delete_report_format_rows (report_format);

      /* Move the dir last, in case any SQL rolls back. */

      report_format_string = g_strdup_printf ("%llu", trash_report_format);
      new_dir = report_format_trash_dir (report_format_string);
      g_free (report_format_string);
      if (move_report_format_dir (dir, new_dir))
        {
          g_free (dir);
          g_free (new_dir);
          sql_rollback ();
          return -1;
        }
      g_free (new_dir);
    }

  g_free (dir);

  sql_commit ();

  return 0;
}

/**
 * @brief Try restore a report format.
 *
 * If success, ends transaction for caller before exiting.
 *
 * @param[in]  report_format_id  UUID of resource.
 *
 * @return 0 success, 1 fail because resource is in use, 2 failed to find
 *         resource, 4 fail because resource with UUID exists, -1 error.
 */
int
restore_report_format (const char *report_format_id)
{
  report_format_t resource, report_format;
  iterator_t params;
  gchar *dir, *trash_dir, *resource_string;
  char *trash_uuid, *owner_uuid;

  if (find_trash ("report_format", report_format_id, &resource))
    {
      sql_rollback ();
      return -1;
    }

  if (resource == 0)
    return 2;

  if (sql_int ("SELECT count(*) FROM report_formats"
               " WHERE name ="
               " (SELECT name FROM report_formats_trash WHERE id = %llu)"
               " AND " ACL_USER_OWNS () ";",
               resource,
               current_credentials.uuid))
    {
      sql_rollback ();
      return 3;
    }

  if (sql_int ("SELECT count(*) FROM report_formats"
               " WHERE uuid = (SELECT original_uuid"
               "               FROM report_formats_trash"
               "               WHERE id = %llu);",
               resource))
    {
      sql_rollback ();
      return 4;
    }

  /* Move to "real" tables. */

  sql ("INSERT INTO report_formats"
       " (uuid, owner, name, extension, content_type, summary,"
       "  description, signature, trust, trust_time, flags,"
       "  creation_time, modification_time)"
       " SELECT"
       "  original_uuid, owner, name, extension, content_type, summary,"
       "  description, signature, trust, trust_time, flags,"
       "  creation_time, modification_time"
       " FROM report_formats_trash"
       " WHERE id = %llu;",
       resource);

  report_format = sql_last_insert_id ();

  init_report_format_param_iterator (&params, resource, 1, 1, NULL);
  while (next (&params))
    {
      report_format_param_t param, trash_param;

      trash_param = report_format_param_iterator_param (&params);

      sql ("INSERT INTO report_format_params"
           " (report_format, name, type, value, type_min, type_max,"
           "  type_regex, fallback)"
           " SELECT"
           "  %llu, name, type, value, type_min, type_max,"
           "  type_regex, fallback"
           " FROM report_format_params_trash"
           " WHERE id = %llu;",
           report_format,
           trash_param);

      param = sql_last_insert_id ();

      sql ("INSERT INTO report_format_param_options"
           " (report_format_param, value)"
           " SELECT %llu, value"
           " FROM report_format_param_options_trash"
           " WHERE report_format_param = %llu;",
           param,
           trash_param);
    }
  cleanup_iterator (&params);

  trash_uuid = sql_string ("SELECT original_uuid FROM report_formats_trash"
                           " WHERE id = %llu;",
                           resource);
  if (trash_uuid == NULL)
    abort ();

  permissions_set_locations ("report_format", resource, report_format,
                             LOCATION_TABLE);
  tags_set_locations ("report_format", resource, report_format,
                      LOCATION_TABLE);

  /* Remove from trash tables. */

  sql ("DELETE FROM report_format_param_options_trash"
       " WHERE report_format_param"
       " IN (SELECT id from report_format_params_trash"
       "     WHERE report_format = %llu);",
       resource);
  sql ("DELETE FROM report_format_params_trash WHERE report_format = %llu;",
       resource);
  sql ("DELETE FROM report_formats_trash WHERE id = %llu;",
       resource);

  /* Move the dir last, in case any SQL rolls back. */

  owner_uuid = report_format_owner_uuid (report_format);
  dir = g_build_filename (GVMD_STATE_DIR,
                          "report_formats",
                          owner_uuid,
                          trash_uuid,
                          NULL);
  free (trash_uuid);
  free (owner_uuid);

  resource_string = g_strdup_printf ("%llu", resource);
  trash_dir = report_format_trash_dir (resource_string);
  g_free (resource_string);
  if (move_report_format_dir (trash_dir, dir))
    {
      g_free (dir);
      g_free (trash_dir);
      sql_rollback ();
      return -1;
    }
  g_free (dir);
  g_free (trash_dir);

  sql_commit ();
  return 0;
}

/**
 * @brief Return the UUID of a report format.
 *
 * @param[in]  report_format  Report format.
 *
 * @return Newly allocated UUID.
 */
char *
report_format_uuid (report_format_t report_format)
{
  return sql_string ("SELECT uuid FROM report_formats WHERE id = %llu;",
                     report_format);
}

/**
 * @brief Return the UUID of the owner of a report format.
 *
 * @param[in]  report_format  Report format.
 *
 * @return Newly allocated owner UUID if there is an owner, else NULL.
 */
char *
report_format_owner_uuid (report_format_t report_format)
{
  if (sql_int ("SELECT " ACL_IS_GLOBAL () " FROM report_formats"
               " WHERE id = %llu;",
               report_format))
    return NULL;
  return sql_string ("SELECT uuid FROM users"
                     " WHERE id = (SELECT owner FROM report_formats"
                     "             WHERE id = %llu);",
                     report_format);
}

/**
 * @brief Set the active flag of a report format.
 *
 * @param[in]  report_format  The report format.
 * @param[in]  active         Active flag.
 */
static void
set_report_format_active (report_format_t report_format, int active)
{
  if (active)
    sql ("UPDATE report_formats SET flags = (flags | %llu), "
         "                          modification_time = m_now ()"
         " WHERE id = %llu;",
         (long long int) REPORT_FORMAT_FLAG_ACTIVE,
         report_format);
  else
    sql ("UPDATE report_formats SET flags = (flags & ~ %llu), "
         "                          modification_time = m_now ()"
         " WHERE id = %llu;",
         (long long int) REPORT_FORMAT_FLAG_ACTIVE,
         report_format);
}

/**
 * @brief Return the name of a report format.
 *
 * @param[in]  report_format  Report format.
 *
 * @return Newly allocated name.
 */
char *
report_format_name (report_format_t report_format)
{
  return sql_string ("SELECT name FROM report_formats WHERE id = %llu;",
                     report_format);
}

/**
 * @brief Return the content type of a report format.
 *
 * @param[in]  report_format  Report format.
 *
 * @return Newly allocated content type.
 */
char *
report_format_content_type (report_format_t report_format)
{
  return sql_string ("SELECT content_type FROM report_formats"
                     " WHERE id = %llu;",
                     report_format);
}

/**
 * @brief Return whether a report format is referenced by an alert.
 *
 * @param[in]  report_format  Report Format.
 *
 * @return 1 if in use, else 0.
 */
int
report_format_in_use (report_format_t report_format)
{
  return !!sql_int ("SELECT count(*) FROM alert_method_data"
                    " WHERE data = (SELECT uuid FROM report_formats"
                    "               WHERE id = %llu)"
                    " AND (name = 'notice_attach_format'"
                    "      OR name = 'notice_report_format'"
                    "      OR name = 'scp_report_format'"
                    "      OR name = 'send_report_format'"
                    "      OR name = 'smb_report_format'"
                    "      OR name = 'verinice_server_report_format');",
                    report_format);
}

/**
 * @brief Return whether a report format in trash is referenced by an alert.
 *
 * @param[in]  report_format  Report Format.
 *
 * @return 1 if in use, else 0.
 */
int
trash_report_format_in_use (report_format_t report_format)
{
  return !!sql_int ("SELECT count(*) FROM alert_method_data_trash"
                    " WHERE data = (SELECT original_uuid"
                    "               FROM report_formats_trash"
                    "               WHERE id = %llu)"
                    " AND (name = 'notice_attach_format'"
                    "      OR name = 'notice_report_format'"
                    "      OR name = 'scp_report_format'"
                    "      OR name = 'send_report_format'"
                    "      OR name = 'smb_report_format'"
                    "      OR name = 'verinice_server_report_format');",
                    report_format);
}

/**
 * @brief Return the extension of a report format.
 *
 * @param[in]  report_format  Report format.
 *
 * @return Newly allocated extension.
 */
char *
report_format_extension (report_format_t report_format)
{
  return sql_string ("SELECT extension FROM report_formats WHERE id = %llu;",
                     report_format);
}

/**
 * @brief Set the name of the report format.
 *
 * @param[in]  report_format  The report format.
 * @param[in]  name           Name.
 */
static void
set_report_format_name (report_format_t report_format, const char *name)
{
  gchar *quoted_name = sql_quote (name);
  sql ("UPDATE report_formats SET name = '%s', modification_time = m_now ()"
       " WHERE id = %llu;",
       quoted_name,
       report_format);
  g_free (quoted_name);
}

/**
 * @brief Return whether a report format is active.
 *
 * @param[in]  report_format  Report format.
 *
 * @return -1 on error, 1 if active, else 0.
 */
int
report_format_active (report_format_t report_format)
{
  long long int flag;
  switch (sql_int64 (&flag,
                     "SELECT flags & %llu FROM report_formats"
                     " WHERE id = %llu;",
                     (long long int) REPORT_FORMAT_FLAG_ACTIVE,
                     report_format))
    {
      case 0:
        break;
      case 1:        /* Too few rows in result of query. */
        return 0;
        break;
      default:       /* Programming error. */
        assert (0);
      case -1:
        return -1;
        break;
    }
  return flag ? 1 : 0;
}

/**
 * @brief Set the summary of the report format.
 *
 * @param[in]  report_format  The report format.
 * @param[in]  summary        Summary.
 */
static void
set_report_format_summary (report_format_t report_format, const char *summary)
{
  gchar *quoted_summary = sql_quote (summary);
  sql ("UPDATE report_formats SET summary = '%s', modification_time = m_now ()"
       " WHERE id = %llu;",
       quoted_summary,
       report_format);
  g_free (quoted_summary);
}

/**
 * @brief Return the type max of a report format param.
 *
 * @param[in]  report_format  Report format.
 * @param[in]  name           Name of param.
 *
 * @return Param type.
 */
static report_format_param_type_t
report_format_param_type (report_format_t report_format, const char *name)
{
  report_format_param_type_t type;
  gchar *quoted_name = sql_quote (name);
  type = (report_format_param_type_t)
         sql_int ("SELECT type FROM report_format_params"
                  " WHERE report_format = %llu AND name = '%s';",
                  report_format,
                  quoted_name);
  g_free (quoted_name);
  return type;
}

/**
 * @brief Return the type max of a report format param.
 *
 * @param[in]  report_format  Report format.
 * @param[in]  name           Name of param.
 *
 * @return Max.
 */
static long long int
report_format_param_type_max (report_format_t report_format, const char *name)
{
  long long int max = 0;
  gchar *quoted_name = sql_quote (name);
  /* Assume it's there. */
  sql_int64 (&max,
             "SELECT type_max FROM report_format_params"
             " WHERE report_format = %llu AND name = '%s';",
             report_format,
             quoted_name);
  g_free (quoted_name);
  return max;
}

/**
 * @brief Return the type min of a report format param.
 *
 * @param[in]  report_format  Report format.
 * @param[in]  name           Name of param.
 *
 * @return Min.
 */
static long long int
report_format_param_type_min (report_format_t report_format, const char *name)
{
  long long int min = 0;
  gchar *quoted_name = sql_quote (name);
  /* Assume it's there. */
  sql_int64 (&min,
             "SELECT type_min FROM report_format_params"
             " WHERE report_format = %llu AND name = '%s';",
             report_format,
             quoted_name);
  g_free (quoted_name);
  return min;
}

/**
 * @brief Validate a value for a report format param.
 *
 * @param[in]  report_format  Report format.
 * @param[in]  param          Param.
 * @param[in]  name           Name of param.
 * @param[in]  value          Potential value of param.
 *
 * @return 0 success, 1 fail.
 */
static int
validate_param_value (report_format_t report_format,
                      report_format_param_t param, const char *name,
                      const char *value)
{
  switch (report_format_param_type (report_format, name))
    {
      case REPORT_FORMAT_PARAM_TYPE_INTEGER:
        {
          long long int min, max, actual;
          min = report_format_param_type_min (report_format, name);
          /* Simply truncate out of range values. */
          actual = strtoll (value, NULL, 0);
          if (actual < min)
            return 1;
          max = report_format_param_type_max (report_format, name);
          if (actual > max)
            return 1;
        }
        break;
      case REPORT_FORMAT_PARAM_TYPE_SELECTION:
        {
          iterator_t options;
          int found = 0;

          init_param_option_iterator (&options, param, 1, NULL);
          while (next (&options))
            if (param_option_iterator_value (&options)
                && (strcmp (param_option_iterator_value (&options), value)
                    == 0))
              {
                found = 1;
                break;
              }
          cleanup_iterator (&options);
          if (found)
            break;
          return 1;
        }
      case REPORT_FORMAT_PARAM_TYPE_STRING:
      case REPORT_FORMAT_PARAM_TYPE_TEXT:
        {
          long long int min, max, actual;
          min = report_format_param_type_min (report_format, name);
          actual = strlen (value);
          if (actual < min)
            return 1;
          max = report_format_param_type_max (report_format, name);
          if (actual > max)
            return 1;
        }
        break;
      case REPORT_FORMAT_PARAM_TYPE_REPORT_FORMAT_LIST:
        {
          if (g_regex_match_simple
                ("^(?:[[:alnum:]-_]+)?(?:,(?:[[:alnum:]-_])+)*$", value, 0, 0)
              == FALSE)
            return 1;
          else
            return 0;
        }
        break;
      default:
        break;
    }
  return 0;
}

/**
 * @brief Set the value of the report format param.
 *
 * @param[in]  report_format  The report format.
 * @param[in]  name           Param name.
 * @param[in]  value_64       Param value in base64.
 *
 * @return 0 success, 1 failed to find param, 2 validation of value failed,
 *         -1 error.
 */
static int
set_report_format_param (report_format_t report_format, const char *name,
                         const char *value_64)
{
  gchar *quoted_name, *quoted_value, *value;
  gsize value_size;
  report_format_param_t param;

  quoted_name = sql_quote (name);

  sql_begin_immediate ();

  /* Ensure the param exists. */

  switch (sql_int64 (&param,
                     "SELECT id FROM report_format_params"
                     " WHERE name = '%s';",
                     quoted_name))
    {
      case 0:
        break;
      case 1:        /* Too few rows in result of query. */
        g_free (quoted_name);
        sql_rollback ();
        return 1;
        break;
      default:       /* Programming error. */
        assert (0);
      case -1:
        g_free (quoted_name);
        sql_rollback ();
        return -1;
        break;
    }

  /* Translate the value. */

  if (value_64 && strlen (value_64))
    value = (gchar*) g_base64_decode (value_64, &value_size);
  else
    {
      value = g_strdup ("");
      value_size = 0;
    }

  /* Validate the value. */

  if (validate_param_value (report_format, param, name, value))
    {
      sql_rollback ();
      g_free (quoted_name);
      return 2;
    }

  quoted_value = sql_quote (value);
  g_free (value);

  /* Update the database. */

  sql ("UPDATE report_format_params SET value = '%s'"
       " WHERE report_format = %llu AND name = '%s';",
       quoted_value,
       report_format,
       quoted_name);

  g_free (quoted_name);
  g_free (quoted_value);

  sql_commit ();

  return 0;
}

/**
 * @brief Return the trust of a report format.
 *
 * @param[in]  report_format  Report format.
 *
 * @return Trust: 1 yes, 2 no, 3 unknown.
 */
int
report_format_trust (report_format_t report_format)
{
  return sql_int ("SELECT trust FROM report_formats WHERE id = %llu;",
                  report_format);
}

/**
 * @brief Filter columns for Report Format iterator.
 */
#define REPORT_FORMAT_ITERATOR_FILTER_COLUMNS                                 \
 { ANON_GET_ITERATOR_FILTER_COLUMNS, "name", "extension", "content_type",     \
   "summary", "description", "trust", "trust_time", "active", NULL }

/**
 * @brief Report Format iterator columns.
 */
#define REPORT_FORMAT_ITERATOR_COLUMNS                                  \
 {                                                                      \
   { "id", NULL, KEYWORD_TYPE_INTEGER },                                \
   { "uuid", NULL, KEYWORD_TYPE_STRING },                               \
   { "name", NULL, KEYWORD_TYPE_STRING },                               \
   { "''", NULL, KEYWORD_TYPE_STRING },                                 \
   { "iso_time (creation_time)", NULL, KEYWORD_TYPE_STRING },           \
   { "iso_time (modification_time)", NULL, KEYWORD_TYPE_STRING },       \
   { "creation_time", "created", KEYWORD_TYPE_INTEGER },                \
   { "modification_time", "modified", KEYWORD_TYPE_INTEGER },           \
   {                                                                    \
     "(SELECT name FROM users WHERE users.id = report_formats.owner)",  \
     "_owner",                                                          \
     KEYWORD_TYPE_STRING                                                \
   },                                                                   \
   { "owner", NULL, KEYWORD_TYPE_INTEGER },                             \
   { "extension", NULL, KEYWORD_TYPE_STRING },                          \
   { "content_type", NULL, KEYWORD_TYPE_STRING },                       \
   { "summary", NULL, KEYWORD_TYPE_STRING },                            \
   { "description", NULL, KEYWORD_TYPE_STRING },                        \
   { "signature", NULL, KEYWORD_TYPE_STRING },                          \
   { "trust", NULL, KEYWORD_TYPE_INTEGER },                             \
   { "trust_time", NULL, KEYWORD_TYPE_INTEGER },                        \
   { "flags & 1", "active", KEYWORD_TYPE_INTEGER },                     \
   { NULL, NULL, KEYWORD_TYPE_UNKNOWN }                                 \
 }

/**
 * @brief Report Format iterator columns for trash case.
 */
#define REPORT_FORMAT_ITERATOR_TRASH_COLUMNS                            \
 {                                                                      \
   { "id", NULL, KEYWORD_TYPE_INTEGER },                                \
   { "uuid", NULL, KEYWORD_TYPE_STRING },                               \
   { "name", NULL, KEYWORD_TYPE_STRING },                               \
   { "''", NULL, KEYWORD_TYPE_STRING },                                 \
   { "iso_time (creation_time)", NULL, KEYWORD_TYPE_STRING },           \
   { "iso_time (modification_time)", NULL, KEYWORD_TYPE_STRING },       \
   { "creation_time", "created", KEYWORD_TYPE_INTEGER },                \
   { "modification_time", "modified", KEYWORD_TYPE_INTEGER },           \
   {                                                                    \
     "(SELECT name FROM users"                                          \
     " WHERE users.id = report_formats_trash.owner)",                   \
     "_owner",                                                          \
     KEYWORD_TYPE_STRING                                                \
   },                                                                   \
   { "owner", NULL, KEYWORD_TYPE_INTEGER },                             \
   { "extension", NULL, KEYWORD_TYPE_STRING },                          \
   { "content_type", NULL, KEYWORD_TYPE_STRING },                       \
   { "summary", NULL, KEYWORD_TYPE_STRING },                            \
   { "description", NULL, KEYWORD_TYPE_STRING },                        \
   { "signature", NULL, KEYWORD_TYPE_STRING },                          \
   { "trust", NULL, KEYWORD_TYPE_INTEGER },                             \
   { "trust_time", NULL, KEYWORD_TYPE_INTEGER },                        \
   { "flags & 1", "active", KEYWORD_TYPE_INTEGER },                     \
   { NULL, NULL, KEYWORD_TYPE_UNKNOWN }                                 \
 }

/**
 * @brief Get filter columns.
 *
 * @return Constant array of filter columns.
 */
const char**
report_format_filter_columns ()
{
  static const char *columns[] = REPORT_FORMAT_ITERATOR_FILTER_COLUMNS;
  return columns;
}

/**
 * @brief Get select columns.
 *
 * @return Constant array of select columns.
 */
column_t*
report_format_select_columns ()
{
  static column_t columns[] = REPORT_FORMAT_ITERATOR_COLUMNS;
  return columns;
}

/**
 * @brief Count the number of Report Formats.
 *
 * @param[in]  get  GET params.
 *
 * @return Total number of Report Formats filtered set.
 */
int
report_format_count (const get_data_t *get)
{
  static const char *filter_columns[] = REPORT_FORMAT_ITERATOR_FILTER_COLUMNS;
  static column_t columns[] = REPORT_FORMAT_ITERATOR_COLUMNS;
  static column_t trash_columns[] = REPORT_FORMAT_ITERATOR_TRASH_COLUMNS;
  return count ("report_format", get, columns, trash_columns, filter_columns,
                0, 0, 0, TRUE);
}

/**
 * @brief Initialise a Report Format iterator, including observed Report
 *        Formats.
 *
 * @param[in]  iterator    Iterator.
 * @param[in]  get         GET data.
 *
 * @return 0 success, 1 failed to find Report Format, 2 failed to find filter,
 *         -1 error.
 */
int
init_report_format_iterator (iterator_t* iterator, const get_data_t *get)
{
  static const char *filter_columns[] = REPORT_FORMAT_ITERATOR_FILTER_COLUMNS;
  static column_t columns[] = REPORT_FORMAT_ITERATOR_COLUMNS;
  static column_t trash_columns[] = REPORT_FORMAT_ITERATOR_TRASH_COLUMNS;

  return init_get_iterator (iterator,
                            "report_format",
                            get,
                            columns,
                            trash_columns,
                            filter_columns,
                            0,
                            NULL,
                            NULL,
                            TRUE);
}

/**
 * @brief Get the extension from a report format iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Extension, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (report_format_iterator_extension, GET_ITERATOR_COLUMN_COUNT);

/**
 * @brief Get the content type from a report format iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Content type, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (report_format_iterator_content_type, GET_ITERATOR_COLUMN_COUNT + 1);

/**
 * @brief Get the summary from a report format iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Summary, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (report_format_iterator_summary, GET_ITERATOR_COLUMN_COUNT + 2);

/**
 * @brief Get the description from a report format iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Description, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (report_format_iterator_description, GET_ITERATOR_COLUMN_COUNT + 3);

/**
 * @brief Get the signature from a report format iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Signature, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (report_format_iterator_signature, GET_ITERATOR_COLUMN_COUNT + 4);

/**
 * @brief Get the trust value from a report format iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Trust value.
 */
const char*
report_format_iterator_trust (iterator_t* iterator)
{
  if (iterator->done) return NULL;
  switch (iterator_int (iterator, GET_ITERATOR_COLUMN_COUNT + 5))
    {
      case 1:  return "yes";
      case 2:  return "no";
      case 3:  return "unknown";
      default: return NULL;
    }
}

/**
 * @brief Get the trust time from a report format iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Time report format was verified.
 */
time_t
report_format_iterator_trust_time (iterator_t* iterator)
{
  int ret;
  if (iterator->done) return -1;
  ret = (time_t) iterator_int (iterator, GET_ITERATOR_COLUMN_COUNT + 6);
  return ret;
}

/**
 * @brief Get the active flag from a report format iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Active flag, or -1 if iteration is complete.
 */
int
report_format_iterator_active (iterator_t* iterator)
{
  if (iterator->done) return -1;
  return (iterator_int64 (iterator, GET_ITERATOR_COLUMN_COUNT + 7)
          & REPORT_FORMAT_FLAG_ACTIVE) ? 1 : 0;
}

/**
 * @brief Initialise a Report Format alert iterator.
 *
 * Iterates over all alerts that use the Report Format.
 *
 * @param[in]  iterator          Iterator.
 * @param[in]  report_format     Report Format.
 */
void
init_report_format_alert_iterator (iterator_t* iterator,
                                   report_format_t report_format)
{
  gchar *available, *with_clause;
  get_data_t get;
  array_t *permissions;

  assert (report_format);

  get.trash = 0;
  permissions = make_array ();
  array_add (permissions, g_strdup ("get_alerts"));
  available = acl_where_owned ("alert", &get, 1, "any", 0, permissions,
                               &with_clause);
  array_free (permissions);

  init_iterator (iterator,
                 "%s"
                 " SELECT DISTINCT alerts.name, alerts.uuid, %s"
                 " FROM alerts, alert_method_data"
                 " WHERE alert_method_data.data = '%s'"
                 " AND alert_method_data.alert = alerts.id"
                 " ORDER BY alerts.name ASC;",
                 with_clause ? with_clause : "",
                 available,
                 report_format_uuid (report_format));

  g_free (with_clause);
  g_free (available);
}

/**
 * @brief Get the name from a report_format_alert iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The name of the Report Format, or NULL if iteration is complete.
 *         Freed by cleanup_iterator.
 */
DEF_ACCESS (report_format_alert_iterator_name, 0);

/**
 * @brief Get the UUID from a report_format_alert iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The UUID of the Report Format, or NULL if iteration is complete.
 *         Freed by cleanup_iterator.
 */
DEF_ACCESS (report_format_alert_iterator_uuid, 1);

/**
 * @brief Get the read permission status from a GET iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return 1 if may read, else 0.
 */
int
report_format_alert_iterator_readable (iterator_t* iterator)
{
  if (iterator->done) return 0;
  return iterator_int (iterator, 2);
}

/**
 * @brief Initialise a report format iterator.
 *
 * @param[in]  iterator       Iterator.
 * @param[in]  report_format  Single report_format to iterate over, or 0 for all.
 * @param[in]  trash          Whether to iterate over trashcan report formats.
 * @param[in]  ascending      Whether to sort ascending or descending.
 * @param[in]  sort_field     Field to sort on, or NULL for "id".
 */
void
init_report_format_param_iterator (iterator_t* iterator,
                                   report_format_t report_format,
                                   int trash,
                                   int ascending,
                                   const char* sort_field)
{
  if (report_format)
    init_iterator (iterator,
                   "SELECT id, name, value, type, type_min, type_max,"
                   " type_regex, fallback"
                   " FROM report_format_params%s"
                   " WHERE report_format = %llu"
                   " ORDER BY %s %s;",
                   trash ? "_trash" : "",
                   report_format,
                   sort_field ? sort_field : "id",
                   ascending ? "ASC" : "DESC");
  else
    init_iterator (iterator,
                   "SELECT id, name, value, type, type_min, type_max,"
                   " type_regex, fallback"
                   " FROM report_format_params%s"
                   " ORDER BY %s %s;",
                   trash ? "_trash" : "",
                   sort_field ? sort_field : "id",
                   ascending ? "ASC" : "DESC");
}

/**
 * @brief Get the report format param from a report format param iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Report format param.
 */
report_format_param_t
report_format_param_iterator_param (iterator_t* iterator)
{
  if (iterator->done) return 0;
  return (report_format_param_t) iterator_int64 (iterator, 0);
}

/**
 * @brief Get the name from a report format param iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Name, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (report_format_param_iterator_name, 1);

/**
 * @brief Get the value from a report format param iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Value, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (report_format_param_iterator_value, 2);

/**
 * @brief Get the name of the type of a report format param iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Static string naming type, or NULL if iteration is complete.
 */
const char *
report_format_param_iterator_type_name (iterator_t* iterator)
{
  if (iterator->done) return NULL;
  return report_format_param_type_name (iterator_int (iterator, 3));
}

/**
 * @brief Get the type from a report format param iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Type.
 */
report_format_param_type_t
report_format_param_iterator_type (iterator_t* iterator)
{
  if (iterator->done) return -1;
  return iterator_int (iterator, 3);
}

/**
 * @brief Get the type min from a report format param iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Type min.
 */
long long int
report_format_param_iterator_type_min (iterator_t* iterator)
{
  if (iterator->done) return -1;
  return iterator_int64 (iterator, 4);
}

/**
 * @brief Get the type max from a report format param iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Type max.
 */
long long int
report_format_param_iterator_type_max (iterator_t* iterator)
{
  if (iterator->done) return -1;
  return iterator_int64 (iterator, 5);
}

/**
 * @brief Get the type regex from a report format param iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Type regex, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
static
DEF_ACCESS (report_format_param_iterator_type_regex, 6);

/**
 * @brief Get the default from a report format param iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Default, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (report_format_param_iterator_fallback, 7);

/**
 * @brief Initialise a report format param option iterator.
 *
 * @param[in]  iterator             Iterator.
 * @param[in]  report_format_param  Param whose options to iterate over.
 * @param[in]  ascending            Whether to sort ascending or descending.
 * @param[in]  sort_field           Field to sort on, or NULL for "id".
 */
void
init_param_option_iterator (iterator_t* iterator,
                            report_format_param_t report_format_param,
                            int ascending, const char *sort_field)
{
  init_iterator (iterator,
                 "SELECT id, value"
                 " FROM report_format_param_options"
                 " WHERE report_format_param = %llu"
                 " ORDER BY %s %s;",
                 report_format_param,
                 sort_field ? sort_field : "id",
                 ascending ? "ASC" : "DESC");
}

/**
 * @brief Get the value from a report format param option iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Value, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (param_option_iterator_value, 1);

/**
 * @brief Create or update report format for check_report_format.
 *
 * @param[in]  quoted_uuid    UUID of report format, quoted for SQL.
 * @param[in]  name           Name.
 * @param[in]  summary        Summary.
 * @param[in]  description    Description.
 * @param[in]  extension      Extension.
 * @param[in]  content_type   Content type.
 * @param[out] report_format  Created report format.
 *
 * @return 0 success, -1 error.
 */
static int
check_report_format_create (const gchar *quoted_uuid, const gchar *name,
                            const gchar *summary, const gchar *description,
                            const gchar *extension, const gchar *content_type,
                            report_format_t *report_format)
{
  gchar *quoted_name, *quoted_summary, *quoted_description;
  gchar *quoted_extension, *quoted_content_type;

  quoted_name = sql_quote (name);
  quoted_summary = sql_quote (summary);
  quoted_description = sql_quote (description);
  quoted_extension = sql_quote (extension);
  quoted_content_type = sql_quote (content_type);

  if (sql_int ("SELECT count (*) FROM report_formats WHERE uuid = '%s';",
               quoted_uuid))
    {
      sql ("UPDATE report_formats"
           " SET owner = NULL, name = '%s', summary = '%s', description = '%s',"
           "     extension = '%s', content_type = '%s', signature = '',"
           "     trust = %i, trust_time = %i, flags = %llu"
           " WHERE uuid = '%s';",
           g_strstrip (quoted_name),
           g_strstrip (quoted_summary),
           g_strstrip (quoted_description),
           g_strstrip (quoted_extension),
           g_strstrip (quoted_content_type),
           TRUST_YES,
           time (NULL),
           (long long int) REPORT_FORMAT_FLAG_ACTIVE,
           quoted_uuid);

      sql ("UPDATE report_formats SET modification_time = m_now ()"
           " WHERE id"
           " IN (SELECT report_formats.id"
           "     FROM report_formats, report_formats_check"
           "     WHERE report_formats.uuid = '%s'"
           "     AND report_formats.id = report_formats_check.id"
           "     AND (report_formats.owner != report_formats_check.owner"
           "          OR report_formats.name != report_formats_check.name"
           "          OR report_formats.summary != report_formats_check.summary"
           "          OR report_formats.description"
           "             != report_formats_check.description"
           "          OR report_formats.extension"
           "             != report_formats_check.extension"
           "          OR report_formats.content_type"
           "             != report_formats_check.content_type"
           "          OR report_formats.trust != report_formats_check.trust"
           "          OR report_formats.flags != report_formats_check.flags));",
           quoted_uuid);
    }
  else
    sql ("INSERT INTO report_formats"
         " (uuid, name, owner, summary, description, extension, content_type,"
         "  signature, trust, trust_time, flags, creation_time,"
         "  modification_time)"
         " VALUES ('%s', '%s', NULL, '%s', '%s', '%s', '%s', '', %i, %i, %i,"
         "         m_now (), m_now ());",
         quoted_uuid,
         g_strstrip (quoted_name),
         g_strstrip (quoted_summary),
         g_strstrip (quoted_description),
         g_strstrip (quoted_extension),
         g_strstrip (quoted_content_type),
         TRUST_YES,
         time (NULL),
         (long long int) REPORT_FORMAT_FLAG_ACTIVE);

  add_role_permission_resource (ROLE_UUID_ADMIN, "GET_REPORT_FORMATS",
                                "report_format", quoted_uuid);
  add_role_permission_resource (ROLE_UUID_GUEST, "GET_REPORT_FORMATS",
                                "report_format", quoted_uuid);
  add_role_permission_resource (ROLE_UUID_OBSERVER, "GET_REPORT_FORMATS",
                                "report_format", quoted_uuid);
  add_role_permission_resource (ROLE_UUID_USER, "GET_REPORT_FORMATS",
                                "report_format", quoted_uuid);

  g_free (quoted_name);
  g_free (quoted_summary);
  g_free (quoted_description);
  g_free (quoted_extension);
  g_free (quoted_content_type);

  switch (sql_int64 (report_format,
                     "SELECT id FROM report_formats WHERE uuid = '%s';",
                     quoted_uuid))
    {
      case 0:
        break;
      default:       /* Programming error. */
        assert (0);
      case 1:        /* Too few rows in result of query. */
      case -1:
        g_warning ("%s: Report format missing: %s",
                   __func__, quoted_uuid);
        return -1;
    }

  resource_set_predefined ("report_format", *report_format, 1);

  return 0;
}

/**
 * @brief Add params for check_report_format.
 *
 * @param[in]  quoted_uuid      UUID of report format, quoted.
 * @param[in]  config_path      Config path.
 * @param[in]  entity           Parsed XML.
 * @param[out] update_mod_time  Whether to update modification time.
 *
 * @return 0 success, -1 error.
 */
static int
check_report_format_add_params (const gchar *quoted_uuid, const gchar *config_path,
                                entity_t entity, int *update_mod_time)
{
  entities_t entities;
  entity_t param;

  entities = entity->entities;
  while ((param = first_entity (entities)))
    {
      g_debug ("%s: possible param: %s", __func__, entity_name (param));

      if (strcmp (entity_name (param), "param") == 0)
        {
          const char *name, *value, *fallback;
          gchar *quoted_name, *quoted_value, *quoted_fallback, *type;
          const char *min, *max;
          array_t *opts;
          entity_t child;

          opts = NULL;
          min = max = NULL;

          child = entity_child (param, "name");
          if (child == NULL)
            {
              g_warning ("%s: Param missing name in '%s'",
                         __func__, config_path);
              return -1;
            }
          name = entity_text (child);

          child = entity_child (param, "default");
          if (child == NULL)
            {
              g_warning ("%s: Param missing default in '%s'",
                         __func__, config_path);
              return -1;
            }
          fallback = entity_text (child);

          child = entity_child (param, "type");
          if (child == NULL)
            {
              g_warning ("%s: Param missing type in '%s'",
                         __func__, config_path);
              return -1;
            }
          type = g_strstrip (g_strdup (entity_text (child)));
          if (report_format_param_type_from_name (type)
              == REPORT_FORMAT_PARAM_TYPE_ERROR)
            {
              g_warning ("%s: Error in param type in '%s'",
                         __func__, config_path);
              return -1;
            }

          if (strcmp (type, "report_format_list"))
            {
              entity_t bound;

              bound = entity_child (child, "min");
              if (bound && strlen (entity_text (bound)))
                {
                  long long int number;
                  char *end;

                  min = entity_text (bound);
                  number = strtoll (min, &end, 0);
                  if (*end != '\0'
                      || number == LLONG_MAX
                      || number == LLONG_MIN)
                    {
                      g_warning ("%s: Failed to parse min in '%s'",
                                 __func__, config_path);
                      g_free (type);
                      return -1;
                    }
                }

              bound = entity_child (child, "max");
              if (bound && strlen (entity_text (bound)))
                {
                  long long int number;
                  char *end;

                  max = entity_text (bound);
                  number = strtoll (max, &end, 0);
                  if (*end != '\0'
                      || number == LLONG_MAX
                      || number == LLONG_MIN)
                    {
                      g_warning ("%s: Failed to parse max in '%s'",
                                 __func__, config_path);
                      g_free (type);
                      return -1;
                    }
                }

              if (strcmp (type, "selection") == 0)
                {
                  entity_t options, option;
                  entities_t children;

                  options = entity_child (child, "options");
                  if (options == NULL)
                    {
                      g_warning ("%s: Selection missing options in '%s'",
                                 __func__, config_path);
                      g_free (type);
                      return -1;
                    }

                  children = options->entities;
                  opts = make_array ();
                  while ((option = first_entity (children)))
                    {
                      array_add (opts, entity_text (option));
                      children = next_entities (children);
                    }
                }

              child = entity_child (param, "value");
              if (child == NULL)
                {
                  g_warning ("%s: Param missing value in '%s'",
                             __func__, config_path);
                  g_free (type);
                  return -1;
                }
              value = entity_text (child);

            }
          else
            {
              entity_t report_format;

              child = entity_child (param, "value");
              if (child == NULL)
                {
                  g_warning ("%s: Param missing value in '%s'",
                             __func__, config_path);
                  g_free (type);
                  return -1;
                }

              report_format = entity_child (child, "report_format");
              if (report_format == NULL)
                {
                  g_warning ("%s: Param missing report format in '%s'",
                             __func__, config_path);
                  g_free (type);
                  return -1;
                }

              value = entity_attribute (report_format, "id");
              if (value == NULL)
                {
                  g_warning ("%s: Report format missing id in '%s'",
                             __func__, config_path);
                  g_free (type);
                  return -1;
                }
            }

          /* Add or update the param. */

          quoted_name = g_strstrip (sql_quote (name));
          quoted_value = g_strstrip (sql_quote (value));
          quoted_fallback = g_strstrip (sql_quote (fallback));

          g_debug ("%s: param: %s", __func__, name);

          if (sql_int ("SELECT count (*) FROM report_format_params"
                       " WHERE name = '%s'"
                       " AND report_format = (SELECT id FROM report_formats"
                       "                      WHERE uuid = '%s');",
                       quoted_name,
                       quoted_uuid))
            {
              g_debug ("%s: param: %s: updating", __func__, name);

              sql ("UPDATE report_format_params"
                   " SET type = %u, value = '%s', type_min = %s,"
                   "     type_max = %s, type_regex = '', fallback = '%s'"
                   " WHERE name = '%s'"
                   " AND report_format = (SELECT id FROM report_formats"
                   "                      WHERE uuid = '%s');",
                   report_format_param_type_from_name (type),
                   quoted_value,
                   min ? min : "NULL",
                   max ? max : "NULL",
                   quoted_fallback,
                   quoted_name,
                   quoted_uuid);

               /* If any value changed, update the modification time. */

               if (sql_int
                    ("SELECT"
                     " EXISTS"
                     "  (SELECT *"
                     "   FROM report_format_params,"
                     "        report_format_params_check"
                     "   WHERE report_format_params.name = '%s'"
                     "   AND report_format_params_check.name = '%s'"
                     "   AND report_format_params.report_format"
                     "       = report_format_params_check.report_format"
                     "   AND (report_format_params.type"
                     "        != report_format_params_check.type"
                     "        OR report_format_params.value"
                     "           != report_format_params_check.value"
                     "        OR report_format_params.type_min"
                     "           != report_format_params_check.type_min"
                     "        OR report_format_params.type_max"
                     "           != report_format_params_check.type_max"
                     "        OR report_format_params.fallback"
                     "           != report_format_params_check.fallback));",
                     quoted_name,
                     quoted_name))
                 *update_mod_time = 1;

              /* Delete existing param options.
               *
               * Predefined report formats can't be modified so the options
               * don't really matter, so don't worry about them for updating
               * the modification time. */

              sql ("DELETE FROM report_format_param_options"
                   " WHERE report_format_param"
                   "       IN (SELECT id FROM report_format_params"
                   "           WHERE name = '%s'"
                   "           AND report_format = (SELECT id"
                   "                                FROM report_formats"
                   "                                WHERE uuid = '%s'));",
                   quoted_name,
                   quoted_uuid);
            }
          else
            {
              g_debug ("%s: param: %s: creating", __func__, name);

              sql ("INSERT INTO report_format_params"
                   " (report_format, name, type, value, type_min, type_max,"
                   "  type_regex, fallback)"
                   " VALUES"
                   " ((SELECT id FROM report_formats WHERE uuid = '%s'),"
                   "  '%s', %u, '%s', %s, %s, '', '%s');",
                   quoted_uuid,
                   quoted_name,
                   report_format_param_type_from_name (type),
                   quoted_value,
                   min ? min : "NULL",
                   max ? max : "NULL",
                   quoted_fallback);
              *update_mod_time = 1;
            }

          g_free (type);

          /* Keep this param. */

          sql ("DELETE FROM report_format_params_check"
               " WHERE report_format = (SELECT id FROM report_formats"
               "                        WHERE uuid = '%s')"
               " AND name = '%s';",
               quoted_uuid,
               quoted_name);

          /* Add any options. */

          if (opts)
            {
              int index;

              index = 0;
              while (opts && (index < opts->len))
                {
                  gchar *quoted_option;
                  quoted_option = sql_quote (g_ptr_array_index (opts, index++));
                  sql ("INSERT INTO report_format_param_options"
                       " (report_format_param, value)"
                       " VALUES ((SELECT id FROM report_format_params"
                       "          WHERE name = '%s'"
                       "          AND report_format = (SELECT id"
                       "                               FROM report_formats"
                       "                               WHERE uuid = '%s')),"
                       "         '%s');",
                       quoted_name,
                       quoted_uuid,
                       quoted_option);
                  g_free (quoted_option);
                }

              /* array_free would try free the elements too. */
              g_ptr_array_free (opts, TRUE);
            }

          g_free (quoted_name);
          g_free (quoted_value);
          g_free (quoted_fallback);
        }
      entities = next_entities (entities);
    }

  return 0;
}

/**
 * @brief Setup a predefined report format from disk.
 *
 * @param[in]  entity        XML.
 * @param[in]  config_path   Config path.
 * @param[in]  name          Name.
 * @param[in]  summary       Summary.
 * @param[in]  description   Description.
 * @param[in]  extension     Extension.
 * @param[in]  content_type  Content type.
 *
 * @return 0 success, -1 error.
 */
static int
check_report_format_parse (entity_t entity, const char *config_path,
                           const char **name, const char **summary,
                           const char **description, const char **extension,
                           const char **content_type)
{
  entity_t child;

  child = entity_child (entity, "name");
  if (child == NULL)
    {
      g_warning ("%s: Missing name in '%s'", __func__, config_path);
      return -1;
    }
  *name = entity_text (child);

  child = entity_child (entity, "summary");
  if (child == NULL)
    {
      g_warning ("%s: Missing summary in '%s'", __func__, config_path);
      return -1;
    }
  *summary = entity_text (child);

  child = entity_child (entity, "description");
  if (child == NULL)
    {
      g_warning ("%s: Missing description in '%s'",
                 __func__, config_path);
      return -1;
    }
  *description = entity_text (child);

  child = entity_child (entity, "extension");
  if (child == NULL)
    {
      g_warning ("%s: Missing extension in '%s'", __func__, config_path);
      return -1;
    }
  *extension = entity_text (child);

  child = entity_child (entity, "content_type");
  if (child == NULL)
    {
      g_warning ("%s: Missing content_type in '%s'",
                 __func__, config_path);
      return -1;
    }
  *content_type = entity_text (child);

  return 0;
}

/**
 * @brief Setup a predefined report format from disk.
 *
 * @param[in]  uuid  UUID of report format.
 *
 * @return 0 success, -1 error.
 */
int
check_report_format (const gchar *uuid)
{
  GError *error;
  gchar *path, *config_path, *xml, *quoted_uuid;
  gsize xml_len;
  const char *name, *summary, *description, *extension, *content_type;
  entity_t entity;
  int update_mod_time;
  report_format_t report_format;

  g_debug ("%s: uuid: %s", __func__, uuid);

  update_mod_time = 0;
  path = predefined_report_format_dir (uuid);
  g_debug ("%s: path: %s", __func__, path);
  config_path = g_build_filename (path, "report_format.xml", NULL);
  g_free (path);

  /* Read the file in. */

  error = NULL;
  g_file_get_contents (config_path, &xml, &xml_len, &error);
  if (error)
    {
      g_warning ("%s: Failed to read '%s': %s",
                  __func__,
                 config_path,
                 error->message);
      g_error_free (error);
      g_free (config_path);
      return -1;
    }

  /* Parse it as XML. */

  if (parse_entity (xml, &entity))
    {
      g_warning ("%s: Failed to parse '%s'", __func__, config_path);
      g_free (config_path);
      return -1;
    }

  /* Get the report format properties from the XML. */

  if (check_report_format_parse (entity, config_path, &name, &summary,
                                 &description, &extension, &content_type))
    {
      g_free (config_path);
      free_entity (entity);
      return -1;
    }

  quoted_uuid = sql_quote (uuid);

  /* Create or update the report format. */

  if (check_report_format_create (quoted_uuid, name, summary, description,
                                  extension, content_type, &report_format))
    goto fail;

  /* Add or update the parameters from the parsed XML. */

  if (check_report_format_add_params (quoted_uuid, config_path, entity,
                                      &update_mod_time))
    goto fail;

  free_entity (entity);
  g_free (config_path);

  /* Remove any params that were not defined by the XML. */

  if (sql_int ("SELECT count (*)"
               " FROM report_format_params_check"
               " WHERE report_format = (SELECT id FROM report_formats"
               "                        WHERE uuid = '%s')",
               quoted_uuid))
    {
      sql ("DELETE FROM report_format_param_options"
           " WHERE report_format_param"
           "       IN (SELECT id FROM report_format_params_check"
           "           WHERE report_format = (SELECT id FROM report_formats"
           "                                  WHERE uuid = '%s'));",
           quoted_uuid);
      sql ("DELETE FROM report_format_params"
           " WHERE id IN (SELECT id FROM report_format_params_check"
           "              WHERE report_format = (SELECT id FROM report_formats"
           "                                     WHERE uuid = '%s'));",
           quoted_uuid);
      update_mod_time = 1;
    }

  /* Update modification time if report format changed. */

  if (update_mod_time)
    sql ("UPDATE report_formats SET modification_time = m_now ()"
         " WHERE uuid = '%s';",
         quoted_uuid);

  /* Keep this report format. */

  sql ("DELETE FROM report_formats_check WHERE uuid = '%s';",
       quoted_uuid);

  g_free (quoted_uuid);
  return 0;

 fail:
  g_free (quoted_uuid);
  g_free (config_path);
  free_entity (entity);
  return -1;
}

/**
 * @brief Verify a report format.
 *
 * @param[in]  report_format  Report format.
 *
 * @return 0 success, -1 error.
 */
static int
verify_report_format_internal (report_format_t report_format)
{
  int format_trust = TRUST_UNKNOWN;
  iterator_t formats;
  get_data_t get;
  gchar *uuid;

  memset(&get, '\0', sizeof (get));
  get.id = report_format_uuid (report_format);
  init_report_format_iterator (&formats, &get);
  if (next (&formats))
    {
      const char *signature;
      gchar *format_signature = NULL;
      gsize format_signature_size;

      signature = report_format_iterator_signature (&formats);

      find_signature ("report_formats", get_iterator_uuid (&formats),
                      &format_signature, &format_signature_size, &uuid);

      if ((signature && strlen (signature))
          || format_signature)
        {
          GString *format;
          file_iterator_t files;
          iterator_t params;

          format = g_string_new ("");

          g_string_append_printf
           (format, "%s%s%s%i", uuid ? uuid : get_iterator_uuid (&formats),
            report_format_iterator_extension (&formats),
            report_format_iterator_content_type (&formats),
            report_format_predefined (report_format) & 1);
          g_free (uuid);

          init_report_format_file_iterator (&files, report_format);
          while (next_file (&files))
            {
              gchar *content = file_iterator_content_64 (&files);
              g_string_append_printf (format,
                                      "%s%s",
                                      file_iterator_name (&files),
                                      content);
              g_free (content);
            }
          cleanup_file_iterator (&files);

          init_report_format_param_iterator (&params,
                                             report_format,
                                             0,
                                             1,
                                             NULL);
          while (next (&params))
            {
              g_string_append_printf
               (format,
                "%s%s",
                report_format_param_iterator_name (&params),
                report_format_param_iterator_type_name (&params));

              if (report_format_param_iterator_type_min (&params) > LLONG_MIN)
                g_string_append_printf
                 (format,
                  "%lli",
                  report_format_param_iterator_type_min (&params));

              if (report_format_param_iterator_type_max (&params) < LLONG_MAX)
                g_string_append_printf
                 (format,
                  "%lli",
                  report_format_param_iterator_type_max (&params));

              g_string_append_printf
               (format,
                "%s%s",
                report_format_param_iterator_type_regex (&params),
                report_format_param_iterator_fallback (&params));

              {
                iterator_t options;
                init_param_option_iterator
                 (&options,
                  report_format_param_iterator_param (&params),
                  1,
                  NULL);
                while (next (&options))
                  if (param_option_iterator_value (&options))
                    g_string_append_printf
                     (format,
                      "%s",
                      param_option_iterator_value (&options));
              }
            }
          cleanup_iterator (&params);

          g_string_append_printf (format, "\n");

          if (format_signature)
            {
              /* Try the feed signature. */
              if (verify_signature (format->str, format->len, format_signature,
                                    strlen (format_signature), &format_trust))
                {
                  cleanup_iterator (&formats);
                  g_free (format_signature);
                  g_string_free (format, TRUE);
                  return -1;
                }
            }
          else if (signature && strlen (signature))
            {
              /* Try the signature from the database. */
              if (verify_signature (format->str, format->len, signature,
                                    strlen (signature), &format_trust))
                {
                  cleanup_iterator (&formats);
                  g_free (format_signature);
                  g_string_free (format, TRUE);
                  return -1;
                }
            }

          g_free (format_signature);
          g_string_free (format, TRUE);
        }
    }
  else
    {
      return -1;
    }
  cleanup_iterator (&formats);

  sql ("UPDATE report_formats SET trust = %i, trust_time = %i,"
       "                          modification_time = m_now ()"
       " WHERE id = %llu;",
       format_trust,
       time (NULL),
       report_format);

  return 0;
}

/**
 * @brief Verify a report format.
 *
 * @param[in]  report_format_id  Report format UUID.
 *
 * @return 0 success, 1 failed to find report format, 99 permission denied,
 *         -1 error.
 */
int
verify_report_format (const char *report_format_id)
{
  int ret;
  report_format_t report_format;

  sql_begin_immediate ();

  if (acl_user_may ("verify_report_format") == 0)
    {
      sql_rollback ();
      return 99;
    }

  report_format = 0;
  if (find_report_format_with_permission (report_format_id, &report_format,
                                          "verify_report_format"))
    {
      sql_rollback ();
      return -1;
    }
  if (report_format == 0)
    {
      sql_rollback ();
      return 1;
    }

  ret = verify_report_format_internal (report_format);
  if (ret)
    {
      sql_rollback ();
      return ret;
    }
  sql_commit ();
  return 0;
}

/**
 * @brief Runs the script of a report format.
 *
 * @param[in]   report_format_id    UUID of the report format.
 * @param[in]   xml_file            Path to main part of the report XML.
 * @param[in]   xml_dir             Path of the dir with XML and subreports.
 * @param[in]   report_format_extra Extra data for report format.
 * @param[in]   output_file         Path to write report to.
 *
 * @return 0 success, -1 error.
 */
static int
run_report_format_script (gchar *report_format_id,
                          gchar *xml_file,
                          gchar *xml_dir,
                          gchar *report_format_extra,
                          gchar *output_file)
{
  iterator_t formats;
  report_format_t report_format;
  gchar *script, *script_dir;
  get_data_t report_format_get;

  gchar *command;
  char *previous_dir;
  int ret;

  /* Setup file names and complete report. */

  memset (&report_format_get, '\0', sizeof (report_format_get));
  report_format_get.id = report_format_id;

  init_report_format_iterator (&formats, &report_format_get);
  if (next (&formats) == FALSE)
    {
      cleanup_iterator (&formats);
      return -1;
    }

  report_format = get_iterator_resource (&formats);

  if (report_format_predefined (report_format))
    {
      script_dir = predefined_report_format_dir (report_format_id);
    }
  else
    {
      gchar *owner;
      owner = sql_string ("SELECT uuid FROM users"
                          " WHERE id = (SELECT owner FROM"
                          "             report_formats WHERE id = %llu);",
                          report_format);
      script_dir = g_build_filename (GVMD_STATE_DIR,
                                     "report_formats",
                                     owner,
                                     report_format_id,
                                     NULL);
      g_free (owner);
    }

  cleanup_iterator (&formats);

  script = g_build_filename (script_dir, "generate", NULL);

  if (!g_file_test (script,
                    G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
    {
      g_warning ("%s: No generate script found at %s",
                 __func__, script);
      g_free (script);
      g_free (script_dir);
      return -1;
    }
  else if (!g_file_test (script,
                         G_FILE_TEST_IS_EXECUTABLE))
    {
      g_warning ("%s: script %s is not executable",
                 __func__, script);
      g_free (script);
      g_free (script_dir);
      return -1;
    }

  /* Change into the script directory. */

  previous_dir = getcwd (NULL, 0);
  if (previous_dir == NULL)
    {
      g_warning ("%s: Failed to getcwd: %s",
                  __func__,
                  strerror (errno));
      g_free (previous_dir);
      g_free (script);
      g_free (script_dir);
      return -1;
    }

  if (chdir (script_dir))
    {
      g_warning ("%s: Failed to chdir: %s",
                  __func__,
                  strerror (errno));
      g_free (previous_dir);
      g_free (script);
      g_free (script_dir);
      return -1;
    }
  g_free (script_dir);

  /* Call the script. */

  command = g_strdup_printf ("%s %s '%s' > %s"
                             " 2> /dev/null",
                             script,
                             xml_file,
                             report_format_extra,
                             output_file);
  g_free (script);

  g_debug ("   command: %s", command);

  if (geteuid () == 0)
    {
      pid_t pid;
      struct passwd *nobody;

      /* Run the command with lower privileges in a fork. */

      nobody = getpwnam ("nobody");
      if ((nobody == NULL)
          || chown (xml_dir, nobody->pw_uid, nobody->pw_gid)
          || chown (xml_file, nobody->pw_uid, nobody->pw_gid)
          || chown (output_file, nobody->pw_uid, nobody->pw_gid))
        {
          g_warning ("%s: Failed to set dir permissions: %s",
                      __func__,
                      strerror (errno));
          g_free (previous_dir);
          return -1;
        }

      pid = fork ();
      switch (pid)
        {
          case 0:
            {
              /* Child.  Drop privileges, run command, exit. */

              proctitle_set ("gvmd: Generating report");

              cleanup_manage_process (FALSE);

              if (setgroups (0,NULL))
                {
                  g_warning ("%s (child): setgroups: %s",
                              __func__, strerror (errno));
                  exit (EXIT_FAILURE);
                }
              if (setgid (nobody->pw_gid))
                {
                  g_warning ("%s (child): setgid: %s",
                              __func__,
                              strerror (errno));
                  exit (EXIT_FAILURE);
                }
              if (setuid (nobody->pw_uid))
                {
                  g_warning ("%s (child): setuid: %s",
                              __func__,
                              strerror (errno));
                  exit (EXIT_FAILURE);
                }

              ret = system (command);
              /* Ignore the shell command exit status, because we've not
                * specified what it must be in the past. */
              if (ret == -1)
                {
                  g_warning ("%s (child):"
                              " system failed with ret %i, %i, %s",
                              __func__,
                              ret,
                              WEXITSTATUS (ret),
                              command);
                  exit (EXIT_FAILURE);
                }

              exit (EXIT_SUCCESS);
            }

          case -1:
            /* Parent when error. */

            g_warning ("%s: Failed to fork: %s",
                        __func__,
                        strerror (errno));
            if (chdir (previous_dir))
              g_warning ("%s: and chdir failed",
                          __func__);
            g_free (previous_dir);
            g_free (command);
            return -1;
            break;

          default:
            {
              int status;

              /* Parent on success.  Wait for child, and check result. */

              g_free (command);

              while (waitpid (pid, &status, 0) < 0)
                {
                  if (errno == ECHILD)
                    {
                      g_warning ("%s: Failed to get child exit status",
                                  __func__);
                      if (chdir (previous_dir))
                        g_warning ("%s: and chdir failed",
                                    __func__);
                      g_free (previous_dir);
                      return -1;
                    }
                  if (errno == EINTR)
                    continue;
                  g_warning ("%s: wait: %s",
                              __func__,
                              strerror (errno));
                  if (chdir (previous_dir))
                    g_warning ("%s: and chdir failed",
                                __func__);
                  g_free (previous_dir);
                  return -1;
                }
              if (WIFEXITED (status))
                switch (WEXITSTATUS (status))
                  {
                    case EXIT_SUCCESS:
                      break;
                    case EXIT_FAILURE:
                    default:
                      g_warning ("%s: child failed, %s",
                                  __func__,
                                  command);
                      if (chdir (previous_dir))
                        g_warning ("%s: and chdir failed",
                                    __func__);
                      g_free (previous_dir);
                      return -1;
                  }
              else
                {
                  g_warning ("%s: child failed, %s",
                              __func__,
                              command);
                  if (chdir (previous_dir))
                    g_warning ("%s: and chdir failed",
                                __func__);
                  g_free (previous_dir);
                  return -1;
                }

              /* Child succeeded, continue to process result. */

              break;
            }
        }
    }
  else
    {
      /* Just run the command as the current user. */

      ret = system (command);
      /* Ignore the shell command exit status, because we've not
        * specified what it must be in the past. */
      if (ret == -1)
        {
          g_warning ("%s: system failed with ret %i, %i, %s",
                      __func__,
                      ret,
                      WEXITSTATUS (ret),
                      command);
          if (chdir (previous_dir))
            g_warning ("%s: and chdir failed",
                        __func__);
          g_free (previous_dir);
          g_free (command);
          return -1;
        }

      g_free (command);
    }

  /* Change back to the previous directory. */

  if (chdir (previous_dir))
    {
      g_warning ("%s: Failed to chdir back: %s",
                  __func__,
                  strerror (errno));
      g_free (previous_dir);
      return -1;
    }
  g_free (previous_dir);

  return 0;
}

/**
 * @brief Completes a report by adding report format info.
 *
 * @param[in]   xml_start      Path of file containing start of report.
 * @param[in]   xml_full       Path to file to print full report to.
 * @param[in]   report_format  Format of report that will be created from XML.
 *
 * @return 0 success, -1 error.
 */
static int
print_report_xml_end (gchar *xml_start, gchar *xml_full,
                      report_format_t report_format)
{
  FILE *out;
  iterator_t params;

  if (gvm_file_copy (xml_start, xml_full) == FALSE)
    {
      g_warning ("%s: failed to copy xml_start file", __func__);
      return -1;
    }

  out = fopen (xml_full, "a");
  if (out == NULL)
    {
      g_warning ("%s: fopen failed: %s",
                 __func__,
                 strerror (errno));
      return -1;
    }

  /* A bit messy having report XML here, but simplest for now. */

  PRINT (out, "<report_format>");
  init_report_format_param_iterator (&params, report_format, 0, 1, NULL);
  while (next (&params))
    PRINT (out,
           "<param><name>%s</name><value>%s</value></param>",
           report_format_param_iterator_name (&params),
           report_format_param_iterator_value (&params));
  cleanup_iterator (&params);

  PRINT (out, "</report_format>");

  PRINT (out, "</report>");

  if (fclose (out))
    {
      g_warning ("%s: fclose failed: %s",
                 __func__,
                 strerror (errno));
      return -1;
    }

  return 0;
}

/**
 * @brief Applies a report format to an XML report.
 *
 * @param[in]  report_format_id   Report format to apply.
 * @param[in]  xml_start          Path to the main part of the report XML.
 * @param[in]  xml_file           Path to the report XML file.
 * @param[in]  xml_dir            Path to the temporary dir.
 * @param[in]  used_rfps          List of already applied report formats.
 *
 * @return Path to the generated file or NULL.
 */
gchar*
apply_report_format (gchar *report_format_id,
                     gchar *xml_start,
                     gchar *xml_file,
                     gchar *xml_dir,
                     GList **used_rfps)
{
  report_format_t report_format;
  GHashTable *subreports;
  GList *temp_dirs, *temp_files;
  gchar *rf_dependencies_string, *output_file, *out_file_part, *out_file_ext;
  gchar *files_xml;
  int output_fd;

  assert (report_format_id);
  assert (xml_start);
  assert (xml_file);
  assert (xml_dir);
  assert (used_rfps);

  /* Check if there would be an infinite recursion loop. */
  if (*used_rfps
      && g_list_find_custom (*used_rfps, report_format_id,
                             (GCompareFunc) strcmp))
    {
      g_message ("%s: Recursion loop for report_format '%s'",
                 __func__, report_format_id);
      return NULL;
    }

  /* Check if report format is available. */
  if (find_report_format_with_permission (report_format_id, &report_format,
                                          "get_report_formats")
      || report_format == 0)
    {
      g_message ("%s: Report format '%s' not found",
                 __func__, report_format_id);
      return NULL;
    }

  /* Check if report format is active */
  if (report_format_active (report_format) == 0)
    {
      g_message ("%s: Report format '%s' is not active",
                 __func__, report_format_id);
      return NULL;
    }

  /* Get subreports. */
  temp_dirs = NULL;
  temp_files = NULL;
  subreports = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  rf_dependencies_string
    = sql_string ("SELECT value"
                  "  FROM report_format_params"
                  " WHERE report_format = %llu"
                  "   AND type = %i",
                  report_format,
                  REPORT_FORMAT_PARAM_TYPE_REPORT_FORMAT_LIST);

  if (rf_dependencies_string)
    {
      gchar **rf_dependencies, **current_rf_dependency;
      GString *files_xml_buf;
      GHashTableIter files_iter;
      gchar *key, *value;

      *used_rfps = g_list_append (*used_rfps, report_format_id);

      /* Recursively create subreports for dependencies. */
      rf_dependencies = g_strsplit (rf_dependencies_string, ",", -1);
      current_rf_dependency = rf_dependencies;

      while (*current_rf_dependency)
        {
          gchar *subreport_dir, *subreport_xml, *subreport_file;
          subreport_file = NULL;

          subreport_dir = g_strdup ("/tmp/gvmd_XXXXXX");

          if (mkdtemp (subreport_dir) == NULL)
            {
              g_warning ("%s: mkdtemp failed", __func__);
              g_free (subreport_dir);
              break;
            }
          subreport_xml = g_build_filename (subreport_dir, "report.xml", NULL);
          temp_dirs = g_list_append (temp_dirs, subreport_dir);
          temp_files = g_list_append (temp_files, subreport_xml);

          if (g_hash_table_contains (subreports, *current_rf_dependency)
              == FALSE)
            {
              subreport_file = apply_report_format (*current_rf_dependency,
                                                    xml_start,
                                                    subreport_xml,
                                                    subreport_dir,
                                                    used_rfps);
              if (subreport_file)
                {
                  g_hash_table_insert (subreports,
                                       g_strdup (*current_rf_dependency),
                                       subreport_file);
                }
            }

          current_rf_dependency ++;
        }

      g_strfreev (rf_dependencies);

      *used_rfps = g_list_remove (*used_rfps, report_format_id);

      /* Build dependencies XML. */
      files_xml_buf = g_string_new ("<files>");
      xml_string_append (files_xml_buf,
                         "<basedir>%s</basedir>",
                         xml_dir);

      g_hash_table_iter_init (&files_iter, subreports);
      while (g_hash_table_iter_next (&files_iter,
                                     (void**)&key, (void**)&value))
        {
          get_data_t report_format_get;
          iterator_t file_format_iter;

          memset (&report_format_get, '\0', sizeof (report_format_get));
          report_format_get.id = key;

          init_report_format_iterator (&file_format_iter, &report_format_get);
          if (next (&file_format_iter))
            {
              xml_string_append (files_xml_buf,
                                 "<file id=\"%s\""
                                 " content_type=\"%s\""
                                 " report_format_name=\"%s\">"
                                 "%s"
                                 "</file>",
                                 key,
                                 report_format_iterator_content_type
                                  (&file_format_iter),
                                 get_iterator_name (&file_format_iter),
                                 value);
            }
          else
            {
              xml_string_append (files_xml_buf,
                                 "<file id=\"%s\">%s</file>",
                                 key, value);
            }
          cleanup_iterator (&file_format_iter);
        }

      g_string_append (files_xml_buf, "</files>");
      files_xml = g_string_free (files_xml_buf, FALSE);
    }
  else
    {
      GString *files_xml_buf;
      /* Build dependencies XML. */
      files_xml_buf = g_string_new ("<files>");
      xml_string_append (files_xml_buf,
                         "<basedir>%s</basedir>",
                         xml_dir);
      g_string_append (files_xml_buf, "</files>");
      files_xml = g_string_free (files_xml_buf, FALSE);
    }

  /* Generate output file. */
  out_file_ext = report_format_extension (report_format);
  out_file_part = g_strdup_printf ("%s-XXXXXX.%s",
                                   report_format_id, out_file_ext);
  output_file = g_build_filename (xml_dir, out_file_part, NULL);
  output_fd = mkstemps (output_file, strlen (out_file_ext) + 1);
  if (output_fd == -1)
    {
      g_warning ("%s: mkstemps failed: %s", __func__, strerror (errno));
      g_free (output_file);
      output_file = NULL;
      goto cleanup;
    }
  g_free (out_file_ext);
  g_free (out_file_part);

  /* Add second half of input XML */

  if (print_report_xml_end (xml_start, xml_file, report_format))
    {
      g_free (output_file);
      output_file = NULL;
      goto cleanup;
    }

  run_report_format_script (report_format_id,
                            xml_file, xml_dir, files_xml, output_file);

  /* Clean up and return filename. */
 cleanup:
  while (temp_dirs)
    {
      gvm_file_remove_recurse (temp_dirs->data);
      g_free (temp_dirs->data);
      temp_dirs = g_list_remove (temp_dirs, temp_dirs->data);
    }
  while (temp_files)
    {
      g_free (temp_files->data);
      temp_files = g_list_remove (temp_files, temp_files->data);
    }
  g_free (files_xml);
  g_hash_table_destroy (subreports);
  if (close (output_fd))
    {
      g_warning ("%s: close of output_fd failed: %s",
                 __func__, strerror (errno));
      g_free (output_file);
      return NULL;
    }

  return output_file;
}

/**
 * @brief Empty trashcan.
 *
 * @return 0 success, -1 error.
 */
int
empty_trashcan_report_formats ()
{
  GArray *report_formats;
  int index, length;
  iterator_t rows;

  sql ("DELETE FROM report_format_param_options_trash"
       " WHERE report_format_param"
       "       IN (SELECT id from report_format_params_trash"
       "           WHERE report_format"
       "                 IN (SELECT id FROM report_formats_trash"
       "                     WHERE owner = (SELECT id FROM users"
       "                                    WHERE uuid = '%s')));",
       current_credentials.uuid);
  sql ("DELETE FROM report_format_params_trash"
       " WHERE report_format IN (SELECT id from report_formats_trash"
       "                         WHERE owner = (SELECT id FROM users"
       "                                        WHERE uuid = '%s'));",
       current_credentials.uuid);

  init_iterator (&rows,
                 "SELECT id FROM report_formats_trash"
                 " WHERE owner = (SELECT id FROM users WHERE uuid = '%s');",
                 current_credentials.uuid);
  report_formats = g_array_new (FALSE, FALSE, sizeof (report_format_t));
  length = 0;
  while (next (&rows))
    {
      report_format_t id;
      id = iterator_int64 (&rows, 0);
      g_array_append_val (report_formats, id);
      length++;
    }
  cleanup_iterator (&rows);

  sql ("DELETE FROM report_formats_trash"
       " WHERE owner = (SELECT id FROM users WHERE uuid = '%s');",
       current_credentials.uuid);

  /* Remove the report formats dirs last, in case any SQL rolls back. */

  for (index = 0; index < length; index++)
    {
      gchar *dir, *name;

      name = g_strdup_printf ("%llu",
                              g_array_index (report_formats,
                                             report_format_t,
                                             index));
      dir = report_format_trash_dir (name);
      g_free (name);

      if (g_file_test (dir, G_FILE_TEST_EXISTS) && gvm_file_remove_recurse (dir))
        {
          g_warning ("%s: failed to remove trash dir %s", __func__, dir);
          g_free (dir);
          sql_rollback ();
          return -1;
        }

      g_free (dir);
    }

  g_array_free (report_formats, TRUE);
  return 0;
}

/**
 * @brief Change ownership of report formats, for user deletion.
 *
 * @param[in]  user       Current owner.
 * @param[in]  inheritor  New owner.
 */
void
inherit_report_formats (user_t user, user_t inheritor)
{
  sql ("UPDATE report_formats SET owner = %llu WHERE owner = %llu;",
       inheritor, user);

  sql ("UPDATE report_formats_trash SET owner = %llu WHERE owner = %llu;",
       inheritor, user);
}

/**
 * @brief Delete all report formats owned by a user.
 *
 * @param[in]  user  The user.
 */
void
delete_report_formats_user (user_t user)
{
  sql ("DELETE FROM report_format_param_options"
       " WHERE report_format_param"
       "       IN (SELECT id FROM report_format_params"
       "           WHERE report_format IN (SELECT id"
       "                                   FROM report_formats"
       "                                   WHERE owner = %llu));",
       user);
  sql ("DELETE FROM report_format_param_options_trash"
       " WHERE report_format_param"
       "       IN (SELECT id FROM report_format_params_trash"
       "           WHERE report_format IN (SELECT id"
       "                                   FROM report_formats_trash"
       "                                   WHERE owner = %llu));",
       user);
  sql ("DELETE FROM report_format_params"
       " WHERE report_format IN (SELECT id FROM report_formats"
       "                         WHERE owner = %llu);",
       user);
  sql ("DELETE FROM report_format_params_trash"
       " WHERE report_format IN (SELECT id"
       "                         FROM report_formats_trash"
       "                         WHERE owner = %llu);",
       user);
  sql ("DELETE FROM report_formats WHERE owner = %llu;", user);
  sql ("DELETE FROM report_formats_trash WHERE owner = %llu;", user);
}


/* Startup. */

/**
 * @brief Bring UUIDs for single predefined report format up to date.
 *
 * @param[in]  old  Old UUID.
 * @param[in]  new  New UUID.
 */
static void
update_report_format_uuid (const char *old, const char *new)
{
  gchar *dir;

  dir = predefined_report_format_dir (old);
  if (g_file_test (dir, G_FILE_TEST_EXISTS))
    gvm_file_remove_recurse (dir);
  g_free (dir);

  sql ("UPDATE report_formats"
       " SET uuid = '%s', modification_time = m_now ()"
       " WHERE uuid = '%s';",
       new,
       old);

  sql ("UPDATE alert_method_data"
       " SET data = '%s'"
       " WHERE data = '%s';",
       new,
       old);
}

/**
 * @brief Bring report format UUIDs in database up to date.
 */
static void
update_report_format_uuids ()
{
  /* Same as migrate_58_to_59, to enable backporting r13519 to OpenVAS-5
   * without backporting the 58 to 59 migrator.  In future these should be
   * done here instead of in a migrator. */

  update_report_format_uuid ("a0704abb-2120-489f-959f-251c9f4ffebd",
                             "5ceff8ba-1f62-11e1-ab9f-406186ea4fc5");

  update_report_format_uuid ("b993b6f5-f9fb-4e6e-9c94-dd46c00e058d",
                             "6c248850-1f62-11e1-b082-406186ea4fc5");

  update_report_format_uuid ("929884c6-c2c4-41e7-befb-2f6aa163b458",
                             "77bd6c4a-1f62-11e1-abf0-406186ea4fc5");

  update_report_format_uuid ("9f1ab17b-aaaa-411a-8c57-12df446f5588",
                             "7fcc3a1a-1f62-11e1-86bf-406186ea4fc5");

  update_report_format_uuid ("f5c2a364-47d2-4700-b21d-0a7693daddab",
                             "9ca6fe72-1f62-11e1-9e7c-406186ea4fc5");

  update_report_format_uuid ("1a60a67e-97d0-4cbf-bc77-f71b08e7043d",
                             "a0b5bfb2-1f62-11e1-85db-406186ea4fc5");

  update_report_format_uuid ("19f6f1b3-7128-4433-888c-ccc764fe6ed5",
                             "a3810a62-1f62-11e1-9219-406186ea4fc5");

  update_report_format_uuid ("d5da9f67-8551-4e51-807b-b6a873d70e34",
                             "a994b278-1f62-11e1-96ac-406186ea4fc5");

  /* New updates go here.  Oldest must come first, so add at the end. */

  update_report_format_uuid ("7fcc3a1a-1f62-11e1-86bf-406186ea4fc5",
                             "a684c02c-b531-11e1-bdc2-406186ea4fc5");

  update_report_format_uuid ("a0b5bfb2-1f62-11e1-85db-406186ea4fc5",
                             "c402cc3e-b531-11e1-9163-406186ea4fc5");
}

/**
 * @brief Ensure every report format has a unique UUID.
 *
 * @return 0 success, -1 error.
 */
static int
make_report_format_uuids_unique ()
{
  iterator_t rows;

  sql ("CREATE TEMPORARY TABLE duplicates"
       " AS SELECT id, uuid, make_uuid () AS new_uuid, owner,"
       "           (SELECT uuid FROM users"
       "            WHERE users.id = outer_report_formats.owner)"
       "           AS owner_uuid,"
       "           (SELECT owner from report_formats"
       "                              WHERE uuid = outer_report_formats.uuid"
       "                              ORDER BY id ASC LIMIT 1)"
       "           AS original_owner,"
       "           (SELECT uuid FROM users"
       "            WHERE users.id = (SELECT owner from report_formats"
       "                              WHERE uuid = outer_report_formats.uuid"
       "                              ORDER BY id ASC LIMIT 1))"
       "           AS original_owner_uuid"
       "    FROM report_formats AS outer_report_formats"
       "    WHERE id > (SELECT id from report_formats"
       "                WHERE uuid = outer_report_formats.uuid"
       "                ORDER BY id ASC LIMIT 1);");

  sql ("UPDATE alert_method_data"
       " SET data = (SELECT new_uuid FROM duplicates"
       "             WHERE duplicates.id = alert_method_data.alert)"
       " WHERE alert IN (SELECT id FROM duplicates);");

  /* Update UUIDs on disk. */
  init_iterator (&rows,
                 "SELECT id, uuid, new_uuid, owner, owner_uuid, original_owner,"
                 "       original_owner_uuid"
                 " FROM duplicates;");
  while (next (&rows))
    {
      gchar *dir, *new_dir;
      const char *old_uuid, *new_uuid;
      int copy;

      old_uuid = iterator_string (&rows, 1);
      new_uuid = iterator_string (&rows, 2);

      if (iterator_int64 (&rows, 3) == 0)
        {
          /* Old-style "global" report format.  I don't think this is possible
           * with any released version, so ignore. */
          continue;
        }
      else if (iterator_int64 (&rows, 5) == 0)
        {
          const char *owner_uuid;
          /* Dedicated subdir in user dir, but must be renamed. */
          copy = 0;
          owner_uuid = iterator_string (&rows, 4);
          dir = g_build_filename (GVMD_STATE_DIR,
                                  "report_formats",
                                  owner_uuid,
                                  old_uuid,
                                  NULL);
          new_dir = g_build_filename (GVMD_STATE_DIR,
                                      "report_formats",
                                      owner_uuid,
                                      new_uuid,
                                      NULL);
        }
      else
        {
          const char *owner_uuid, *original_owner_uuid;

          /* Two user-owned report formats, may be the same user. */

          owner_uuid = iterator_string (&rows, 4);
          original_owner_uuid = iterator_string (&rows, 6);

          /* Copy the subdir if both report formats owned by one user. */
          copy = owner_uuid
                 && original_owner_uuid
                 && (strcmp (owner_uuid, original_owner_uuid) == 0);

          dir = g_build_filename (GVMD_STATE_DIR,
                                  "report_formats",
                                  owner_uuid,
                                  old_uuid,
                                  NULL);
          new_dir = g_build_filename (GVMD_STATE_DIR,
                                      "report_formats",
                                      owner_uuid,
                                      new_uuid,
                                      NULL);
        }

      if (copy)
        {
          gchar *command;
          int ret;

          command = g_strdup_printf ("cp -a %s %s > /dev/null 2>&1",
                                     dir,
                                     new_dir);
          g_debug ("   command: %s", command);
          ret = system (command);
          g_free (command);

          if (ret == -1 || WEXITSTATUS (ret))
            {
              /* Presume dir missing, just log a warning. */
              g_warning ("%s: cp %s to %s failed",
                         __func__, dir, new_dir);
            }
          else
            g_debug ("%s: copied %s to %s", __func__, dir, new_dir);
        }
      else
        {
          if (rename (dir, new_dir))
            {
              g_warning ("%s: rename %s to %s: %s",
                         __func__, dir, new_dir, strerror (errno));
              if (errno != ENOENT)
                {
                  g_free (dir);
                  g_free (new_dir);
                  sql_rollback ();
                  return -1;
                }
            }
          else
            g_debug ("%s: moved %s to %s", __func__, dir, new_dir);
        }
      g_free (dir);
      g_free (new_dir);
    }
  cleanup_iterator (&rows);

  sql ("UPDATE report_formats"
       " SET uuid = (SELECT new_uuid FROM duplicates"
       "             WHERE duplicates.id = report_formats.id)"
       " WHERE id IN (SELECT id FROM duplicates);");

  if (sql_changes () > 0)
    g_debug ("%s: gave %d report format(s) new UUID(s) to keep UUIDs unique.",
             __func__, sql_changes ());

  sql ("DROP TABLE duplicates;");

  return 0;
}

/**
 * @brief Check that trash report formats are correct.
 *
 * @return 0 success, -1 error.
 */
static int
check_db_trash_report_formats ()
{
  gchar *dir;
  struct stat state;

  dir = g_build_filename (GVMD_STATE_DIR,
                          "report_formats_trash",
                          NULL);

  if (g_lstat (dir, &state))
    {
      iterator_t report_formats;
      int count;

      if (errno != ENOENT)
        {
          g_warning ("%s: g_lstat (%s) failed: %s",
                     __func__, dir, g_strerror (errno));
          g_free (dir);
          return -1;
        }

      /* Remove all trash report formats. */

      count = 0;
      init_iterator (&report_formats, "SELECT id FROM report_formats_trash;");
      while (next (&report_formats))
        {
          report_format_t report_format;

          report_format = iterator_int64 (&report_formats, 0);

          sql ("DELETE FROM alert_method_data_trash"
               " WHERE data = (SELECT original_uuid"
               "               FROM report_formats_trash"
               "               WHERE id = %llu)"
               " AND (name = 'notice_attach_format'"
               "      OR name = 'notice_report_format');",
               report_format);

          permissions_set_orphans ("report_format", report_format,
                                   LOCATION_TRASH);
          tags_remove_resource ("report_format", report_format, LOCATION_TRASH);

          sql ("DELETE FROM report_format_param_options_trash"
               " WHERE report_format_param"
               " IN (SELECT id from report_format_params_trash"
               "     WHERE report_format = %llu);",
               report_format);
          sql ("DELETE FROM report_format_params_trash WHERE report_format = %llu;",
               report_format);
          sql ("DELETE FROM report_formats_trash WHERE id = %llu;",
               report_format);

          count++;
        }
      cleanup_iterator (&report_formats);

      if (count)
        g_message ("Trash report format directory was missing."
                   " Removed all %i trash report formats.",
                   count);
    }

  g_free (dir);
  return 0;
}

/**
 * @brief Ensure the predefined report formats exist.
 *
 * @return 0 success, -1 error.
 */
int
check_db_report_formats ()
{
  GError *error;
  GDir *dir;
  gchar *path;
  const gchar *report_format_path;
  iterator_t report_formats;

  if (check_db_trash_report_formats ())
    return -1;

  /* Bring report format UUIDs in database up to date. */
  update_report_format_uuids ();
  if (make_report_format_uuids_unique ())
    return -1;

  /* Open the global report format dir. */

  path = predefined_report_format_dir (NULL);

  error = NULL;
  dir = g_dir_open (path, 0, &error);
  if (dir == NULL)
    {
      g_warning ("%s: Failed to open directory '%s': %s",
                 __func__, path, error->message);
      g_error_free (error);
      g_free (path);
      return -1;
    }
  g_free (path);

  /* Remember existing global report formats. */

  sql ("CREATE TEMPORARY TABLE report_formats_check"
       " AS SELECT id, uuid, name, owner, summary, description, extension,"
       "           content_type, signature, trust, trust_time, flags,"
       "           creation_time, modification_time"
       "    FROM report_formats"
       "    WHERE owner IS NULL;");

  sql ("CREATE TEMPORARY TABLE report_format_params_check"
       " AS SELECT id, name, report_format, type, value, type_min, type_max,"
       "           type_regex, fallback"
       "    FROM report_format_params"
       "    WHERE report_format IN (SELECT id FROM report_formats"
       "                            WHERE owner IS NULL);");

  /* Create or update global report formats from disk. */

  while ((report_format_path = g_dir_read_name (dir)))
    check_report_format (report_format_path);

  /* Remove previous global report formats that were not defined. */

  init_iterator (&report_formats,
                 "SELECT id, uuid, name FROM report_formats"
                 " WHERE uuid IN (SELECT uuid FROM report_formats_check)"
                 " AND (EXISTS (SELECT * FROM alert_method_data_trash"
                 "              WHERE data = report_formats.uuid"
                 "              AND (name = 'notice_attach_format'"
                 "                   OR name = 'notice_report_format'))"
                 "      OR EXISTS (SELECT * FROM alert_method_data"
                 "                 WHERE data = report_formats.uuid"
                 "                 AND (name = 'notice_attach_format'"
                 "                      OR name = 'notice_report_format')));");
  while (next (&report_formats))
    g_warning
     ("Removing old report format %s (%s) which is in use by an alert.\n"
      "Alert will fallback to TXT report format (%s), if TXT exists.",
      iterator_string (&report_formats, 2),
      iterator_string (&report_formats, 1),
      "a3810a62-1f62-11e1-9219-406186ea4fc5");
  cleanup_iterator (&report_formats);

  sql ("DELETE FROM report_format_param_options"
       " WHERE report_format_param"
       "       IN (SELECT id FROM report_format_params"
       "           WHERE report_format"
       "                 IN (SELECT id FROM report_formats"
       "                     WHERE uuid IN (SELECT uuid"
       "                                    FROM report_formats_check)));");

  sql ("DELETE FROM report_format_params"
       " WHERE report_format IN (SELECT id FROM report_formats"
       "                         WHERE uuid IN (SELECT uuid"
       "                                        FROM report_formats_check));");

  sql ("DELETE FROM resources_predefined"
       " WHERE resource_type = 'report_format'"
       " AND resource IN (SELECT id FROM report_formats_check);");

  sql ("DELETE FROM report_formats"
       " WHERE uuid IN (SELECT uuid FROM report_formats_check);");

  /* Forget the old global report formats. */

  sql ("DROP TABLE report_format_params_check;");
  sql ("DROP TABLE report_formats_check;");

  return 0;
}

/**
 * @brief Ensure that the report formats trash directory matches the database.
 *
 * @return -1 if error, 0 if success.
 */
int
check_db_report_formats_trash ()
{
  gchar *dir;
  GError *error;
  GDir *directory;
  const gchar *entry;

  dir = report_format_trash_dir (NULL);
  error = NULL;
  directory = g_dir_open (dir, 0, &error);

  if (directory == NULL)
    {
      assert (error);
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_warning ("g_dir_open (%s) failed - %s", dir, error->message);
          g_error_free (error);
          g_free (dir);
          return -1;
        }
    }
  else
    {
      entry = NULL;
      while ((entry = g_dir_read_name (directory)) != NULL)
        {
          gchar *end;
          if (strtol (entry, &end, 10) < 0)
            /* Only interested in positive numbers. */
            continue;
          if (*end != '\0')
            /* Only interested in numbers. */
            continue;

          /* Check whether the db has a report format with this ID. */
          if (sql_int ("SELECT count(*) FROM report_formats_trash"
                       " WHERE id = %s;",
                       entry)
              == 0)
            {
              int ret;
              gchar *entry_path;

              /* Remove the directory. */

              entry_path = g_build_filename (dir, entry, NULL);
              ret = gvm_file_remove_recurse (entry_path);
              g_free (entry_path);
              if (ret)
                {
                  g_warning ("%s: failed to remove %s from %s",
                             __func__, entry, dir);
                  g_dir_close (directory);
                  g_free (dir);
                  return -1;
                }
            }
        }
      g_dir_close (directory);
    }
  g_free (dir);
  return 0;
}