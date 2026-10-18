# SPDX-License-Identifier: Apache-2.0
"""USB ESP test boot without the scripted HID scenario's runner injection."""
from AsteriaVm import Display, Guest, Ps2
from AsteriaVm.Drivers import Production


def run(context):
    reached, detail = Production._amd64(context)
    context.check('lifecycle.user-idle', reached, detail)
    absent = '# input: no scripted xHCI input arrived, hardware demo skipped'
    context.wait_for_line('USER:' + absent, 60.0)
    context.wait_for_line('USER:# input: type or click; keys (US layout) and buttons are echoed here', 10.0)
    mode = next(Display.parse_start(line) for line in Guest.channel_lines(context.console.lines)
                if Display.parse_start(line) is not None)
    width, height = mode[:2]
    initial_x, initial_y = Display.pointer_position(Guest.channel_lines(context.console.lines), width, height)
    Ps2.inject_terminal(context)
    for line in ('asteria', '[L-- 5,-3]', '[--- 5,-3]'):
        context.wait_for_line('USER:' + line, 20.0)
    channel = Guest.channel_lines(context.console.lines)
    context.check('xhci.hardware-fallback', absent in channel and '# root service: done' not in channel,
                  'absent script leaves the test pager echoing real USB devices')
    serial = context.artifact('Serial.log').read_text(errors='replace').splitlines()
    context.check('xhci.hardware-serial', all('USER:' + line in serial
                  for line in ('asteria', '[L-- 5,-3]', '[--- 5,-3]')))
    wanted = initial_x + 5, initial_y - 3
    context.wait_for_line(lambda line: line.startswith('USER:# pointer:')
                          and Display.pointer_position([line[5:]], width, height) == wanted, 20.0)
    context.check('xhci.hardware-pointer',
                  Display.pointer_position(Guest.channel_lines(context.console.lines), width, height) == wanted,
                  'real mouse movement follows the synthetic broker demonstration')
    Production.repeat_terminal(context)
    Production._display(context)
    Production._pointer_checks(context, initial_y=wanted[1])
