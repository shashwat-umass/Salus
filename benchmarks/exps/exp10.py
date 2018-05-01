# -*- coding: future_fstrings -*-
"""
OSDI Experiment 10

Have jobs of 5 different JCTs, let them enter the system in order, creating stair shaped memory usage figure.

Scheduler: fair
Work conservation: True
Collected data: memory usage over time
"""
from __future__ import absolute_import, print_function, division, unicode_literals

from absl import flags

from benchmarks.driver.server.config import presets
from benchmarks.driver.workload import WTL
from benchmarks.exps import run_seq, parse_actions_from_cmd, Pause, maybe_forced_preset

FLAGS = flags.FLAGS


def main(argv):
    scfg = maybe_forced_preset(presets.AllocProf)
    if argv:
        run_seq(scfg.copy(output_dir=FLAGS.save_dir),
                *parse_actions_from_cmd(argv))
        return

    run_seq(scfg.copy(output_dir=FLAGS.save_dir),
            WTL.create("resnet50", 50, 265),
            Pause(10),
            WTL.create("googlenet", 100, 200),
            Pause(10),
            WTL.create("inception3", 25, 170),
            Pause(10),
            WTL.create("vgg16", 50, 50),
            Pause(10),
            WTL.create("overfeat", 100, 80),
            )
