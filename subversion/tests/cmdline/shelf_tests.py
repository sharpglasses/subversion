#!/usr/bin/env python
#
#  shelve_tests.py:  testing shelving
#
#  Subversion is a tool for revision control.
#  See http://subversion.apache.org for more information.
#
# ====================================================================
#    Licensed to the Apache Software Foundation (ASF) under one
#    or more contributor license agreements.  See the NOTICE file
#    distributed with this work for additional information
#    regarding copyright ownership.  The ASF licenses this file
#    to you under the Apache License, Version 2.0 (the
#    "License"); you may not use this file except in compliance
#    with the License.  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing,
#    software distributed under the License is distributed on an
#    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
#    KIND, either express or implied.  See the License for the
#    specific language governing permissions and limitations
#    under the License.
######################################################################

# General modules
import shutil, stat, re, os, logging

logger = logging.getLogger()

# Our testing module
import svntest
from svntest import wc

# (abbreviation)
Skip = svntest.testcase.Skip_deco
SkipUnless = svntest.testcase.SkipUnless_deco
XFail = svntest.testcase.XFail_deco
Issues = svntest.testcase.Issues_deco
Issue = svntest.testcase.Issue_deco
Wimp = svntest.testcase.Wimp_deco
Item = wc.StateItem

#----------------------------------------------------------------------

def state_from_status(wc_dir):
  _, output, _ = svntest.main.run_svn(None, 'status', '-v', '-u', '-q',
                                      wc_dir)
  return svntest.wc.State.from_status(output, wc_dir)

def shelve_unshelve_verify(sbox, modifier):
  """Round-trip: shelve; verify all changes are reverted;
     unshelve; verify all changes are restored.
  """

  wc_dir = sbox.wc_dir
  virginal_state = state_from_status(wc_dir)

  # Make some changes to the working copy
  modifier(sbox)

  # Save the modified state
  modified_state = state_from_status(wc_dir)

  # Shelve; check there are no longer any modifications
  svntest.actions.run_and_verify_svn(None, [],
                                     'shelve', 'foo')
  svntest.actions.run_and_verify_status(wc_dir, virginal_state)

  # Unshelve; check the original modifications are here again
  svntest.actions.run_and_verify_svn(None, [],
                                     'unshelve', 'foo')
  svntest.actions.run_and_verify_status(wc_dir, modified_state)

#----------------------------------------------------------------------

def shelve_unshelve(sbox, modifier):
  """Round-trip: build 'sbox'; apply changes by calling 'modifier(sbox)';
     shelve and unshelve; verify changes are fully reverted and restored.
  """

  if not sbox.is_built():
    sbox.build()
  was_cwd = os.getcwd()
  os.chdir(sbox.wc_dir)
  sbox.wc_dir = ''

  shelve_unshelve_verify(sbox, modifier)

  os.chdir(was_cwd)

######################################################################
# Tests
#
#   Each test must return on success or raise on failure.

def shelve_text_mods(sbox):
  "shelve text mods"

  def modifier(sbox):
    sbox.simple_append('A/mu', 'appended mu text')

  shelve_unshelve(sbox, modifier)

#----------------------------------------------------------------------

def shelve_prop_changes(sbox):
  "shelve prop changes"

  def modifier(sbox):
    sbox.simple_propset('p', 'v', 'A')
    sbox.simple_propset('p', 'v', 'A/mu')

  shelve_unshelve(sbox, modifier)

#----------------------------------------------------------------------

def shelve_adds(sbox):
  "shelve adds"

  def modifier(sbox):
    sbox.simple_add_text('A new file\n', 'A/new')
    sbox.simple_add_text('A new file\n', 'A/new2')
    sbox.simple_propset('p', 'v', 'A/new2')

  shelve_unshelve(sbox, modifier)

#----------------------------------------------------------------------

@Issue(4709)
def shelve_deletes(sbox):
  "shelve deletes"

  def modifier(sbox):
    sbox.simple_rm('A/mu')

  shelve_unshelve(sbox, modifier)

