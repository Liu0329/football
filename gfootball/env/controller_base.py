# coding=utf-8
# Copyright 2019 Google LLC
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


"""Base controller class."""

from gfootball.env import football_action_set
from gfootball.env import player_base


class Controller(player_base.PlayerBase):
  """Base controller class."""

  def __init__(self, player_config, env_config):
    player_base.PlayerBase.__init__(self, player_config)
    self._active_actions = {}
    self._env_config = env_config
    self._last_action = football_action_set.action_idle
    self._last_direction = football_action_set.action_idle
    self._current_direction = football_action_set.action_idle

  # C++ 的 ResetNotSticky 每帧清除 pass/shot，所以按住时需要每帧重发。
  # sprint/dribble/pressure 等不被 ResetNotSticky 清除，不需要重发。
  _STICKY_SEND_ACTIONS = None

  @classmethod
  def _get_sticky_actions(cls):
    if cls._STICKY_SEND_ACTIONS is None:
      cls._STICKY_SEND_ACTIONS = {
          football_action_set.action_shot,
          football_action_set.action_short_pass,
          football_action_set.action_long_pass,
          football_action_set.action_high_pass,
      }
    return cls._STICKY_SEND_ACTIONS

  def _check_action(self, action, active_actions):
    """Compare (and update) controller's state with the set of active actions.

    Args:
      action: Action to check
      active_actions: Set of all active actions
    """
    assert isinstance(action, football_action_set.CoreAction)
    if not action.is_in_actionset(self._env_config):
      return
    state = active_actions.get(action, 0)
    # pass/shot 被 ResetNotSticky 每帧清除，按住时需要每帧重发。
    # 只在 last_action==idle 时才占用（不阻塞同帧其他 release）。
    if action in self._get_sticky_actions() and state:
      if self._last_action == football_action_set.action_idle:
        self._active_actions[action] = state
        self._last_action = action
      return
    # 其他按键：只在状态变化时发送（按下/松开各一次）
    if (self._last_action == football_action_set.action_idle and
        self._active_actions.get(action, 0) != state):
      self._active_actions[action] = state
      if state:
        self._last_action = action
      else:
        self._last_action = football_action_set.disable_action(action)
        assert self._last_action

  def _check_direction(self, action, state):
    """Compare (and update) controller's direction with the current direction.

    Args:
      action: Action to check
      state: Current state of the action being checked
    """
    assert isinstance(action, football_action_set.CoreAction)
    if not action.is_in_actionset(self._env_config):
      return
    if self._current_direction != football_action_set.action_idle:
      return
    if state:
      self._current_direction = action

  def get_env_action(self, left, right, top, bottom, active_actions):
    """For a given controller's state generate next environment action.

    Args:
      action: Action to check
      state: Current state of the action being checked
    """
    self._current_direction = football_action_set.action_idle
    self._check_direction(football_action_set.action_top_left, top and
                          left)
    self._check_direction(football_action_set.action_top_right, top and
                          right)
    self._check_direction(football_action_set.action_bottom_left,
                          bottom and left)
    self._check_direction(football_action_set.action_bottom_right,
                          bottom and right)
    if self._current_direction == football_action_set.action_idle:
      self._check_direction(football_action_set.action_right, right)
      self._check_direction(football_action_set.action_left, left)
      self._check_direction(football_action_set.action_top, top)
      self._check_direction(football_action_set.action_bottom, bottom)
    if self._current_direction != self._last_direction:
      self._last_direction = self._current_direction
      if self._current_direction == football_action_set.action_idle:
        return football_action_set.action_release_direction
      else:
        return self._current_direction
    self._last_action = football_action_set.action_idle
    # sprint/dribble 的 release 优先：它们在 C++ 里是 sticky（不被 ResetNotSticky 清除），
    # 如果 release 被 pass sticky 吞掉，C++ 就永远以为 sprint 按着 → 锁死。
    # 先检查它们的 release，再检查其他 action。
    for act in [football_action_set.action_sprint,
                football_action_set.action_dribble,
                football_action_set.action_pressure,
                football_action_set.action_team_pressure,
                football_action_set.action_keeper_rush]:
      if not act.is_in_actionset(self._env_config):
        continue
      s = active_actions.get(act, 0)
      if not s and self._active_actions.get(act, 0):
        self._active_actions[act] = 0
        self._last_action = football_action_set.disable_action(act)
        return self._last_action
    self._check_action(football_action_set.action_long_pass,
                       active_actions)
    self._check_action(football_action_set.action_high_pass,
                       active_actions)
    self._check_action(football_action_set.action_short_pass,
                       active_actions)
    self._check_action(football_action_set.action_shot, active_actions)
    self._check_action(football_action_set.action_keeper_rush,
                       active_actions)
    self._check_action(football_action_set.action_sliding, active_actions)
    self._check_action(football_action_set.action_pressure,
                       active_actions)
    self._check_action(football_action_set.action_team_pressure,
                       active_actions)
    self._check_action(football_action_set.action_switch, active_actions)
    self._check_action(football_action_set.action_sprint, active_actions)
    self._check_action(football_action_set.action_dribble, active_actions)
    return self._last_action
