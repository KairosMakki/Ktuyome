#!/usr/bin/env python3
"""Headless QEMU test helper: boots the ISO, sends keys/mouse, saves screenshots."""
import socket, subprocess, time, sys, os
from PIL import Image
class Q:
    def __init__(s, disk="/tmp/test.img"):
        if os.path.exists("/tmp/mon"): os.remove("/tmp/mon")
        if not os.path.exists(disk): subprocess.run(["qemu-img","create","-f","raw",disk,"64M"],capture_output=True)
        s.p = subprocess.Popen(["qemu-system-i386","-m","256","-cdrom","ktuyome.iso","-hda",disk,"-boot","d","-vga","std",
            "-display","none","-monitor","unix:/tmp/mon,server,nowait","-serial","null"],stderr=subprocess.PIPE)
        for _ in range(50):
            if os.path.exists("/tmp/mon"): break
            time.sleep(0.1)
        s.sock = socket.socket(socket.AF_UNIX); s.sock.connect("/tmp/mon"); s.sock.settimeout(2)
        try: s.sock.recv(4096)
        except: pass
    def cmd(s, c):
        s.sock.sendall((c+"\n").encode()); time.sleep(0.15)
        try: s.sock.recv(65536)
        except: pass
    def key(s, k, wait=0.35): s.cmd("sendkey "+k); time.sleep(wait)
    def type(s, text):
        m={" ":"spc",".":"dot","-":"minus","\n":"ret","/":"slash","<":"shift-comma",">":"shift-dot","'":"apostrophe","=":"equal"}
        for ch in text:
            k=m.get(ch, ("shift-"+ch.lower()) if ch.isupper() else ch); s.key(k,0.12)
    def move(s,dx,dy): s.cmd(f"mouse_move {dx} {dy}"); time.sleep(0.2)
    def click(s): s.cmd("mouse_button 1"); time.sleep(0.1); s.cmd("mouse_button 0"); time.sleep(0.3)
    def shot(s, name):
        s.cmd("screendump /tmp/s.ppm"); time.sleep(0.3)
        Image.open("/tmp/s.ppm").save(f"/tmp/{name}.png"); print("saved", name)
    def close(s): s.p.kill()
