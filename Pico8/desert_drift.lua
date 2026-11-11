-- desert drift
-- by @johanpeitz

-- based on 
-- https://codeincomplete.com/posts/javascript-racer/



t=0

uf=nil
df=nil

best=1

function _init()
 cartdata("jpddrift")
 
 -- reset best
 best=dget(0)
 if (best==0) then
  reset_best()
 end
 
 
 game_init()
end

function reset_best()
 dset(0,30*60*5) 
 best=dget(0)
end

function _update()
 uf()
end

function _draw()
 df()
end


-->8
-- game

position=0
dz=0
px=0
py=0
pz=0
pdx=0
offroad=0
camd=16
camh=6
drawdistance=34
segments={}
seglen=64
roadwidth=8
timer=0
ftime=0
ft=0
tdz=0
centi=0

sunoffset=0
cityoffset=0
sunspeed=0.5
cityspeed=4

bbs={
 {x=0,y=0,w=16,h=32,s=1,c=0}, -- cactus
 {x=16,y=16,w=16,h=16,s=1,c=0}, -- bush
 {x=32,y=0,w=32,h=32,s=2,c=1}, -- cliff
 {x=16,y=0,w=16,h=16,s=1,c=1}, -- rock
 {x=64,y=0,w=32,h=24,s=2,c=1}, -- billboard
 {x=64,y=24,w=8,h=8,s=2,c=0}, -- stick
 {x=48,y=0,w=16,h=32,s=2,c=1}, -- cliff2
 {x=96,y=0,w=16,h=32,s=3,c=1}, -- power pole
 {x=112,y=0,w=16,h=32,s=1,c=0}, -- flag
 {x=80,y=-32,w=48,h=32,s=3,c=2} -- tunnel
} 

function game_init()
 uf=game_update
 df=game_draw
 
 ftime=0
 t=-60 
 ft=0
 centi=0
 position=0
 dz=0
 px=0
 py=0
 pz=0
 pdx=0
 tdz=0
 offroad=0
 camd=16
 camh=6
 drawdistance=34
 segments={}
 seglen=64
 roadwidth=8
 timer=0

 sunoffset=0
 cityoffset=0
 sunspeed=0.5
 cityspeed=4
  
 reset_road()
 pz=camh*camd
end

function reset_road()
 local short=false
 
 -- start stretch
 add_straight(32,0)
  
 if (not short) then

 add_curve(20,0.2)
 add_straight(20,0.2)
 add_curve(20,-0.2,0.1)
 add_straight(20)

 add_curve(20,0.3,0.2)
 add_curve(20,-0.4,-0.1)
 add_straight(10)
 add_curve(60,0.5,-0.2)
 add_curve(40,-0.3,-0.2)
 add_straight(10)
 add_curve(20,0.4,0.1)
 add_curve(20,-0.1,0)
 add_curve(20,0.2,0)
 
 add_straight(40,0.2)
 add_curve(30,0.5,-0.1)
 add_straight(20,-0.1)
 add_curve(30,-0.3,-0.1)
 
 end
 
 -- last stretch
 add_straight(drawdistance+5)
 
 -- start line
 segments[3].c = 7 
 for i=3,8 do
  add_bb(9,i,450)
  add_bb(9,i,-450)
 end
 
 -- finish line
 local fl=#segments-(drawdistance+2)
 segments[fl].c = 12
 for i=fl-5,fl+5 do
  add_bb(9,i,450)
  add_bb(9,i,-450)
 end
 
 if (not short) then
 
 -- debris
 for i=3,#segments do
  -- white sticks
  if (i%8==0) then
   add_bb(6,i,470)
   add_bb(6,i,-470)
  end
 
  id=2
  if (rnd()>0.8) id=1
  if (rnd()>0.8) id=4
  add_bb(id,i,500+rnd(2000))
  id=2
  if (rnd()>0.8) id=1
  if (rnd()>0.8) id=4
  add_bb(id,i,-600-rnd(2000))
 end
  
 -- billboards
 for i=42,82,5 do
  add_bb(5,i,600)
 end

 -- power lines
 local d=-5000
 for i=30,90,5 do
  add_bb(8,i,d)
  d+=300
  if (d>-600) d=-600
 end
 d=600
 for i=90,120,5 do
  add_bb(8,i,d)
  d+=500
 end
  
 -- cliffs
 local d=5000
 for i=120,180 do
  add_bb(3,i,d+rnd(100))
  add_bb(7,i,d+600)
  if (d>600) d-=400
 end
 d=5000
 for i=150,260 do
  add_bb(3,i,-d-rnd(100))
  add_bb(7,i,-d-600)
  if (d>600) d-=400
 end  
 local d=5000
 for i=240,260 do
  add_bb(3,i,d+rnd(100))
  add_bb(7,i,d+600)
  if (d>600) d-=400
 end
  
 -- tunnel
 for i=260,290 do
  if (i%1==0) then
   add_bb(10,i,1)
   add_bb(10,i,-1)
  end
 end 

 -- cliffs
 d=-700
 for i=290,400 do
  add_bb(3,i,d-rnd(100))
  add_bb(7,i,d-600)
  d-=100
 end  
 -- billboards
 for i=300,330,5 do
  add_bb(5,i,600)
 end


 -- power lines
 local d=-3000
 for i=300,390,5 do
  add_bb(8,i,d)
  d+=300
  if (d>-600) d=-600
 end
 d=600
 for i=390,450,5 do
  add_bb(8,i,d)
  d+=200
 end
 
  -- billboards
 for i=440,470,5 do
  add_bb(5,i,600)
 end 
 
 end
 
 tracklen=#segments*seglen
