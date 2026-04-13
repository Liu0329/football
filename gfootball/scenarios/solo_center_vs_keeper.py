# coding=utf-8
# Copyright 2019 Google LLC
#
# Solo dribble training: one outfield player with ball at centre circle,
# opposing goalkeeper only (no extra defenders).


from . import *


def build_scenario(builder):
  builder.config().game_duration = 600
  builder.config().deterministic = False
  builder.config().offsides = False
  builder.config().end_episode_on_score = True
  builder.config().end_episode_on_out_of_play = True
  builder.config().end_episode_on_possession_change = True
  # Centre spot (same convention as other academy scenarios).
  builder.SetBallPosition(0.0, 0.0)

  builder.SetTeam(Team.e_Left)
  builder.AddPlayer(-1.0, 0.0, e_PlayerRole_GK)
  builder.AddPlayer(0.0, 0.0, e_PlayerRole_CB)

  builder.SetTeam(Team.e_Right)
  # Right team coordinates use the same "left-goal = x=-1" convention;
  # the engine mirrors the right team automatically to the right side.
  builder.AddPlayer(-1.0, 0.0, e_PlayerRole_GK)
