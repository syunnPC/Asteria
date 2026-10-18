# SPDX-License-Identifier: Apache-2.0
"""real toolkit client, native pools, input and render-model captures."""
import json
import re
import struct
import subprocess
import time
import zlib
from AsteriaVm import Display, Guest, Session, RenderCorpus
from AsteriaVm.Drivers import ProductDesktop
from AsteriaVm.Drivers.SessionCompositor import complete_snapshot


def demo_window(lines):
    windows = {}
    for line in Session.complete_transcript(Guest.root_service_lines(lines)):
        words = line.split()
        if words[:3] == ['#', 'session:', 'window']:
            values = list(map(int, words[3:]))
            windows[values[0]] = values
        elif words[:3] == ['#', 'session:', 'title'] and bytes.fromhex(words[4]) == b'toolkit-demo':
            return windows[int(words[3])]
    raise ValueError('no complete toolkit window snapshot')


def crop(rgb, width, rect):
    x, y, w, h = rect
    return b''.join(rgb[((y+row)*width+x)*3:((y+row)*width+x+w)*3] for row in range(h))


def png(width, height, rgb):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind+data))
    scan = b''.join(b'\0'+rgb[y*width*3:(y+1)*width*3] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB',width,height,8,2,0,0,0))
            + chunk(b'IDAT', zlib.compress(scan)) + chunk(b'IEND',b''))


def render(repository, width, height, state, fallback=False):
    candidates = [repository/'Build/CMake'/preset/directory/'ToolkitRender'
                  for preset in ('host-debug','host-release','amd64','ia64')
                  for directory in ('Tools','HostTools')]
    tool = next((path for path in candidates if path.is_file()), None)
    if tool is None:
        raise ValueError('build ToolkitRender before running toolkit evidence')
    font = '-' if fallback else str(repository/'Data/Fonts/Vegur/Vegur-Regular.otf')
    output = subprocess.run([str(tool),str(width),str(height),font,state],check=True,stdout=subprocess.PIPE)
    return Display.parse_ppm(output.stdout)[2]


def cursor_pixels(repository, rgb, width, height, position):
    """The bottom/right capture fence clips the cursor to native scanout."""
    cw,ch,hot,pixels = Display.load_cursor(repository)
    errors = []
    for y in range(ch):
        for x in range(cw):
            px,py = position[0]+x-hot[0],position[1]+y-hot[1]
            pixel = pixels[y*cw+x]
            if pixel>>24 != 255 or not (0 <= px < width and 0 <= py < height):
                continue
            at = (py*width+px)*3
            if rgb[at:at+3] != bytes((pixel>>16&255,pixel>>8&255,pixel&255)):
                errors.append((x,y))
    return errors