end

function add_seg(curve,y)
 local cols={6,13}
 local n=#segments
 local s={}
 s.id=n
 s.p1={x=0,y=lasty(),z=n*seglen,cam={},scrn={}}
 s.p2={x=0,y=y,z=(n+1)*seglen,cam={},scrn={}}
 s.c=cols[1+(n%2)]
 s.curve=curve
 s.bbs={}
 add(segments,s)
end

function lasty()
 if (#segments==0) return 0
 return segments[#segments].p2.y;
end

function add_bb(id,n,pos)
 add(segments[n].bbs,{id=id,pos=pos})
end

function add_road(enter,hold,leave,curve,y)
 if (y==nil) y=0
 local starty=lasty()
 local endy=starty+y*seglen
 local total=enter+hold+leave
 for n=0,enter do
  add_seg(easein(0,curve,n/enter),easeinout(starty,endy,n/total))
 end
 for n=0,hold do
  add_seg(curve,easeinout(starty,endy,(enter+n)/total))
 end
 for n=0,leave do
  add_seg(easeinout(curve,0,n/leave),easeinout(starty,endy,(enter+hold+n)/total))
 end 
end

function add_curve(n,c,y)
 add_road(flr(n/3),flr(n/3),flr(n/3),c,y)
end

function add_straight(n,y)
 add_road(flr(n/3),flr(n/3),flr(n/3),0,y)
end

function find_seg(z)
 return segments[1+flr(z/seglen)%#segments]
end

function game_update()
 if (t==0) sfx(2)
 if (ftime>0) then
  ft+=1
  if (btnp()) then
   sfx(3)
   confetti={}
   game_init()
  end
  
  local p=add_particle(rnd(132)-4,-8,rnd(0.6)-0.3,1+rnd(1),confetti)
  p.t=132
  local r=flr(rnd(100)+1)
  if (r>0)  p.c={3,11}
  if (r>20) p.c={9,10}
  if (r>40) p.c={13,12}
  if (r>60) p.c={6,7}
  if (r>80) p.c={8,14}
  for p in all(confetti) do
   update_particle(p)
  end

  return
 end
 
 t+=1
 if (t>120) timer+=1
 local ps=find_seg(position+pz)
 local sp=dz/30

 local tp=1
 if (sp<0.2) tp=0.9
 if (sp<0.1) tp=0
 if (sp>0.5) tp=0.8
 if (sp>0.7) tp=0.7
 tp=1
 if (t>120) then
  if (btn()) pdx-=0.02*tp
  if (btn()) pdx+=0.02*tp
 end
 px+=pdx
 centi=0.4*sp*ps.curve
 px-=centi
 pdx*=0.85
 
 dz*=1-min(0.1,abs(pdx))*0.1
 
 if (t>120) then
  if (btn()) dz+=0.65
  if (btn()) dz*=0.97
 else
  if (btn()) then
   tdz+=1.5
   local p=add_particle(55,117,-rnd(),rnd(),particles)
   p.c={6,7}
   p=add_particle(72,117,rnd(),rnd(),particles)
   p.c={6,7}
   sfx(0,-1,flr(max(0,tdz*0.7)),1)
  else
   sfx(0,-1,flr(max(1,tdz*0.7)),1)
  end
 end
 tdz*=0.95
 position+=dz
 dz*=0.98
 offroad=0
 if (abs(px)>0.85) then
  offroad=1
  dz*=0.96
  sfx(6,-1,flr(rnd()*32),1)
  
  for bb in all(ps.bbs) do
   local bbd=bbs[bb.id]
   if (bbd.c==1) then
    local bbw=bbd.w*40
    local bbx=bb.pos
    if (bb.pos<0) bbx-=bbw
    if (overlap(px*520,bbx,bbw,1)) then
     if (dz>0) dz=-10
     pdx=-sgn(bb.pos)*0.2*max(0.5,sp)
     sfx(1)
    end
   elseif (bbd.c==2) then
    dz=0
    pdx=-sgn(px)*0.2
    sfx(1)
   end
  end 
 end
 
 if (ps.c==12 and ftime==0) then
  ftime=timer
  music(0)
  if (ftime<best) then
    best=ftime
    dset(0,best)
  end
 end

 if (t>120) then
  sfx(0,
      -1,
      flr(max(0,dz*0.7)),
      1)
 end
 
 for p in all(particles) do
  update_particle(p)
 end

 if (offroad==0 and dz>10 and abs(centi)>0.04 and t%2==0) then
  local p=add_particle(54,116,-rnd(),rnd(),particles)
  p.c={6,7}
  p=add_particle(73,116,rnd(),rnd(),particles)
  p.c={6,7}
  sfx(5,-1,0,4)
 end

 if (dz>2 and t%2==1) then
  if (px<-0.80 or px>1.15) then
   add_particle(54,116,-rnd(),rnd(),particles)
  end
  if (px>0.80 or px<-1.15) then
   add_particle(73,116,rnd(),rnd(),particles)
  end
 end
 
 sunoffset+=sunspeed*ps.curve*sp
 cityoffset+=cityspeed*ps.curve*sp
end

function interp(a,b,p)
 return a+(b-a)*p
end

function overlap(x1,x2,w2)
 if (x1<x2) return false
 if (x1>x2+w2) return false
 return true
end

function game_draw()
 -- sky
 cls(12)
 
 -- sun
 fillp(0b1010010110100101.1)
 circfill(30-sunoffset,20,16,6)
 fillp()
 circfill(30-sunoffset,20,14,15)
 fillp(0b1010010110100101.1)
 circfill(30-sunoffset,20,13,10)
 fillp()
 circfill(30-sunoffset,20,12,10)

 -- city
 if (position>17000) then
  spr(192,128+48-cityoffset,35+py,16,4)
 end
 
 -- segments
 local maxy=128
 -- base segment
 bs = find_seg(position)
 bp = (position%seglen)/seglen
 ps = find_seg(position+pz)
 pp = ((position+pz)%seglen)/seglen
 py = interp(ps.p1.y,ps.p2.y,pp)
 
 local x=0
 local dx=-bs.curve*bp

 for n=1,drawdistance do
  local s=segments[(bs.id+n)%#segments]
  project(s.p1,px*roadwidth-x,py+camh,position,camd,roadwidth)
  project(s.p2,px*roadwidth-x-dx,py+camh,position,camd,roadwidth)
  s.clip=maxy
 
  x+=dx
  dx+=s.curve
  skip=false
  if ((s.p1.cam.z<=camd) or
      (s.p2.scrn.y>=maxy)) 
      then
       skip=true
  end
  
  if (not skip) then
   draw_segment(s)
   maxy=s.p2.scrn.y
  end
 end
 
 -- draw billboards
 for n=drawdistance,1,-1 do
  local s=segments[(bs.id+n)%#segments]
  for bb in all(s.bbs) do
   local x=s.p1.scrn.x+bb.pos*s.p1.scrn.scale
   local y=interp(s.p1.scrn.y,s.p2.scrn.y,0.5)
   local bbd=bbs[bb.id]
   clip(0,0,128,s.clip+1)
   local f=false
   if (bb.pos<0) then
    f=true
    x-=10*bbd.w*s.p1.scrn.scale*bbd.s
   end
   
   skip=false
   if ((s.p1.cam.z<=camd)) 
       then
        skip=true
   end
   
   if (skip==false) then
    local fo=0
    if (f) fo=1
    sspr(bbd.x,64+bbd.y,bbd.w,bbd.h,
         x,
         3+y-10*bbd.h*s.p1.scrn.scale*bbd.s,
         1.5*fo+10*bbd.w*s.p1.scrn.scale*bbd.s,
         10*bbd.h*s.p1.scrn.scale*bbd.s,
         f
         )
   end
  end   
 end
 clip()

 -- car
 local id=32
 local to=0
 if (pdx>0.03) then 
  id=4 to=1
 end
 if (pdx<-0.03) then
  id=0 to=-1
 end 
 spr(36,48+to,110,4,1)
 spr(52,52+to,109)
 spr(52,68+to,109,1,1,true)
 local co=offroad*(t/max(dz,5))%2
 if (dz<1) co=0
 spr(id,48,
  100+co,
  4,2)
  
 -- particles
 for p in all(particles) do
  draw_particle(p,1)
 end
 for p in all(particles) do
  draw_particle(p,2)
 end
          
 for p in all(confetti) do
  draw_confetti(p)
 end

 draw_speedo(111,111,0)        
 draw_speedo(110,110,7) 
 
 printo(int2time(timer),95,2,7)
 spr(112,110,18,4,1)
 print(int2time(best),95,12,
  (ftime>0 and 7 or 6))
 
 draw_counter()
 draw_finish()

end

function int2time(i)
 local s=flr(i/30)
 local m=flr(s/60)
 local h=flr(99*(i-s*30)/30)
 s-=m*60
 if (s<10) s="0"..s
 if (m<10) m="0"..m
 if (h<10) h="0"..h
 return m..":"..s.."."..h
end

function printo(str,x,y,c)
 for q=-1,1 do
  for w=-1,1 do
   print(str,x+q,y+w,0)
  end
 end
 print(str,x,y,c)
end

function draw_finish() 
 local y=easeout(-20,0,min(ft,30)/30)
 rectfill(0,y-1,127,y+7,0)
 printo("finished!",1,y+1,7)
 printo(int2time(ftime),95,y+1,7)
 if (y==0 and flr(ft/4)%4<3) then
  print("(x) to retry",1,12,13)
 end
end

function draw_counter()
 local y=easeout(-10,50,min(30+t,60)/60)
 if (t>130) y=easeout(50,-20,max((t-130),0)/30)
 if (t<160) then
  local x=22
  pal(7,0)
  spr(64,x,y-10,8,1)
  spr(80,x+48,y-10,8,1)
  pal()
  spr(64,x,y-10-1,8,1)
  spr(80,x+48,y-10-1,8,1)
  
  printo("made by @johanpeitz",2,y-48,7) 
  
  rectfill(0,y-1,127,y+8,0)
  spr(96+(t>30 and 1 or 0),45,y)
  spr(96+(t>60 and 1 or 0),55,y)
  spr(96+(t>90 and 2 or 0),65,y)
  spr(96+(t>120 and 3 or 0),75,y)
 end

 printo("(z)  gas",2,109,7)
 printo("(x)  break",2,115,7)
 printo(" steer",2,121,7)
end
  
function project(p,cx,cy,cz,cdp,rw)
 p.cam.x=p.x-cx
 p.cam.y=p.y-cy
 p.cam.z=p.z-cz
 p.scrn.scale=cdp/p.cam.z
 p.scrn.x=flr(64+(p.scrn.scale*p.cam.x*64))
 p.scrn.y=flr(64-(p.scrn.scale*p.cam.y*64))
 p.scrn.w=flr(   (p.scrn.scale*rw*64))
end

function draw_segment(s)
 -- grass
-- local bgcols={2,4}
-- local bgcols={11,3}
 local bgcols={9,15}
 local sidecols={7,6,7,2,1,2}

 rectfill(0,
          s.p1.scrn.y,
          127,
          s.p2.scrn.y+1,
          bgcols[1+s.id%2])
 
 -- road
 polygon(s.p1.scrn.x-s.p1.scrn.w,
         s.p1.scrn.y,
         s.p1.scrn.w*2,
         s.p2.scrn.x-s.p2.scrn.w,
         s.p2.scrn.y+1,
         s.p2.scrn.w*2,
         s.c)
         
 -- sides and lanes
 local w1=s.p1.scrn.w/20
 local w2=s.p2.scrn.w/20
 if (s.id%2==0) then
  polygon(s.p1.scrn.x+w1,
          s.p1.scrn.y,
          w2,
          s.p2.scrn.x+w2,
          s.p2.scrn.y+1,
          w2,
          7)
 end
 polygon(s.p1.scrn.x-s.p1.scrn.w,
         s.p1.scrn.y,
         -w1,
         s.p2.scrn.x-s.p2.scrn.w,
         s.p2.scrn.y+1,
         -w2,
         sidecols[1+s.id%2])
 polygon(s.p1.scrn.x+s.p1.scrn.w,
         s.p1.scrn.y,
         -w1,
         s.p2.scrn.x+s.p2.scrn.w,
         s.p2.scrn.y+1,
         -w2,
         sidecols[1+s.id%2]) 
end

function polygon(x1,y1,w1,x2,y2,w2,c)
 local tx=x2
 local dtx=(x2-x1)/abs(y1-y2)
 local tw=w2
 local dtw=(w2-w1)/abs(y1-y2)
 
 if (false) then
  line(x1,y1,x1+w1,y1,c)
  line(x1+w1,y1,x2+w2,y2,c)
  line(x2+w2,y2,x2,y2,c)
  line(x2,y2,x1,y1,c)
 else
  for y=y2,y1 do
   line(tx,y,tx+tw,y,c)
   tx-=dtx
   tw-=dtw
  end
 end
end

function draw_speedo(x,y,c)
 circ(x,y,15,c)
 circfill(x,y,2,c)
 for a=-0.1,0.75,0.1 do
  pset(x+12*cos(a+0.06),
       y+12*sin(a+0.06),
       c)
 end
 local a=0.75+1-0.75*max(0,dz)/30
 local nc=c
 if (nc==7) nc=12
 line(x,y,x+11*cos(a),y+11*sin(a),nc)
end
-->8
-- easing

function easein(a,b,p)
 return a+(b-a)*(p^2)
end

function easeout(a,b,p)
 return a+(b-a)*(1-(1-p)^2)
end

function easeinout(a,b,p)
 return a+(b-a)*(-cos(p/2)/2+0.5)
end
-->8

-->8
-- particles
particles={}
confetti={}
function add_particle(x,y,dx,dy,tbl)
 local p={}
 p.c={4,9}
 p.t=15
 p.x=x
 p.y=y
 p.dx=dx
 p.dy=dy
 p.ix=1
 p.iy=1
 p.f=116
 p.tf=4
 p.fc=0
 p.a=tbl
 add(tbl,p)
 return p
end

function update_particle(p)
 p.x+=p.dx
 p.y+=p.dy
 p.dx*=p.ix
 p.dy*=p.iy
 p.t-=1
 if (p.t%2==0) p.fc+=1
 if (p.fc==p.tf) p.fc=0
 
 if (p.t<0) del(p.a,p)
end

function draw_particle(p,l)
 local r=5*(1-(p.t/15))-l+1
 circfill(p.x,p.y-l,r,p.c[l])
end

function draw_confetti(p)
 pal(8,p.c[1])
 pal(14,p.c[2])
 
 spr(p.f+p.fc,p.x,p.y)
 pal()
end