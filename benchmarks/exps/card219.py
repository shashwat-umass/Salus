# -*- coding: future_fstrings -*-
#
# Copyright 2019 Peifeng Yu <peifeng@umich.edu>
# 
# This file is part of Salus
# (see https://github.com/SymbioticLab/Salus).
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# 
#    http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
"""
Server hang when adding log(info). See card#219

Scheduler: fair
Work conservation: True
Collected data: memory usage over time
"""
from __future__ import absolute_import, print_function, division, unicode_literals

from absl import flags

from benchmarks.driver.runner import Executor
from benchmarks.driver.server.config import presets
from benchmarks.driver.workload import WTL
from benchmarks.exps import run_seq, maybe_forced_preset, Pause


FLAGS = flags.FLAGS


def case1():
    scfg = maybe_forced_preset(presets.MostEfficient)

    # BUG: seems we must run a single job first otherwise it will hang
    run_seq(scfg.copy(output_dir=FLAGS.save_dir/'case1'),
            WTL.create("vae", 128, 20))


def case2():
    scfg = maybe_forced_preset(presets.MostEfficient)

    # BUG: seems we must run a single job first otherwise it will hang
    run_seq(scfg.copy(output_dir=FLAGS.save_dir/'case2'),
            WTL.create("super_res", 128, 20))


def test():
    scfg = maybe_forced_preset(presets.MostEfficient)

    # BUG: seems we must run a single job first otherwise it will hang
    run_seq(scfg.copy(output_dir=FLAGS.save_dir/'test'),
            WTL.create("alexnet", 25, 20))


def main(argv):
    command = argv[0] if argv else "case1"

    {
        "case1": case1,
        "case2": case2,
        "test": test,
    }[command]()
