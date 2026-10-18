# SPDX-License-Identifier: Apache-2.0
"""identity and authentication synthetic login flow. Screens and logs contain no secrets."""
import json
import time
from AsteriaVm.Drivers.ProductDesktop import chord, edge
from AsteriaVm import Display, LoginModel


def press(context,key):
    context.qmp.execute('input-send-event',{'events':[edge(key,True)]})
    time.sleep(0.12 if context.case.isa=='Ia64' else 0.04)
    context.qmp.execute('input-send-event',{'events':[edge(key,False)]})
    time.sleep(0.12 if context.case.isa=='Ia64' else 0.04)


def text(context,value):
    # Test credentials exist only in this harness and the fixture writer;
    # QMP events are deliberately never written to an artifact or a log.
    codes={'-':'minus'}
    for character in value:
        shifted=character.isupper()
        if shifted: context.qmp.execute('input-send-event',{'events':[edge('shift',True)]})
        press(context,codes.get(character,character.lower()))
        if shifted: context.qmp.execute('input-send-event',{'events':[edge('shift',False)]})


def state(context,value,edit=None,since=0):
    prefix='USER:# login: state %d edit '%value
    found=context.wait_for_line(lambda line:line=='USER:# login: Recovery; seat locked' or
        (line.startswith(prefix) and (edit is None or line.startswith(prefix+str(edit)+' '))),180,since=since)
    context.require('login.expected-state.%d'%value,found[1].startswith(prefix),'seat entered Recovery')
    return found


def screenshot(context,name,value,edit=0,account='',masks=0,password_focus=False):
    directory=context.target.root/'Build/Tmp/Login'
    directory.mkdir(parents=True,exist_ok=True)
    ppm=directory/(context.case.isa+'-'+name+'.ppm')
    deadline=time.monotonic()+context.seconds(30)
    equal=False
    while time.monotonic()<deadline:
        context.qmp.screendump(ppm,format='ppm')
        width,height,pixels=Display.parse_ppm(ppm.read_bytes())
        expected=LoginModel.render(context.target.root,width,height,value,edit,account,masks,password_focus)
        if pixels==expected:equal=True;break
        time.sleep(.1)
    context.check('login.screen.'+name,equal)
    context.qmp.screendump(directory/(context.case.isa+'-'+name+'.png'),format='png')
    (directory/(context.case.isa+'-'+name+'.json')).write_text(json.dumps({
        'state':value,'edit':edit,'account':account,'mask_characters':masks,'password_focus':password_focus,
        'retry_seconds':1 if edit==2 else 0,
        'width':width,'height':height,'matches':equal},indent=2)+'\n')


def editing(context,since=0):
    context.wait_for_line(lambda line:line=='USER:# login: editing ready',180,since=since)


def run(context):
    state(context,0)
    context.wait_for_line(lambda line:line=='USER:# login: negative probe focus refused; trusted writes sent',60)
    time.sleep(1)
    context.qmp.execute('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':50}},{'type':'btn','data':{'down':True,'button':'left'}},{'type':'btn','data':{'down':False,'button':'left'}}]})
    screenshot(context,'AwaitingAttention',0)
    chord(context)
    state(context,1,0)
    editing(context)
    text(context,'administrator')
    press(context,'tab')
    text(context,'wrong')
    screenshot(context,'Login-masked',1,account='administrator',masks=5,password_focus=True)
    start=len(context.console.lines)
    press(context,'ret')
    state(context,1,2,since=start)
    screenshot(context,'RetryAt',1,2,account='administrator')
    state(context,1,0,since=start)
    editing(context,since=start)
    press(context,'tab')
    text(context,'Asteria-test-admin-036')
    press(context,'ret')
    state(context,3)
    chord(context)
    state(context,4)
    screenshot(context,'Locked',4,account='administrator',password_focus=True)
    start=len(context.console.lines)
    chord(context)
    state(context,4,since=start)
    context.check('login.second-sas-remains-locked',not any(line.startswith('USER:# login: state 3 ') for line in context.console.lines[start:]))
    editing(context,since=start)
    text(context,'Asteria-test-admin-036')
    press(context,'ret')
    state(context,3,since=start)
    chord(context)
    start,_=state(context,4,since=start)
    press(context,'f12')
    state(context,0,since=start)
    secrets=('Asteria-test-admin-036','Asteria-test-user-036','wrong')
    context.check('login.logs-secret-free',all(not any(secret in line for secret in secrets) for line in context.console.lines))
    start=len(context.console.lines)
    chord(context);state(context,1,0,since=start);editing(context,since=start)
    text(context,'administrator');press(context,'tab');text(context,'Asteria-test-admin-036');press(context,'ret')
    context.wait_for_line(lambda line:line=='USER:# login: password snapshot held by auth only',180,since=start)
    cancelled=len(context.console.lines)
    chord(context);state(context,1,0,since=cancelled)
    press(context,'esc');state(context,0,since=cancelled)
    context.wait_for_line(lambda line:line.startswith('USER:# auth: ') and 'status=14 ' in line and 'wipe=1' in line,600,since=start)
    context.check('login.auth-cancellation-cleanup',True)
    hashes=[line for line in context.console.lines if line.startswith('USER:# auth: ')]
    (context.target.root/'Build/Tmp/Login'/(context.case.isa+'-Hashing.json')).write_text(json.dumps(hashes,indent=2)+'\n')
    context.check('login.full-profile-success-failure',any('status=0 ' in line for line in hashes) and any('status=10 ' in line for line in hashes))
    context.check('login.logs-secret-free-after-cancel',all(not any(secret in line for secret in secrets) for line in context.console.lines))
    context.check('login.flow',True)