def run(context):
    context.wait_for_line('USER:# session: desktop ',180)
    context.wait_for_line('USER:# toolkit: component admitted',30)
    context.wait_for_line('USER:# toolkit: ready',90)
    context.wait_for_line('USER:# toolkit: font Vegur-Regular',30)
    _,storage = context.wait_for_line('USER:# toolkit: pools ',30)
    match = re.fullmatch(r'USER:# toolkit: pools (\d+)x(\d+) pages=(\d+) frames=(\d+) workspace-pages=(\d+)',storage)
    context.require('toolkit.native-pool-plan',match is not None,storage)
    sw,sh,pages,frames,workspace = map(int,match.groups())
    context.check('toolkit.complete-double-buffer-storage',(sw,sh)==context.case.expected_display
                  and pages==(sw*sh*4+4095)//4096 and 2*((pages+511)//512) <= frames <= 32 and workspace <= 512)
    context.wait_for_line('USER:# toolkit: event 8 ',90)
    width,height = context.case.expected_display
    position = [width//2,height//2]
    events = []
    def send(batch):
        context.qmp.execute('input-send-event',dict(events=batch))
        events.append(batch)
        time.sleep(0.25 if context.case.isa == 'Ia64' else 0.06)
    def move(x,y):
        while position != [x,y]:
            dx,dy = [max(-60,min(60,wanted-old)) for wanted,old in zip((x,y),position)]
            send([dict(type='rel',data=dict(axis='x',value=dx)),dict(type='rel',data=dict(axis='y',value=dy))])
            position[:] = [position[0]+dx,position[1]+dy]
    def button(down):
        send([dict(type='btn',data=dict(down=down,button='left'))])
    def click(x,y):
        move(x,y); button(True); button(False)
    def key(qcode,down):
        send([ProductDesktop.edge(qcode,down)])
    def capture(name,state):
        move(width-1,height-1)
        deadline = time.monotonic()+context.seconds(10)
        pointer = context.artifact('Pointer.ppm')
        while True:
            context.qmp.screendump(pointer,format='ppm')
            pw,ph,prgb = Display.parse_ppm(pointer.read_bytes())
            errors = cursor_pixels(context.target.root,prgb,pw,ph,position)
            if not errors or time.monotonic() >= deadline: break
            time.sleep(0.1)
        context.check('toolkit.'+name+'.pointer-fence',(pw,ph)==(width,height) and not errors,
                      '%d visible opaque cursor pixels differ' % len(errors))
        complete_snapshot(context,len(context.console.lines)-1)
        context.stop()
        try:
            window = demo_window(context.console.lines)
            _,x,y,fw,fh,_,_,iw,ih,serial,*flags = window
            path = context.artifact(name+'.ppm'); context.qmp.screendump(path,format='ppm')
            w,h,rgb = Display.parse_ppm(path.read_bytes())
            context.require('toolkit.'+name+'.native',(w,h)==(width,height))
            actual = crop(rgb,width,(x+4,y+25,iw,ih))
            expected = render(context.target.root,iw,ih,state)
            context.artifact(name+'-model.ppm').write_bytes(Display.write_ppm(iw,ih,expected))
            differences = sum(a != b for a,b in zip(actual,expected))
            context.check('toolkit.'+name+'.render-model',len(actual)==len(expected) and differences==0,
                          '%d differing channels; image serial %d' % (differences,serial))
            context.artifact(name+'.png').write_bytes(png(fw,fh,crop(rgb,width,(x,y,fw,fh))))
            evidence = context.target.root/'Build/Tmp/ToolkitSession'
            evidence.mkdir(parents=True,exist_ok=True)
            (evidence/(context.case.isa+'-'+name+'.png')).write_bytes(context.artifact(name+'.png').read_bytes())
            return window
        finally:
            context.qmp.cont()
    initial = capture('initial','initial')
    _,x,y,fw,fh,*_ = initial
    # Compact DemoForm cells (column, row): button (0,0), checkbox (2,0),
    # textbox (3,1) and the two-row list (0,4); 10-pixel cell margins.
    header = (context.target.root/'Source/User/Toolkit/SessionComponent.hpp').read_text()
    theme = (context.target.root/'Source/User/Toolkit/Theme.hpp').read_text()
    cell_width = RenderCorpus.header_constant(header,'DemoCellWidth')
    row_height = RenderCorpus.header_constant(header,'DemoRowHeight')
    def cell(column,row,dx,dy):
        return x+4+column*cell_width+10+dx, y+25+row*row_height+10+dy
    click(*cell(0,0,20,14))
    context.wait_for_line('USER:# toolkit: button',30)
    click(*cell(3,1,20,14))
    key('ctrl',True); key('a',True); key('a',False); key('ctrl',False)
    for character in 'asteria':
        key(character,True); key(character,False)
    context.wait_for_line('USER:# toolkit: text '+b'asteria'.hex(),30)
    context.check('toolkit.us-text',True)
    click(*cell(2,0,20,14))
    context.wait_for_line('USER:# toolkit: check 1',30)
    # The second item: below the list border and the first item.
    item = RenderCorpus.header_constant(theme,'ItemHeight')
    click(*cell(0,4,20,RenderCorpus.header_constant(theme,'Border')+item+item//2))
    context.wait_for_line('USER:# toolkit: list 1',30)
    normal = capture('normal','interacted')
    start = len(context.console.lines)
    click(x+fw-32,y+14)
    context.wait_for_line('USER:# toolkit: event 9 ',30,since=start)
    context.wait_for_line('USER:# toolkit: event 8 ',90,since=start)
    maximized = capture('maximized','interacted')
    context.check('toolkit.maximized',(maximized[1:5]==[0,0,width,height]) and bool(maximized[-1]&2))
    start = len(context.console.lines)
    click(width-32,14)
    context.wait_for_line('USER:# toolkit: event 9 ',30,since=start)
    context.wait_for_line('USER:# toolkit: event 8 ',90,since=start)
    restored = capture('restored','interacted')
    context.check('toolkit.restored',restored[1:5]==normal[1:5] and not restored[-1]&2)
    # Opaque/outline move policy stays in the compositor; content is unchanged.
    dx = 12 if x+fw+12 <= width else -12
    dy = 6 if y+fh+6 <= height else -6
    move(x+100,y+14); button(True); move(x+100+dx,y+14+dy); button(False)
    moved = capture('moved','interacted')
    context.check('toolkit.moved',moved[1:3]==[x+dx,y+dy] and moved[3:5]==normal[3:5])
    click(moved[1]+moved[3]-14,moved[2]+14)
    context.wait_for_line('USER:# toolkit: event 11 ',30)
    context.wait_for_line('USER:# toolkit: closed',90)
    context.check('toolkit.close.form-path',True)
    context.check('toolkit.healthy',not [line for line in context.console.lines if '# watchdog:' in line or '# startup:' in line])
    context.artifact('Input.json').write_text(json.dumps(events,indent=2))
