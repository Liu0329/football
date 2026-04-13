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


"""Script allowing to play the game by multiple players."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import platform

from absl import app
from absl import flags
from absl import logging
import pygame

from gfootball.env import config
from gfootball.env import football_env

FLAGS = flags.FLAGS

flags.DEFINE_string('players', 'keyboard:left_players=1',
                    'Semicolon separated list of players, single keyboard '
                    'player on the left by default')
flags.DEFINE_string('level', '', 'Level to play')
flags.DEFINE_enum('action_set', 'default', ['default', 'full'], 'Action set')
flags.DEFINE_bool('real_time', True,
                  'If true, environment will slow down so humans can play.')
flags.DEFINE_bool('render', True, 'Whether to do game rendering.')
flags.DEFINE_bool(
    'skip_menu', False,
    'If true, skip the mode selection screen and start match mode directly '
    '(uses --level if set, otherwise default 11v11).')

# Single-player dribble training: centre circle vs opposing keeper only.
_TRAINING_LEVEL = 'solo_center_vs_keeper'
_DEFAULT_MATCH_LEVEL = '11_vs_11_stochastic'


def _menu_font(size):
  """Load a font without SysFont (avoids pygame Windows registry bugs)."""
  if platform.system() == 'Windows':
    fonts_dir = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts')
    for name in ('msyh.ttc', 'msyhbd.ttc', 'simhei.ttf', 'simsun.ttc',
                 'arial.ttf'):
      path = os.path.join(fonts_dir, name)
      if not os.path.isfile(path):
        continue
      try:
        return pygame.font.Font(path, size)
      except (IOError, OSError, pygame.error):
        continue
  return pygame.font.Font(None, size)


def _choose_mode_interactive():
  """Blocking UI: returns 'training' or 'match'."""
  # 注意：不在这里调用 pygame.quit()，因为游戏引擎后续还需要 pygame 保持初始化状态。
  # 若在菜单结束后 quit()，主循环里的 pygame.event.get() 等调用会失效，
  # 导致帧率控制失效、游戏全速运行。
  pygame.init()
  try:
    w, h = 720, 480
    screen = pygame.display.set_mode((w, h))
    pygame.display.set_caption('Google Research Football')
    title_font = _menu_font(40)
    item_font = _menu_font(28)
    hint_font = _menu_font(22)

    def text_surface(s, font, color):
      return font.render(s, True, color)

    options = [
        ('training', '1. 单人训练模式（中圈带球，对门将）'),
        ('match', '2. 比赛模式（11 对 11）'),
    ]
    selected = 0
    rects = []
    pad = 16

    while True:
      screen.fill((24, 28, 36))
      title = text_surface('选择游戏模式', title_font, (240, 240, 245))
      screen.blit(title, title.get_rect(center=(w // 2, 56)))

      rects.clear()
      y0 = 130
      for i, (mode_id, label) in enumerate(options):
        sel = i == selected
        bg = (55, 95, 160) if sel else (45, 50, 62)
        border = (120, 170, 240) if sel else (70, 75, 88)
        surf = text_surface(label, item_font, (250, 250, 252))
        bw, bh = max(560, surf.get_width() + pad * 2), surf.get_height() + pad * 2
        rect = pygame.Rect(0, 0, bw, bh)
        rect.center = (w // 2, y0 + i * (bh + 18))
        rects.append((rect, mode_id))
        pygame.draw.rect(screen, bg, rect)
        pygame.draw.rect(screen, border, rect, 2)
        screen.blit(surf, surf.get_rect(center=rect.center))

      hint = text_surface(
          '方向键 / 鼠标移动选择，Enter 或 左键确认，Esc 退出',
          hint_font, (160, 165, 175))
      screen.blit(hint, hint.get_rect(center=(w // 2, h - 44)))

      pygame.display.flip()

      for event in pygame.event.get():
        if event.type == pygame.QUIT:
          raise KeyboardInterrupt
        if event.type == pygame.KEYDOWN:
          if event.key == pygame.K_ESCAPE:
            raise KeyboardInterrupt
          if event.key == pygame.K_UP:
            selected = (selected - 1) % len(options)
          if event.key == pygame.K_DOWN:
            selected = (selected + 1) % len(options)
          if event.key in (pygame.K_RETURN, pygame.K_KP_ENTER, pygame.K_1,
                           pygame.K_2):
            if event.key == pygame.K_1:
              return 'training'
            if event.key == pygame.K_2:
              return 'match'
            return options[selected][0]
        if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
          for rect, mode_id in rects:
            if rect.collidepoint(event.pos):
              return mode_id
        if event.type == pygame.MOUSEMOTION:
          for i, (rect, _) in enumerate(rects):
            if rect.collidepoint(event.pos):
              selected = i

      pygame.time.wait(16)
  finally:
    # 只隐藏菜单窗口，不调用 pygame.quit()，让 pygame 保持初始化供游戏使用。
    # 游戏引擎的渲染窗口会在 env.render() 时重新创建。
    pygame.display.quit()


def main(_):
  players = FLAGS.players.split(';') if FLAGS.players else ''
  assert not (any(['agent' in player for player in players])
             ), ('Player type \'agent\' can not be used with play_game.')

  if FLAGS.render and not FLAGS.skip_menu:
    try:
      mode = _choose_mode_interactive()
    except KeyboardInterrupt:
      logging.info('Exited from mode menu.')
      return
    if mode == 'training':
      level = _TRAINING_LEVEL
    else:
      level = FLAGS.level if FLAGS.level else _DEFAULT_MATCH_LEVEL
  else:
    level = FLAGS.level if FLAGS.level else _DEFAULT_MATCH_LEVEL

  cfg_values = {
      'action_set': FLAGS.action_set,
      'dump_full_episodes': True,
      'players': players,
      'real_time': FLAGS.real_time,
      'level': level,
  }
  cfg = config.Config(cfg_values)
  env = football_env.FootballEnv(cfg)
  if FLAGS.render:
    env.render()
  env.reset()
  try:
    while True:
      for event in pygame.event.get(pygame.QUIT):
        raise KeyboardInterrupt
      keys = pygame.key.get_pressed()
      if keys[pygame.K_ESCAPE]:
        raise KeyboardInterrupt
      _, _, done, _ = env.step([])
      if done:
        env.reset()
  except KeyboardInterrupt:
    logging.warning('Game stopped, writing dump...')
    env.write_dump('shutdown')
    exit(1)


if __name__ == '__main__':
  app.run(main)