#----------------------------------------------------------------------

@XFail()
def shelve_empty_adds(sbox):
  "shelve empty adds"
  sbox.build(empty=True)

  def modifier(sbox):
    sbox.simple_add_text('', 'empty')
    sbox.simple_add_text('', 'empty-with-prop')
    sbox.simple_propset('p', 'v', 'empty-with-prop')

  shelve_unshelve(sbox, modifier)

#----------------------------------------------------------------------

@XFail()
def shelve_empty_deletes(sbox):
  "shelve empty deletes"
  sbox.build(empty=True)
  sbox.simple_add_text('', 'empty')
  sbox.simple_add_text('', 'empty-with-prop')
  sbox.simple_propset('p', 'v', 'empty-with-prop')
  sbox.simple_commit()

  def modifier(sbox):
    sbox.simple_rm('empty', 'empty-with-prop')

  shelve_unshelve(sbox, modifier)

#----------------------------------------------------------------------

def shelve_from_inner_path(sbox):
  "shelve from inner path"

  def modifier(sbox):
    sbox.simple_append('A/mu', 'appended mu text')

  sbox.build()
  was_cwd = os.getcwd()
  os.chdir(sbox.ospath('A'))
  sbox.wc_dir = '..'

  shelve_unshelve_verify(sbox, modifier)

  os.chdir(was_cwd)

#----------------------------------------------------------------------

def save_revert_restore(sbox, modifier1, modifier2):
  "Save 2 checkpoints; revert; restore 1st"

  sbox.build()
  was_cwd = os.getcwd()
  os.chdir(sbox.wc_dir)
  sbox.wc_dir = ''
  wc_dir = ''

  # Make some changes to the working copy
  modifier1(sbox)

  # Remember the modified state
  modified_state1 = state_from_status(wc_dir)

  # Save a checkpoint; check nothing changed
  svntest.actions.run_and_verify_svn(None, [],
                                     'shelf-save', 'foo')
  svntest.actions.run_and_verify_status(wc_dir, modified_state1)

  # Modify again; remember the state; save a checkpoint
  modifier2(sbox)
  modified_state2 = state_from_status(wc_dir)
  svntest.actions.run_and_verify_svn(None, [],
                                     'shelf-save', 'foo')
  svntest.actions.run_and_verify_status(wc_dir, modified_state2)

  # Revert
  svntest.actions.run_and_verify_svn(None, [],
                                     'revert', '-R', '.')
  virginal_state = svntest.actions.get_virginal_state(wc_dir, 1)
  svntest.actions.run_and_verify_status(wc_dir, virginal_state)

  # Restore; check the original modifications are here again
  svntest.actions.run_and_verify_svn(None, [],
                                     'unshelve', 'foo', '1')
  svntest.actions.run_and_verify_status(wc_dir, modified_state1)

  os.chdir(was_cwd)

#----------------------------------------------------------------------

def checkpoint_basic(sbox):
  "checkpoint basic"

  def modifier1(sbox):
    sbox.simple_append('A/mu', 'appended mu text\n')

  def modifier2(sbox):
    sbox.simple_append('iota', 'appended iota text\n')
    sbox.simple_append('A/mu', 'appended another line\n')

  save_revert_restore(sbox, modifier1, modifier2)

#----------------------------------------------------------------------

@Issue(3747)
def shelve_mergeinfo(sbox):
  "shelve mergeinfo"

  def modifier(sbox):
    sbox.simple_propset('svn:mergeinfo', '/trunk/A:1-3,10', 'A')
    sbox.simple_propset('svn:mergeinfo', '/trunk/A/mu:1-3,10', 'A/mu')

  shelve_unshelve(sbox, modifier)

#----------------------------------------------------------------------

