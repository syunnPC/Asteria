# SPDX-License-Identifier: Apache-2.0
"""Synthetic receipt authorizes root's bounded service-failure injection."""
import time
from AsteriaVm.Drivers.Login import state,editing,press,text,screenshot
from AsteriaVm import Display
from AsteriaVm.Drivers.ProductDesktop import chord


def run(context):
    state(context,0)
    context.wait_for_line(lambda line:line=='USER:# login: negative probe focus refused; trusted writes sent',60)
    chord(context);state(context,1,0);editing(context)
    text(context,'administrator');press(context,'tab');text(context,'incomplete')
    begin=len(context.console.lines)
    press(context,{'auth':'f9','login':'f10','display':'f11'}[context.case.login_failure])
    context.wait_for_line(lambda line:line=='USER:# login: Recovery; seat locked',60,since=begin)
    if context.case.login_failure=='display':
        context.wait_for_line(lambda line:line=='USER:# login: dead display covered; seat locked',60,since=begin)
        ppm=context.artifact('Recovery-display.ppm');context.qmp.screendump(ppm,format='ppm')
        width,height,pixels=Display.parse_ppm(ppm.read_bytes())
        context.check('login.failure.display.opaque-cover',pixels==bytes((16,32,64))*(width*height))
    time.sleep(.5)
    if context.case.login_failure=='auth':screenshot(context,'Failure-auth',6)
    chord(context)
    time.sleep(.5)
    context.check('login.failure.'+context.case.login_failure+'.seat-locked',
        not any(line.startswith('USER:# login: state 3 ') for line in context.console.lines[begin:]))
