# SPDX-License-Identifier: Apache-2.0
"""Secret-free identity and authentication screen oracle, using only public UI state."""
import json
import re
from AsteriaVm import Display


def render(root,width,height,state,edit=0,account='',masks=0,password_focus=False,jis=False,retry_seconds=1):
    strings={name:json.loads(value) for name,value in re.findall(r'ASTERIA_LOGIN_STRING\((\w+),\s*("(?:[^"\\]|\\.)*")\)',
        (root/'Data/Strings/Login.en-US.inc').read_text())}
    font=Display.load_font(root)
    glyphs={int(point,16):int(code,16) for code,point in re.findall(r'\{(0x[0-9A-Fa-f]+)U,\s*(0x[0-9A-Fa-f]+)U\}',
        (root/'Source/User/Terminal/Cp437.hpp').read_text())}
    background=bytes((16,32,64));foreground=bytes((255,255,255))
    pixels=bytearray(background*(width*height))
    left=max(0,(width-640)//2);top=max(0,(height-400)//2)
    def text(x,y,value):
        for index,character in enumerate(value):
            for row,mask in enumerate(font[glyphs.get(ord(character),0xfe)]):
                for column in range(8):
                    px,py=left+x+index*8+column,top+y+row
                    if 0<=px<width and 0<=py<height:
                        offset=(py*width+px)*3
                        pixels[offset:offset+3]=foreground if mask&(128>>column) else background
    text(24,24,strings['Title']);text(24,360,strings['Fixture'])
    simple={0:'Attention',2:'Starting',5:'LoggingOut',6:'Recovery'}
    if state in simple:text(24,136,strings[simple[state]])
    elif state in (1,4):
        if state==4:text(24,72,strings['Locked'])
        text(24,296,strings['KeyboardJis' if jis else 'KeyboardUs'])
        text(24,120,strings['User']);text(128,120,account)
        text(24,168,strings['Password']);text(128,168,'*'*min(masks,55))
        text(24,232,strings['Verifying' if edit==1 else 'Retry' if edit==2 else 'Unlock' if state==4 else 'SignIn'])
        if edit==2:text(24,256,strings['RetryTime']+str(retry_seconds))
        if edit==0:
            count=min(masks if password_focus else len(account),55)
            for row in range(16):
                for column in range(8):
                    offset=((top+(168 if password_focus else 120)+row)*width+left+128+8*count+column)*3
                    pixels[offset:offset+3]=foreground
    return bytes(pixels)