def unshelve_refuses_if_conflicts(sbox):
  "unshelve refuses if conflicts"

  def modifier1(sbox):
    sbox.simple_append('alpha', 'A-mod1\nB\nC\nD\n', truncate=True)
    sbox.simple_append('beta', 'A-mod1\nB\nC\nD\n', truncate=True)

  def modifier2(sbox):
    sbox.simple_append('beta', 'A-mod2\nB\nC\nD\n', truncate=True)

  sbox.build(empty=True)
  was_cwd = os.getcwd()
  os.chdir(sbox.wc_dir)
  sbox.wc_dir = ''
  wc_dir = ''

  sbox.simple_add_text('A\nB\nC\nD\n', 'alpha')
  sbox.simple_add_text('A\nB\nC\nD\n', 'beta')
  sbox.simple_commit()
  initial_state = state_from_status(wc_dir)

  # Make initial mods; remember this modified state
  modifier1(sbox)
  modified_state1 = state_from_status(wc_dir)
  assert modified_state1 != initial_state

  # Shelve; check there are no longer any local mods
  svntest.actions.run_and_verify_svn(None, [],
                                     'shelve', 'foo')
  svntest.actions.run_and_verify_status(wc_dir, initial_state)

  # Make a different local mod that will conflict with the shelf
  modifier2(sbox)
  modified_state2 = state_from_status(wc_dir)

  # Try to unshelve; check it fails with an error about a conflict
  svntest.actions.run_and_verify_svn(None, '.*[Cc]onflict.*',
                                     'unshelve', 'foo')
  # Check nothing changed in the attempt
  svntest.actions.run_and_verify_status(wc_dir, modified_state2)

#----------------------------------------------------------------------

@Skip()  # fails on MacOSX; unknown reason, maybe related to global state
def shelve_binary_file_mod(sbox):
  "shelve binary file mod"

  sbox.build(empty=True)
  sbox.simple_add_text('\0\1\2\3\4\5', 'bin')
  sbox.simple_commit()
  sbox.simple_update()

  def modifier(sbox):
    sbox.simple_append('bin', '\5\4\3\2\1\0', truncate=True)

  shelve_unshelve(sbox, modifier)

#----------------------------------------------------------------------

def shelve_binary_file_add(sbox):
  "shelve binary file add"

  def modifier(sbox):
    sbox.simple_add_text('\0\1\2\3\4\5', 'bin')

  shelve_unshelve(sbox, modifier)

#----------------------------------------------------------------------

def shelve_binary_file_del(sbox):
  "shelve binary file del"

  sbox.build(empty=True)
  sbox.simple_add_text('\0\1\2\3\4\5', 'bin')
  sbox.simple_commit()
  sbox.simple_update()

  def modifier(sbox):
    sbox.simple_rm('bin')

  shelve_unshelve(sbox, modifier)

#----------------------------------------------------------------------

def shelve_with_log_message(sbox):
  "shelve with log message"

  sbox.build(empty=True)
  was_cwd = os.getcwd()
  os.chdir(sbox.wc_dir)
  sbox.wc_dir = ''

  sbox.simple_add_text('New file', 'f')
  log_message = 'Log message for foo'
  svntest.actions.run_and_verify_svn(None, [],
                                     'shelve', 'foo', '-m', log_message)
  expected_output = svntest.verify.RegexListOutput(
    ['foo .*',
     ' ' + log_message
    ])
  svntest.actions.run_and_verify_svn(expected_output, [],
                                     'shelf-list')

  os.chdir(was_cwd)


########################################################################
# Run the tests

# list all tests here, starting with None:
test_list = [ None,
              shelve_text_mods,
              shelve_prop_changes,
              shelve_adds,
              shelve_deletes,
              shelve_empty_adds,
              shelve_empty_deletes,
              shelve_from_inner_path,
              checkpoint_basic,
              shelve_mergeinfo,
              unshelve_refuses_if_conflicts,
              shelve_binary_file_mod,
              shelve_binary_file_add,
              shelve_binary_file_del,
              shelve_with_log_message,
             ]

if __name__ == '__main__':
  svntest.main.run_tests(test_list)
  # NOTREACHED


### End of file.
