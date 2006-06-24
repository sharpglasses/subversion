/*
 * merge.c:  merging changes into a working file
 *
 * ====================================================================
 * Copyright (c) 2000-2006 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#include "svn_wc.h"
#include "svn_diff.h"
#include "svn_path.h"

#include "wc.h"
#include "entries.h"
#include "translate.h"
#include "questions.h"

#include "svn_private_config.h"



svn_error_t *
svn_wc_merge2(const char *left,
              const char *right,
              const char *merge_target,
              svn_wc_adm_access_t *adm_access,
              const char *left_label,
              const char *right_label,
              const char *target_label,
              svn_boolean_t dry_run,
              enum svn_wc_merge_outcome_t *merge_outcome,
              const char *diff3_cmd,
              const apr_array_header_t *merge_options,
              apr_pool_t *pool)
{
  const char *tmp_target, *result_target;
  const char *mt_pt, *mt_bn;
  apr_file_t *result_f;
  svn_boolean_t is_binary;
  apr_hash_t *keywords;
  const char *eol;
  const svn_wc_entry_t *entry;
  svn_boolean_t contains_conflicts, special;

  svn_path_split(merge_target, &mt_pt, &mt_bn, pool);

  /* Sanity check:  the merge target must be under revision control. */
  SVN_ERR(svn_wc_entry(&entry, merge_target, adm_access, FALSE, pool));
  if (! entry)
    {
      *merge_outcome = svn_wc_merge_no_merge;
      return SVN_NO_ERROR;
    }

  /* Decide if the merge target is a text or binary file. */
  SVN_ERR(svn_wc_has_binary_prop(&is_binary, merge_target, adm_access, pool));
  
  if (! is_binary)              /* this is a text file */
    {
      apr_uint32_t translate_opts = SVN_WC_TRANSLATE_TO_NF;

      if (diff3_cmd)
        /* Force a copy into the temporary wc area to avoid having
           temporary files created below to appear in the actual wc. */
        translate_opts |= SVN_WC_TRANSLATE_FORCE_COPY;

      /* Make sure a temporary copy of 'target' is available with keywords
         contracted and line endings in repository-normal (LF) form.
         This is the file that diff3 will read as the 'mine' file.  */
      SVN_ERR(svn_wc_translated_file2
              (&tmp_target, merge_target,
               merge_target, adm_access,
               translate_opts, pool));

      /* Open a second temporary file for writing; this is where diff3
         will write the merged results. */
      SVN_ERR(svn_io_open_unique_file2(&result_f, &result_target,
                                       merge_target, SVN_WC__TMP_EXT,
                                       svn_io_file_del_on_pool_cleanup,
                                       pool));

      /* Run an external merge if requested. */
      if (diff3_cmd)
        {
          int exit_code;
          const char *tmp_left, *tmp_right;

          /* LEFT and RIGHT might be in totally different directories than
             MERGE_TARGET, and our [external] diff3 command wants them all
             to be in the same directory.  So make temporary copies of LEFT
             and RIGHT right next to the target. */

          SVN_ERR(svn_io_open_unique_file2(NULL, &tmp_left,
                                           tmp_target,
                                           SVN_WC__TMP_EXT,
                                           svn_io_file_del_on_pool_cleanup,
                                           pool));
          SVN_ERR(svn_io_copy_file(left, tmp_left, TRUE, pool));

          SVN_ERR(svn_io_open_unique_file2(NULL, &tmp_right,
                                           tmp_target,
                                           SVN_WC__TMP_EXT,
                                           svn_io_file_del_on_pool_cleanup,
                                           pool));
          SVN_ERR(svn_io_copy_file(right, tmp_right, TRUE, pool));

          SVN_ERR(svn_io_run_diff3_2(".",
                                     tmp_target, tmp_left, tmp_right,
                                     target_label, left_label, right_label,
                                     result_f, &exit_code, diff3_cmd,
                                     merge_options, pool));

          contains_conflicts = exit_code == 1;
        }
      else
        {
          svn_diff_t *diff;
          const char *target_marker;
          const char *left_marker;
          const char *right_marker;
          svn_stream_t *ostream;
          svn_diff_file_options_t *options;

          ostream = svn_stream_from_aprfile(result_f, pool);
          options = svn_diff_file_options_create(pool);

          if (merge_options)
            SVN_ERR(svn_diff_file_options_parse(options, merge_options, pool));

          SVN_ERR(svn_diff_file_diff3_2(&diff,
                                        left, tmp_target, right,
                                        options, pool));

          /* Labels fall back to sensible defaults if not specified. */
          if (target_label)
            target_marker = apr_psprintf(pool, "<<<<<<< %s", target_label);
          else
            target_marker = "<<<<<<< .working";

          if (left_label)
            left_marker = apr_psprintf(pool, "||||||| %s", left_label);
          else
            left_marker = "||||||| .old";

          if (right_label)
            right_marker = apr_psprintf(pool, ">>>>>>> %s", right_label);
          else
            right_marker = ">>>>>>> .new";

          SVN_ERR(svn_diff_file_output_merge(ostream, diff,
                                             left, tmp_target, right,
                                             left_marker,
                                             target_marker,
                                             right_marker,
                                             "=======", /* seperator */
                                             FALSE, /* display original */
                                             FALSE, /* resolve conflicts */
                                             pool));
          SVN_ERR(svn_stream_close(ostream));

          contains_conflicts = svn_diff_contains_conflicts(diff);
        }

      /* Close the output file */
      SVN_ERR(svn_io_file_close(result_f, pool));

      if (contains_conflicts && ! dry_run)  /* got a conflict */
        {
          /* Preserve the three pre-merge files, and modify the
             entry (mark as conflicted, track the preserved files). */ 
          const char *left_copy, *right_copy, *target_copy;
          const char *parentt, *left_base, *right_base, *target_base;
          svn_wc_adm_access_t *parent_access;
          svn_wc_entry_t tmp_entry;
      
          /* I miss Lisp. */

          SVN_ERR(svn_io_open_unique_file2(NULL,
                                           &left_copy,
                                           merge_target,
                                           left_label,
                                           svn_io_file_del_none,
                                           pool));

          /* Have I mentioned how much I miss Lisp? */

          SVN_ERR(svn_io_open_unique_file2(NULL,
                                           &right_copy,
                                           merge_target,
                                           right_label,
                                           svn_io_file_del_none,
                                           pool));

          /* Why, how much more pleasant to be forced to unroll my loops.
             If I'd been writing in Lisp, I might have mapped an inline
             lambda form over a list, or something equally disgusting.
             Thank goodness C was here to protect me! */

          SVN_ERR(svn_io_open_unique_file2(NULL,
                                           &target_copy,
                                           merge_target,
                                           target_label,
                                           svn_io_file_del_none,
                                           pool));

          /* We preserve all the files with keywords expanded and line
             endings in local (working) form. */

          /* NOTE: Callers must ensure that the svn:eol-style and
             svn:keywords property values are correct in the currently
             installed props.  With 'svn merge', it's no big deal.  But
             when 'svn up' calls this routine, it needs to make sure that
             this routine is using the newest property values that may
             have been received *during* the update.  Since this routine
             will be run from within a log-command, install_file()
             needs to make sure that a previous log-command to 'install
             latest props' has already executed first.  Ben and I just
             checked, and that is indeed the order in which the log items
             are written, so everything should be fine.  Really.  */

          /* Create LEFT and RIGHT backup files, in expanded form.
             We use merge_target's current properties to do the translation. */
          SVN_ERR(svn_wc__get_keywords(&keywords, merge_target, adm_access,
                                       NULL, pool));
          SVN_ERR(svn_wc__get_eol_style(NULL, &eol, merge_target, adm_access,
                                        pool));
          SVN_ERR(svn_wc__get_special(&special, merge_target, adm_access,
                                      pool));
          SVN_ERR(svn_subst_copy_and_translate3(left,
                                                left_copy,
                                                eol, eol ? TRUE : FALSE, 
                                                keywords, TRUE, special,
                                                pool));
          SVN_ERR(svn_subst_copy_and_translate3(right,
                                                right_copy,
                                                eol, eol ? TRUE : FALSE, 
                                                keywords, TRUE, special,
                                                pool));

          /* Back up MERGE_TARGET verbatim (it's already in expanded form.) */
          SVN_ERR(svn_io_copy_file(merge_target,
                                   target_copy, TRUE, pool));

          /* Derive the basenames of the 3 backup files. */
          svn_path_split(left_copy, NULL, &left_base, pool);
          svn_path_split(right_copy, NULL, &right_base, pool);
          svn_path_split(target_copy, &parentt, &target_base, pool);
          tmp_entry.conflict_old = left_base;
          tmp_entry.conflict_new = right_base;
          tmp_entry.conflict_wrk = target_base;

          /* Mark merge_target's entry as "Conflicted", and start tracking
             the backup files in the entry as well. */
          SVN_ERR(svn_wc_adm_retrieve(&parent_access, adm_access, parentt,
                                      pool));
          SVN_ERR(svn_wc__entry_modify 
                  (parent_access, mt_bn, &tmp_entry,
                   SVN_WC__ENTRY_MODIFY_CONFLICT_OLD
                   | SVN_WC__ENTRY_MODIFY_CONFLICT_NEW
                   | SVN_WC__ENTRY_MODIFY_CONFLICT_WRK,
                   TRUE, pool));

          *merge_outcome = svn_wc_merge_conflict;

        }
      else if (contains_conflicts && dry_run)
        {
          *merge_outcome = svn_wc_merge_conflict;
        } /* end of conflict handling */
      else
        {
          svn_boolean_t same;
          SVN_ERR(svn_io_files_contents_same_p(&same, result_target,
                                               merge_target, pool));

          *merge_outcome = same ? svn_wc_merge_unchanged : svn_wc_merge_merged;
        }

      if (*merge_outcome != svn_wc_merge_unchanged && ! dry_run)
        {
          /* replace MERGE_TARGET with the new merged file, expanding. */
          SVN_ERR(svn_wc__get_keywords(&keywords, merge_target, adm_access,
                                       NULL, pool));
          SVN_ERR(svn_wc__get_eol_style(NULL, &eol, merge_target, adm_access,
                                        pool));
          SVN_ERR(svn_wc__get_special(&special, merge_target, adm_access,
                                      pool));
          SVN_ERR(svn_subst_copy_and_translate3(result_target, merge_target,
                                                eol, eol ? TRUE : FALSE, 
                                                keywords, TRUE, special,
                                                pool));
        }

    } /* end of merging for text files */

  else if (! dry_run) /* merging procedure for binary files */
    {
      /* ### when making the binary-file backups, should we be honoring
         keywords and eol stuff?   */

      const char *left_copy, *right_copy;
      const char *parentt, *left_base, *right_base;
      svn_wc_adm_access_t *parent_access;
      svn_wc_entry_t tmp_entry;
      
      /* reserve names for backups of left and right fulltexts */
      SVN_ERR(svn_io_open_unique_file2(NULL,
                                       &left_copy,
                                       merge_target,
                                       left_label,
                                       svn_io_file_del_none,
                                       pool));

      SVN_ERR(svn_io_open_unique_file2(NULL,
                                       &right_copy,
                                       merge_target,
                                       right_label,
                                       svn_io_file_del_none,
                                       pool));

      /* create the backup files */
      SVN_ERR(svn_io_copy_file(left,
                               left_copy, TRUE, pool));
      SVN_ERR(svn_io_copy_file(right,
                               right_copy, TRUE, pool));
      
      /* Derive the basenames of the backup files. */
      svn_path_split(left_copy, &parentt, &left_base, pool);
      svn_path_split(right_copy, &parentt, &right_base, pool);
      tmp_entry.conflict_old = left_base;
      tmp_entry.conflict_new = right_base;
      tmp_entry.conflict_wrk = NULL;

      /* Mark merge_target's entry as "Conflicted", and start tracking
         the backup files in the entry as well. */
      SVN_ERR(svn_wc_adm_retrieve(&parent_access, adm_access, parentt, pool));
      SVN_ERR(svn_wc__entry_modify 
              (parent_access, mt_bn, &tmp_entry,
               SVN_WC__ENTRY_MODIFY_CONFLICT_OLD
               | SVN_WC__ENTRY_MODIFY_CONFLICT_NEW
               | SVN_WC__ENTRY_MODIFY_CONFLICT_WRK,
               TRUE, pool));

      *merge_outcome = svn_wc_merge_conflict; /* a conflict happened */

    } /* end of binary conflict handling */
  else
    *merge_outcome = svn_wc_merge_conflict; /* dry_run for binary files. */

  /* Merging is complete.  Regardless of text or binariness, we might
     need to tweak the executable bit on the new working file.  */
  if (! dry_run)
    {
      SVN_ERR(svn_wc__maybe_set_executable(NULL, merge_target, adm_access,
                                           pool));

      SVN_ERR(svn_wc__maybe_set_read_only(NULL, merge_target,
                                          adm_access, pool));

    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_merge(const char *left,
             const char *right,
             const char *merge_target,
             svn_wc_adm_access_t *adm_access,
             const char *left_label,
             const char *right_label,
             const char *target_label,
             svn_boolean_t dry_run,
             enum svn_wc_merge_outcome_t *merge_outcome,
             const char *diff3_cmd,
             apr_pool_t *pool)
{
  return svn_wc_merge2(left, right, merge_target, adm_access,
                       left_label, right_label, target_label,
                       dry_run, merge_outcome, diff3_cmd, NULL, pool);
}
