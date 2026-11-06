--          = pico racer =
--          from kometbomb

carcol = {{1,13,12},{1,3,11},{8,14,15}}
frame = 0
md=1
mdx=0
mdy=17
level={
	{horzx=1,horzy=0,
		skyx=1,skyy=3,
		landx=0,landy=8,
		road=0, rubble=13,
		rbank=4,rwater=12, 
		tree=86, trunk=89},
	{horzx=17,horzy=0,
		skyx=17,skyy=3,
		landx=1, landy=8,
		road=3, rubble=14,
		rbank=12,rwater=1,
		tree=13, trunk=106},
	{horzx=33,horzy=0,
		skyx=17,skyy=3,
		landx=2, landy=8,
		road=0, rubble=15,
		rbank=4,rwater=12,
		tree=13, trunk=106}
}

fadepal = {
	{1,1,1,1,0,0,0,0,0},
	{2,2,2,1,1,1,0,0,0},
	{3,3,3,5,5,1,1,0,0},
	{4,4,2,2,2,1,0,0,0},
	{5,5,5,1,1,1,0,0,0},
	{6,6,6,5,5,1,1,0,0},
	{7,7,6,6,5,2,1,0,0},
	{8,8,8,4,4,2,1,0,0},
	{9,9,4,2,2,1,0,0,0},
	{10,10,10,14,14,8,2,0,0},
	{11,11,3,3,3,1,1,0,0},
	{12,12,13,2,2,1,0,0,0},
	{13,13,13,2,2,1,1,0,0},
	{14,14,8,8,2,1,1,0,0},
	{15,15,14,8,8,2,1,1,0}
}

groundpal = {
	nil,
	nil,
	nil,
	{9,9,9,4,4,2,2,1,0},
	nil,
	nil,
	nil,
	nil,
	{9, 9, 4, 4, 2,1,1,0,0},
	nil,
	nil,
	nil,
	nil,
	nil,
	{15,9, 4, 2,1,0,0,0,0}
}

horzpal = {
	nil,
	nil,
	nil,
	{ 4, 2, 2, 1, 1, 0, 0, 0,0},
	nil,
	nil,
	{7,7,15,15,15,6,6,6,6},
	nil,
	{ 9, 4, 4, 2, 2, 1, 0, 0,0},
	{10, 9, 9, 4, 4, 2, 1, 0,0},
}

sprpal = {
	nil,
	nil,
	nil,
	{ 4, 2, 2, 1, 1, 0, 0, 0,0},
	nil,
	nil,
	{7,7,6,6,5,2,1,0,0},
	nil,
	{ 9, 4, 4, 2, 2, 1, 0, 0,0},
	{10, 9, 9, 4, 4, 2, 1, 0,0},
}

function curve()
cls()
local x=10
local y=64

local a=0
for s in all(trackq) do
	for l=0,s.len,32 do
		line(x,y,x,y)
		x += cos(a)
		y -= sin(a)
		a += s.curve	
		end
end

stop()

end

function addscore(sc)
	score += sc
	while score >= 10000 do
		score -= 10000
		score2 += 1
	end
end

function updatebestscore()
	if score2 > bestscore2 or 
		(score2 == bestscore2 and
			score >= bestscore) then
			bestscore = score
			bestscore2 = score2
			
			dset(0, bestscore)
			dset(1, bestscore2)
	end
end

function setfade(x,gr)
 if x < 1 then
 	pal()
 	return
 end
 for p=1,15 do
 	if gr == 1 and groundpal[p] != nil then
 		pal(p,groundpal[p][flr(x)])
 	elseif gr == 2 and horzpal[p] != nil then
 		pal(p,horzpal[p][flr(x)])
 	elseif gr == 3 and sprpal[p] != nil then
 		pal(p,sprpal[p][flr(x)])
 	else
 		pal(p,fadepal[p][flr(x)])
		end 	
 end
end

function sqr(x)
	return x * x
end

function getroadz(y)
	return 2048/(y+1)
end

function drawcar(ox,y,z,turn,col,isplr)
 local x = ox + px
 local xb = x / 12 - turn / 2
 local s = flr(abs(xb))
 local f = false
 
 if xb < 0 then
  f = true
 end
 
 
 if s < 0 then
  s = 0
 end
 
 if s > 2 then
  s = 2
 end
 
 local zscl1 = 1 * 32 / (z + 32)
 local zscl2 = 1 * 32 / (z + 1 + 32)
 local zscl3 = 1 * 32 / (z + 2 + 32)
 local dy = 32 * 32 / (z + 32)
 local ody = flr(dy)
 if ody < 2 then
  ody = 2 
 elseif ody > 63 then
  ody = 63
 end
 local xoff = xoffs[ody]
 
 local sw = 24 * 24 / (z + 32)
 local sh = 8 * 24 / (z + 32)
 local sl = sh
 
 if sl < 4 then
	 sl = 4
 end


 if isplr and fade > 6 then
 	setfade(6,3)
 else
 setfade(fade,3)
 end 

 if col == 0 then
 	--mypal()
 else
 	local p = carcol[col]
 	if flr(fade) > 0 then
 	pal(4,fadepal[p[1]][flr(fade)])
 	pal(9,fadepal[p[2]][flr(fade)])
 	pal(10,fadepal[p[3]][flr(fade)])
 	else
 	pal(4,p[1])
 	pal(9,p[2])
 	pal(10,p[3])
 	end
 end
 
 local t = 0
 if isplr then
 	t = jump * 2
 end
 
 if sw < 24 then
 sspr(24, 32 + s*8, 24, 8, xoff+x*zscl3-sw/2+64+turn*zscl3, (y)*zscl3+64-sh-t, sw, sh, f, false)
	--sspr(24, 32 + s*8, 24, 8, xoff+x*zscl3-12+64+turn*zscl3, (y)*zscl3+64-8-t/2, 24*zscl3, 8*zscl3, f, false)
 sspr(0, 32 + s*8, 24, 8, xoff+x*zscl1-sw/2+64, (y)*zscl1+64-sh, sw, sh, f, false)
 pal()
 if fade > 7 then
 sspr(32, 32 + 3*8, 8, 8, xoff+x*zscl1+sh/3+64, (y)*zscl1+64-sh, sl, sl, f, false)
 sspr(32, 32 + 3*8, 8, 8, xoff+x*zscl1-sh+64, (y)*zscl1+64-sh, sl, sl, f, false)
 end
 
 else
	spr(64 + s*16 + 3, xoff+x*zscl3-12+64+turn*zscl3, (y)*zscl3+64-8-t, 3, 1, f, false)
	spr(70, xoff+x*zscl2-4+64+turn*zscl2*0.5, (y-2)*zscl2+64-8-t/2, 1, 1, f, false)
	spr(64 + s*16, xoff+x*zscl1-12+64, y*zscl1+64-8, 3, 1, f, false)
	pal()
 if fade > 7 then

	spr(116, xoff+x*zscl1+2+64, y*zscl1+64-8, 1, 1, f, false)
	spr(116, xoff+x*zscl1-8+64, y*zscl1+64-8, 1, 1, f, false)
	end
	
 end
 --pal()
end

function drawseg(y,curve,xm)
	local z=getroadz(y)
	local mip=0
	if z > 320 then
		mip=2
	elseif z > 160 then
		mip=1+y%2
	elseif z > 112 then
		mip=1
	elseif z > 64 then
		mip=0+y%2
	end
	
	if mip == 0 then
		mip = level[clev].road
	end
	
	
	local x=xm*64/(z+1)+curve/2
	local rw=80*64/(z+1)
	local w=8*64/(z+1)
	
	sspr(0+mip*16+8,(z/2+py)%8,8,1,64+x-rw/2-1,y+64,rw/2+2,1)
	sspr(0+mip*16+8,(z/2+py)%8,8,1,64+x,y+64,rw/2+1,1,true)
	
	sspr(0+mip*16,(z/2+py)%8,8,1,64+x-rw/2-w,y+64,w,1)
	sspr(0+mip*16,(z/2+py)%8,8,1,64+x+rw/2,y+64,w,1)
end

function addseg(len, curve,trigger)
	local seg = {curve=curve, len=len, trigger=trigger}
	add(track,seg)
end

function _init()
	cartdata("kb_picoracer")
	gamemode = 3
	bestscore = dget(0)
	bestscore2 = dget(1)
	initgame()
end

function initgame()
	md=1
	mdx=0
	mdy=17
	maptravel=0
	disttrav=0
	gotime = 0
	
	resetpos = false
	starttimer = 3 * 30
	clev = 1
	seg = 1
	score = 0
	score2 = 0
 pt = 0
 px = 0
	py = 0
	carsscr = 0
	horzmove = 0
 tzbase = 0
 tpos = 1
 horzx = 0
 track = {}

	rveryshort=64
	rshort=128
 rlong=256
 rverylong=512
 tight=0.02
 loose=0.01
 verytight=0.03
 
 -- egypt leg 1
 addseg(rshort,0)
 addseg(rshort,loose,{flags=3,flagdir=0})
 addseg(rshort,0)
 addseg(rlong,-loose,{flags=4,flagdir=1,cars=1})
 addseg(rshort,-tight,{cars=1})
 addseg(rshort,-loose,{cars=1})
 addseg(rlong,0,{flags=4,flagdir=-1})
 addseg(rshort,loose,{cars=3})
 addseg(rshort,0)
 addseg(rshort,-loose)
 addseg(rshort,0,{cars=3})
 addseg(rlong,tight)
 addseg(rshort,0,{ball=-32})
 addseg(rlong,-tight,{cars=5})
 addseg(rshort,0)
 addseg(rshort,0)
 addseg(rlong,-loose,{goal=1})
 
 -- egypt leg 2
 addseg(rlong,-tight,{cars=3})
 addseg(rshort,-tight)
 addseg(rshort,-loose,{cars=3})
 addseg(rshort,0,{flags=4,flagdir=0})
 addseg(rshort,tight)
 addseg(rshort,verytight,{flags=1,flagdir=1})
 addseg(rshort,tight)
 addseg(rshort,0,{flags=1,flagdir=-1})
 addseg(rshort,-tight)
 addseg(rlong,-verytight,{cars=4})
 addseg(rshort,0)
 addseg(rshort,0,{jump={-32,0,32},river=1})
	addseg(rshort,loose)
	addseg(rshort,0,{jump={-32,0,32},river=1})
	addseg(rshort,0,{ball=32})
	addseg(rshort,0,{jump={-32},river=1})
 addseg(rshort,0,{flags=4,flagdir=1})
	addseg(rshort,0,{jump={32},river=1,cars=10})
	addseg(rshort,0,{goal=1})
	
	-- egypt leg 3 night
	addseg(rlong,loose,{cars=10,fade=1})
 addseg(rlong,0,{cars=4})
 addseg(rshort,-loose,{cars=2})
 addseg(rlong,loose,{cars=4})
 addseg(rshort,0,{cars=5})
 addseg(rshort,-loose,{cars=10})
 addseg(rlong,-tight,{cars=10})
 addseg(rlong,0,{cars=10,lev=2})
 addseg(rshort,0,{cars=2})
 addseg(rlong,tight)
 addseg(rlong,loose,{cars=10})
	addseg(rshort,0,{fade=-1})
	addseg(rshort,0,{goal=1})
	
	-- north pole leg 1

 addseg(rlong,tight)
 addseg(rshort,0)
	addseg(rshort+32,0,{jump={-32},river=1})
	addseg(rshort+32,0,{jump={-32},river=1})
 addseg(rshort+32,0,{jump={32},river=1})
	addseg(rshort+32,0,{jump={32},river=1})
 addseg(rshort+32,0,{flags=3,flagdir=0})
	addseg(rshort,0,{jump={0},river=1})
 addseg(rshort+64,0,{jump={32},river=1})
 addseg(rshort,0,{jump={-32},river=1,cars=5})
 addseg(rshort,verytight)
 addseg(rshort,0,{cars=5})
 addseg(rshort,-verytight)
 addseg(rshort,0,{ball=-32})
 addseg(rlong,-tight,{goal=1})
 
 -- north pole leg 2
  
 addseg(rlong,-tight,{cars=3})
 addseg(rshort,-tight)
 addseg(rshort,-loose,{cars=2})
 addseg(rshort,0,{flags=2,flagdir=0})
 addseg(rlong,tight)
 addseg(rshort,0,{flags=2,flagdir=-1})
 addseg(rshort,-verytight)
 addseg(rlong,-tight,{cars=4})
 addseg(rshort,0,{flags=2,flagdir=1})
 addseg(rshort+16,0,{jump={0,0,32},river=1})
	addseg(rshort,tight,{ball=0})
	addseg(rshort,0,{jump={-32,0,32},river=1})
	addseg(rshort,0)
	addseg(rshort,0,{jump={32},river=1})
 addseg(rshort,0,{flags=2,flagdir=1})
	addseg(rshort,0,{jump={-32},river=1,cars=10,fade=1})
	addseg(rshort,0,{goal=1})

	
		-- north pole leg 3 night
	addseg(rlong,loose,{cars=10, carspeed=1.75})
 addseg(rlong,0,{cars=4})
 addseg(rshort,tight,{cars=2})
 addseg(rlong,loose,{cars=4})
 addseg(rshort,0,{cars=5})
 addseg(rshort,-loose,{cars=10})
 addseg(rlong,-tight,{cars=10})
 addseg(rlong,0,{cars=10,lev=3})
 addseg(rshort,0,{cars=2})
 addseg(rshort,tight)
 addseg(rlong,verytight,{cars=10})
	addseg(rshort,0,{fade=-1})
	addseg(rshort,0,{goal=1})
	
	--	mountains leg 1
	
 addseg(rlong,-loose,{cars=3})
 addseg(rshort,-tight,{flags=3,flagdir=0})
 addseg(rshort,-loose,{cars=3})
 addseg(rshort,0,{flags=4,flagdir=0})
 addseg(rlong,tight)
 addseg(rshort,verytight,{flags=1,flagdir=1})
 addseg(rlong,tight)
 addseg(rshort,0,{flags=1,flagdir=-1})
 addseg(rlong,-tight)
 addseg(rlong,-verytight,{cars=4})
 addseg(rshort,0)
 addseg(rshort,0,{jump={-32,0,32},river=1})
	addseg(rlong,loose)
	addseg(rshort,-loose,{jump={-32,0,32},river=1})
	addseg(rshort,0,{ball=-32})
	addseg(rshort,0,{jump={-32},river=1})
 addseg(rshort,0,{flags=4,flagdir=1})
	addseg(rshort,0,{jump={32},river=1,cars=10})
	addseg(rshort,0,{goal=1})

	-- mountains leg 2
	addseg(rlong,loose,{cars=10})
 addseg(rlong,0,{cars=4})
 addseg(rshort,-loose,{cars=2})
 addseg(rlong,loose,{cars=4})
 addseg(rshort,0,{cars=5})
 addseg(rshort,-loose,{cars=10})
 addseg(rlong,-tight,{cars=10})
 addseg(rlong,0,{cars=10})
 addseg(rshort,0,{cars=2})
 addseg(rlong,tight)
 addseg(rlong,loose,{cars=10, carspeed=2})
	addseg(rshort,0,{fade=1})
	addseg(rshort,0,{goal=1, ball=0})
	
	--night
	
	addseg(rlong,-loose,{cars=10})
 addseg(rlong,0,{cars=4})
 addseg(rlong,loose,{cars=2})
 addseg(rlong,-loose,{cars=4})
 addseg(rshort,0,{cars=5})
 addseg(rshort,-loose,{cars=10})
 addseg(rlong,tight,{cars=10})
 addseg(rlong,0,{cars=10,lev=3})
 addseg(rshort,0,{cars=2})
 addseg(rshort,-tight)
 addseg(rshort,verytight,{cars=10})
	addseg(rlong,tight,{fade=-1})
	addseg(rshort,0,{river=1,jump={-32}})
	addseg(rlong,0,{finish=1})
	-- finished
	addseg(rlong,0)
	
	
 trackq = track
 goals = 0
 cheatlev = 0
 if cheatlev > 0 then
	for t=1,#trackq do 
		if trackq[t].trigger != nil and
  	trackq[t].trigger.goal != nil then
			goals += 1
			
			if goals == cheatlev then
				tpos = t - 3
				break
			end
 	end
 end
 end
 
 plrht = 0
 jump = 0
 
	--curve()
 
 carspeed = 1.4
 speed = 0
 fade = 0
 clock = 45*30+29
 fadedir = 0
 
 passedcars = 0
 
 smokeq = {}
 drawq = {}
 objq = {}
 
 if gamemode == 4 then
	 sfx(2,1)
 end
 
 trigseg(trackq[1],32,trackq[1].len+32)
end

function drawobj(t,x,y,z,turn,col,isplr)
 add(drawq, {t=t, x=x, y=y, z=z,turn=turn,col=col,isplr=isplr})
end

function addobj(t,x,y,z,c)
	add(objq, {t=t,x=x,y=y,z=z,col=c,pass=false})
end

function addsmoke(x,y,z)
	add(smokeq, {x=x,y=y,z=z,f=0})
end

function trigseg(seg,z1,z2)

	if seg.curve < -tight then
		addobj(10, 48, 19, tzbase + track[tpos].len-32)
		addobj(8, 48, 32, tzbase + track[tpos].len-32)
	elseif seg.curve > tight then
		addobj(9, -48, 19, tzbase + track[tpos].len-32)
		addobj(8, -48, 32, tzbase + track[tpos].len-32)
	end

	local tr = seg.trigger
	if tr == nil then
		return
	end
	
	if tr.carspeed != nil then
		carspeed = tr.carspeed
	end
	
	
	if tr.fade != nil then
		fadedir = tr.fade
	end
	
	if tr.lev != nil then
		clev = tr.lev
	end
	
	if tr.ball != nil then
		addobj(11, tr.ball, 32, z1)
	end
	
	if tr.river != nil then
		addobj(6, 0, 32, z1+20)
	end

	if tr.jump != nil then
		for j in all(tr.jump) do
			addobj(12, j, 32, z1)
		end
	end
	
	if tr.goal != nil then
		addobj(16, 0, 32, z1)
	end
	
	if tr.finish != nil then
		--stop()
		addobj(17, 0, 32, z1)
	end
	
	if tr.flags != nil then
		local x=-tr.flagdir*32
		for i=1,tr.flags do
			addobj(4, x, 32, z1+i*48)
			x += 16*tr.flagdir
		end
	end
	
	if tr.cars != nil then
		local c = 0
		for z=z1,z2,32 do
			if carsscr < 10 then
				carsscr += 1
				addobj(3, rnd() * 64 - 32, 32, z, flr(rnd(2) + 0.5) + 1)
			end
			c += 1
			if c >= tr.cars then
				break
			end
		end
	end
end


function updateobjs()
--	print(#objq)
--	stop()
	for o in all(objq) do
	 if o.t != 3 then
 		o.z -= speed
 		
 		if o.t == 11 and o.passed then
 			o.z += 10
 			o.y -= 2
 		end
 	else
 		o.z -= speed - carspeed
 	end
 	
 	if o.t == 6 and plrht <= 0 and o.z <= -12 then
 		sfx(0,0)
 		jump = 2
 		
 		speed /= 4
 	elseif o.t == 16 and not o.passed and o.z <= -12 then
			clock += 32*30
			seg += 1 	
			o.passed = true
			disttrav = seg * 4
			sfx(6,2)
		elseif o.t == 17 and not o.passed and o.z <= -12 then
			--stop()
			gamemode = 2
			deinitgame()
			o.passed = true
 		-- goal!!!
 		-- goal!!!
 	elseif plrht < 10 and o.z <= -12 and o.z >= -15
 		and abs(-o.x - px*2) < 12
 	then
 		if o.t != 6 and o.t != 16 then
 		if o.t == 3 then
 			if -px < o.x then 
 				pt = -speed
	 		else
 				pt = speed
 			end
 			speed *= 3/4
 			sfx(3,0)
 		elseif o.t == 4 then
 			addscore(100)
 			sfx(1,0)
 			del(objq, o)
 		elseif o.t == 11 and not o.passed then
 			addscore(1000)
				o.passed = true
				sfx(0,0)
			elseif o.t == 12 and not o.passed then
				jump = 3
				sfx(0,0)
			elseif not resetpos then
 		 if -px < o.x then 
 				pt = -speed
	 		else
 				pt = speed
 			end
 			speed /= 2
			end
			end
 	end
 	
 	if o.z < -18 then
 		if o.t == 3 and not o.passed then
	 		passedcars += 1
	 		sfx(5,0)
	 		o.passed = true
	 	end
 	end
-- 	stop()
 	if o.z <= -32 or o.z > 1024 then
 		if o.t == 3 then
 			carsscr -= 1
 		end
 	 del(objq, o)
 	end
 end
 
	for o in all(smokeq) do
 	o.z -= speed
 	o.y -= 0.25
 	o.f += 0.334
-- 	stop()
 	if o.z <= -32 or o.f >= 4 then
 	 del(smokeq, o)
 	end
 end

end

function deinitgame()
	sfx(-1,0)
	sfx(-1,1)
	sfx(-1,2)
	frame = 0
end

function updategame()
 drawq = {}
 
 if gotime < 30 then
 gotime += 1
 end
 
 
	local accl = 0
	local turn = 0

	frame +=1
	
	local tach = (speed * 20) % 25 + speed * 3
	
	if tach == 0 then
		tach += frame % 3
	end
	
	if tach > 31 then
		tach = 31
	end

	poke(0x3200+68*2,tach)
	poke(0x3200+68*2+1,3+0x60)
	poke(0x3200+68*2+2,tach/1.5)
	poke(0x3200+68*2+3,1+0x60)

	if btn(0) then
		turn = -1
	elseif btn(1) then
	 turn = 1
	end
	
	if btn(3) or btn(4) then
		accl = -1
		--fade = fade - 0.25
	elseif btn(2) or btn(5) then
		accl = 1
	 --fade = fade + 0.25
	end
	
	if clock <= 0 then
		accl = 0
	end
	
	if gamemode != 0 then
		accl = 0
		turn = 0
	end
	
	if gamemode == 4 then
		starttimer -= 1
		
		if starttimer <= 0 then
			gamemode = 0
			sfx(8,0)
		elseif starttimer % 30 == 0 then
			sfx(7,0)
		end
	end
	
	if not resetpos and plrht <= 0 then
	 if accl > 0 then
		 if speed > 2.5 then
			 speed += 0.003	
 		elseif speed > 2 then
	 		speed += 0.005	
		 elseif speed > 1.25 then
			 speed += 0.007	
 		else
		 	speed += 0.03
	 	end
	 elseif accl < 0 then
		 speed -= 0.05
	 else
		 speed -= 0.03
	 end
	end
	
	if not resetpos and plrht <= 0 and abs(px) >= 20 then
		if speed > 0.5 then
		speed -= 0.25
		end
		
		if speed > 1 then
		jump = 0.5
		end
		
		if speed < 0.1 and abs(px) >= 32 then
			resetpos = true
		end
	end 
	
	if speed < 0 then
	 speed = 0
	end
	
	if speed > 3.19 then
	 speed = 3.19
	end
	
	if not resetpos and speed <= 0 and plrht > 0 then
		speed = 0.5
	end
	
	xpush = trackq[tpos].curve * 32 * speed

	if plrht <= 0 then
		px -= pt
	end
	
	px += xpush
	
	if not resetpos then
	 if (accl < 0 and speed > 2) 
		 or (accl > 0 and speed < 0.75) or
		 jump > 0 or abs(-pt - xpush) >= 3.5 then
		 if fade < 6 then
		  addsmoke(-px*2+4, 32-plrht, -14 + speed/2)
		  addsmoke(-px*2-4, 32-plrht, -14 + speed/2)
		
		  --addsmoke(-px*2+4+pt, 32-plrht, -10)
		  --addsmoke(-px*2-4+pt, 32-plrht, -10)
		 end
		 
		 if frame % 3 == 0 and plrht <= 0 then 
		 	sfx(10,3)
		 end
	 end
	end
	
	if turn < 0 then
		pt -= 0.6
	elseif turn > 0 then
		pt += 0.6
	else
		if pt > 0 then
			pt -= 0.4
		elseif pt < 0 then
		 pt += 0.4
	end
	
	if pt > -0.4 and pt < 0.4 then
	 pt = 0
	end
	end
	
	if pt < -2 then
	 pt = -2
	end
	
	if pt > 2 then
	 pt = 2
	end
	
	if resetpos then
		pt = 0
		local s = 1
		
		if abs(px) > 20 then
			s = 2
		end
	
		if px > 0 then
			px -= s
		elseif px < 0 then
			px += s
		end
		
		if px >= -3 and px <= 3 then
			resetpos = false
		end
	
	end
	
	horzmove = (horzmove * 2 + trackq[tpos].curve * 60 * speed) / 3
 horzx -= horzmove
	
	plrht += jump
	
	if plrht < 0 and jump != 0 then
		jump /= -3
		plrht = 0
		sfx(9,0)
		if abs(jump) < 0.5 then
			jump = 0
		end
	elseif plrht > 0 then
		jump -= 0.2
	end
	
	py = py + 1 * speed
	tzbase += 1 * speed
	
	if trackq[tpos].len <= tzbase then
		tzbase = trackq[tpos].len - tzbase
		tpos += 1
		if tpos > #trackq then
			tpos = 1
		end
		
		local ntpos = tpos + 1
		
		if ntpos > #trackq then
			ntpos = 1
		end
		
		trigseg(trackq[ntpos],
			tzbase + trackq[tpos].len,
			tzbase + trackq[tpos].len + trackq[ntpos].len)
		
		
		--spawn objects for segment
		
		if fade < 2 then
		--addobj(4, rnd() * 64 - 32, 32, tzbase + track[tpos].len)
		end
		
		for c=0,80,32 do
			--addobj(2, c + 80, 32, tzbase + track[tpos].len)
			--addobj(2, -c - 80, 32, tzbase + track[tpos].len)

		end
		
		for c=0,track[tpos].len,32 do
			addobj(level[clev].rubble, rnd() * -40 - 64, 32, tzbase + c + track[tpos].len)
			addobj(level[clev].rubble, rnd() * 40 + 64, 32, tzbase + c + track[tpos].len)
		end
	
	
	 --[[
	 if not trackq[tpos].tunnel and
	 	track[ntpos].tunnel then
	 	addobj(6, 0, 0, tzbase + track[tpos].len)
	 end
	 
	 if trackq[tpos].tunnel and
	 	not track[ntpos].tunnel then
	 	addobj(7, 0, 0, tzbase + track[tpos].len)
	 end
	 --]]
		
		if fade < 3 then
		for c =0,trackq[ntpos].len,140 do
		addobj(1, -64, 19, tzbase + track[tpos].len+8+c)
		addobj(1, 64, 19, tzbase + track[tpos].len+8+c)
		addobj(2, -64, 32, tzbase + track[tpos].len+8+c)
		addobj(2, 64, 32, tzbase + track[tpos].len+8+c)
		end
		end
		
		--addobj(10, -48, 19, tzbase + track[tpos].len)
		--addobj(8, -48, 32, tzbase + track[tpos].len)

		--addobj(11, 0, 32, tzbase + track[tpos].len)


		--addobj(12, rnd()*40-40, 32, tzbase + track[tpos].len)

		
	end
	
	
	--if rnd() < 0.1 then
	-- addsmoke(-px*2, 32, -14)
	--end
	
	--print(#objq)
	--stop()
	updateobjs()

 for o in all(objq) do
	 drawobj(o.t,o.x,o.y,o.z,0,o.col)
 end
 
 for o in all(smokeq) do
	 drawobj(5,o.x,o.y,o.z,0,o.f)
 end

	--drawobj(0,16,32,10)
	drawobj(0,-px*2,32-plrht,-14,pt*2,0,true)
 --drawcar(px*2, 50,40,trackq[tpos].curve*100)
	--drawcar(-px / 2,50,1,pt*2)	
	
	if gamemode == 0 then
	
	addscore(speed/2)

	if clock > 0 then
		clock -= 1
	end
	
	if clock <= 0 and speed <= 0 then
		gamemode = 1
		deinitgame()
	end
	end
	
	if fadedir < 0 then
		fade -= 0.075
		
		if fade < 0 then
			fade = 0
		end
	elseif fadedir > 0 then
		fade += 0.075
		
		if fade > 9 then
			fade = 9
		end
	end
end

function drawtree(x,y,z,bspr,w,h,f)

	setfade(fade,3)

 local dy = 32*32 / (z + 32)
 local ody = flr(dy)
 if ody < 2 then
  ody = 2
 elseif ody > 63 then
  ody = 63
 end
	local sw = w * 8 * 32 / (z + 32)
	local sh = h * 8 * 32 / (z + 32)
	dy = y*32 / (z + 32)
	if sw < w * 8 then 
		local srcx = (bspr % 16) * 8
 	local srcy = flr(bspr/16) * 8
  
 	sspr(srcx,srcy,w*8, h*8, (x+px)*32/(z+32)+64-sw/2+xoffs[ody],dy+64-sh,sw,sh, f, false)
 else
 spr(bspr,(x+px)*32/(z+32)+64+xoffs[ody]-w*4,dy+64-h*8, w, h, f, false)
 end
end

function drawsmoke(x,y,z)
 local dy = 32*32 / (z + 32)
 local ody = flr(dy)
 if ody < 2 then
  ody = 2
 elseif ody > 63 then
  ody = 63
 end
	local s = 8 * 32 / (z + 32)
	dy = y*32 / (z + 32)
	if s < 8 then 
  sspr(0,56+32,8,8,(x+px)*32/(z+32)+64-s/2+xoffs[ody],dy+64-s,s,s)
 else
  spr(112,(x+px)*32/(z+32)+64+xoffs[ody]-4,dy+64-16, 1, 1)
 end
end


function drawwall(z,c)


	dy1 = 32 * 32 / (z + 32 + 20)
	dy2 = (32 + 8) * 32 / (z + 32 + 20)
	dy3 = 32 * 32 / (z + 32)
	
	if z <= -32 then
		return
	end

	color(level[clev].rbank)

	rectfill(0, dy1+64, 127, dy2+64)
	color(level[clev].rwater)

	rectfill(0, dy2+64, 127, dy3+64)

	color()
end


function drawgoal(z,c)

	dy1 = -6 * 32 / (z + 32)
	dy2 = (-1) * 32 / (z + 32)
	dy3 = 32 * 32 / (z + 32)
	
	local dy = 32*32 / (z + 32)
 local ody = flr(dy)
 if ody < 2 then
  ody = 2
 elseif ody > 63 then
  ody = 63
 end

	local x1 = (-54 + px) * 32 / (z + 32) + xoffs[ody]
	local x2 = (54 + px) * 32 / (z + 32) + xoffs[ody]  

	
	local w = 8*32/(z+32)

	sspr(96,16,8,16,x1+64-w/2,dy2+64,w,dy3-dy2)
	sspr(96,16,8,16,x2+64-w/2,dy2+64,w,dy3-dy2)


	color(7)

	rectfill(x1 + 64, dy1+64, x2 + 64, dy2+64)

	--rectfill(0, dy2+64, 127, dy3+64)

	color()
end

function drawstart()
	drawgame()
	local z = 1 - (starttimer % 30 / 30)
	if starttimer < 3 * 30 then
	sspr(flr((starttimer)/30+1)*8,64+8,8,16,64-8*z,64-16*z,16*z,32*z)
	gotime =0
	end
end


function _draw()
	if gamemode == 0 then 
		drawgame()
	elseif gamemode == 4 then
		drawstart()
	elseif gamemode == 5 then
		drawgameovermap()
	elseif gamemode == 1 then
		drawgameover()
	elseif gamemode == 2 then
		drawgamecompleted()
	elseif gamemode == 3 then
		drawgametitle()
	end
	
end

function updategametitle()
	frame += 1
	
	if btnp(2) or btnp(4) or btnp(5) then
		gamemode = 4
		initgame()
	end
end

function initmap()
	perc=0
	for y=16,16+6 do
		for x=0,15 do
			if mget(x,y) >= 224 then
				mset(x,y,mget(x,y)-16)
			end 
		end
	end
	
	local i = tpos
	while i > 1 do
		if trackq[i].trigger != nil and trackq[i].trigger.goal != nil then
			break
		end
		i-=1
	end
	local j = i+1
	while j < #trackq do
		if trackq[j].trigger != nil and trackq[j].trigger.goal != nil
		then
			break
		end
		j+=1
	end
	
	local len = j-i
	disttrav=(seg-1)*4*6+flr((tpos-i+1)*4*6/len)
	--[[color(7)
	cls()
	print(tpos-i+1)
	print(len)
	print(seg)
	print(disttrav)
	stop()--]]
	
	if disttrav == nil or disttrav < 0 then
		disttrav = 0
	end
	--cls()
	--print(i)
	--print(j)
	--print(len)
	--stop()
end

function updategameover()
	frame += 1
	
	if frame > 3 * 30 then
		gamemode = 5
		frame = 0
		initmap()
	end
end

function updategameovermap()
	if frame == 0 then
		initmap()
		perc=0
	end

	frame += 1
	
	if disttrav > 0 then
		perc = min(99,min(frame,disttrav)*100/(3*3*4*6))
	else
		perc = 0
	end
	
	if frame % 6 == 0 and md >= 0 then
	
		if mget(mdx,mdy) >= 211 then
			mset(mdx,mdy,mget(mdx,mdy)+16)
		end
		
		if md == 0 then
			mdx+=1
		elseif md == 1 then
			mdy+=1
		elseif md == 2 then
			mdx-=1
		elseif md == 3 then
			mdy-=1
		end
		
		local t = mget(mdx,mdy)
	
		if t == 213 then
			if md == 3 then
				md = (md + 1) % 4
			else
				md = (md + 3) % 4 
			end
		elseif t == 214 then
			if md == 3 then
				md = (md + 3) % 4
			else
				md = (md + 1) % 4 
			end
		elseif t == 215 then
			if md == 2 then
				md = (md + 1) % 4
			else
				md = (md + 3) % 4 
			end
		elseif t == 216 then
			if md == 0 then
				md = (md + 3) % 4
			else
				md = (md + 1) % 4 
			end
		elseif t < 211 then
			md = -1
		end
		
		maptravel += 6
		
		if maptravel >= disttrav then
			md = -1
		end

	end
	
	if btnp(2) or btnp(4) or btnp(5) then
		gamemode = 3
		updatebestscore()
		deinitgame()
	end
end

function updategamecompleted()
	updategame()

	if passedcars > 0 then
		addscore(20)
		passedcars -= 1
	elseif clock >= 0.1*30 then
		addscore(10)
		clock -= 0.1*30
	end
	if clock < 0.1*30 and passedcars <= 0 and btnp(2) then
		gamemode = 3
		updatebestscore()
		deinitgame()
	end
end

function _update()
	if gamemode == 0 or gamemode == 4 then
		updategame()
	elseif gamemode == 1 then
		updategameover()
	elseif gamemode == 2 then
		updategamecompleted()
	elseif gamemode == 3 then
		updategametitle()
	elseif gamemode == 5 then
		updategameovermap()
	end
	
end



function drawgame()
 cls()
 
 local	ntpos = tpos + 1
		
		if ntpos > #trackq then
			ntpos = 1
		end

	setfade(fade,0)
 
 for x=0,15 do
 	map(0,0,x*8,0,1,8)
 end
 
 curlev = level[clev]
 
 setfade(fade,1)
 
 for x=0,15 do
 	map(curlev.landx,curlev.landy,x*8,64,1,8)
 end
		
 map(curlev.skyx,curlev.skyy,(horzx / 2) % 128,64-32,16,4)
 map(curlev.skyx,curlev.skyy,(horzx / 2) % 128 - 128,64-32,16,4)
 map(curlev.skyx,curlev.skyy,(horzx / 2) % 128 + 128,64-32,16,4)

	setfade(fade,1)

 map(curlev.horzx,curlev.horzy,horzx % 128,64-24,16,3)
 map(curlev.horzx,curlev.horzy,horzx % 128 - 128,64-24,16,3)
 map(curlev.horzx,curlev.horzy,horzx % 128 + 128,64-24,16,3)
 
 setfade(fade,2)

 local x = 0
 local xs = 0.0
 local xs2 = 0
 local tp = tpos
 local lz = getroadz(63) - tzbase
 xoffs={}
	for y=63,2,-1 do
	 local rz = getroadz(y)
	 if rz - lz > trackq[tp].len then
	  tp += 1
	  lz = rz
	  if tp > #trackq then
	  	tp = 1
	  end
	  
	 end
	 xs2 = trackq[tp].curve
	 xs = xs + xs2 
	 x = x + xs * rz / 32  
	 xoffs[y]=x/2
	 drawseg(y,x,px,false)
	end
	
	sort(drawq)
	
	for o in all(drawq) do
		if o.z > -34 and o.z < 320 then
	 if o.t == 0 then 
	 	drawcar(o.x, o.y, o.z, o.turn, 0, true)
	 elseif o.t == 1 then
	 	drawtree(o.x,o.y,o.z,level[clev].tree,3,3)
 	elseif o.t == 2 then
	 	drawtree(o.x,o.y,o.z,level[clev].trunk,1,2)
	 elseif o.t == 3 then
	 	drawcar(o.x,o.y,o.z,0, o.col)
	 elseif o.t == 4 then
	 	drawtree(o.x,o.y,o.z,74,2,2)
	 elseif o.t == 5 then
	 	drawtree(o.x,o.y,o.z,112 + flr(o.col),1,1)	
	 elseif o.t == 6 then
	 	drawwall(o.z, 7)	
 	elseif o.t == 7 then
	 	drawwall(o.z, 0)	
		elseif o.t == 8 then
	 	drawtree(o.x,o.y,o.z,44,1,2)	
	 elseif o.t == 9 then
	 	drawtree(o.x,o.y,o.z,36,2,2)	
	 elseif o.t == 10 then
	 	drawtree(o.x,o.y,o.z,36,2,2,true)	
	 elseif o.t == 11 then
	 	drawtree(o.x,o.y,o.z,78,2,2)	
	 elseif o.t == 12 then
	 	drawtree(o.x,o.y,o.z,125,3,1)	
	 elseif o.t == 13 then
	 	drawtree(o.x,o.y,o.z,71,1,1)	
	 elseif o.t == 14 then
	 	drawtree(o.x,o.y,o.z,72,1,1)	
	 elseif o.t == 15 then
	 	drawtree(o.x,o.y,o.z,121,1,1)	
 	elseif o.t == 16 or o.t == 17 then
	 	drawgoal(o.z,1)	
	 end
	 end
	end
	
	pal()
	
	spr(62,0,128-8,2,1)
	prnum(passedcars,3,13,128-7,false)

	spr(143,128-4*8+2,128-8+1)
	prnum(flr((seg-1)/3+1),1,128-3*8+1,128-7,false)
	spr(142,128-2*8+1,128-8+1)
	prnum(flr((seg-1)%3+1),1,128-1*8,128-7,false)
	
	prnum(score,4,8*2,8,false)
	prnum(score2,2,0,8,false)
	prnum(clock/30,2,64-8,8,true)
	
	prnum(speed*100,3,128-8*5+3,8,false)
	
	spr(138,64-13,0,4,1)
	spr(154,0,0,4,1)
	spr(170,127-8*4+2,0,4,1)
	spr(158,127-2*8+4,8,2,1)
	
	if gotime < 15 and gamemode == 0 then
		local z = gotime/15 + 0.5
		sspr(0,96+16,24,16,64-z*12,64-z*8,z*24,z*16)
	end
	
	

	--drawlogo()

 --line(64,0,64,127)
 --pal(7,0,1)
 --pal()
	
	--cursor(0,0)
	--color(7)
	--print(stat(1)*100)
	--print(clock/30)
	--print(passedcars)
	--print(tpos.."/"..#trackq)
end



	
function sort(a)
  for i=1,#a do
    local j = i
    while j > 1 and 
   		(a[j-1].t != 6 and a[j].t == 6 or a[j-1].z < a[j].z) do
      a[j],a[j-1] = a[j-1],a[j]
      j = j - 1
    end
  end
end


function drawlogo()
	for x=64-6*8,64+5*8,8 do
		local f = flr(sin(x/60+frame/30)*1.5+1.5)
		if f < 0 then
			f = 0
		elseif f > 2 then
			f = 2
		end
		spr(208+f,x,64-20-4+sin(x/80+frame/30)*2)
		spr(208+f,x,64-20-4+8+sin(x/80+frame/30)*2)
	end
	
	nicetext(64-5*8,64-20,"pico racer")

end


function drawgametitle()
	drawgame()
	
	drawlogo()
	
	cursor(64-5*4,64-13+14)
	color(1)
	print("best score:")
	cursor(64-5*4-1,64-13-1+14)
	color(7)
	print("best score:")

	prnum(bestscore,4,64-3*8+8*2,64+8,false)
	prnum(bestscore2,2,64-3*8,64+8,false)

	cursor(64-4*9.5,64+25)
	color(1)
	print("press fire to start")

	cursor(64-4*9.5-1,64+25-1)
	color(8)
	print("press fire to start")
	
	spr(73,42,98,1,1)
	cursor(52,100)
	color(frame/4%8)
	print("kometbomb")
	
end

function drawgameover()
	drawgame()

	nicetext(64-5*8+4, 64-8, "game over")

	
	--rectfill(4,40,124,40+8*7+4)
	
	--map(0,16,0,44,16,7)

end

function drawgameovermap()
	drawgame()
	nicetext(64-6*6-4, 64-36, "course map")

	cursor(64-11*4,64+45)
	
	color(1)
	

	print("press fire to continue")
	color(7)
	cursor(64-11*4-1,64+45-1)

	print("press fire to continue")
	
	color(0)
	--rectfill(4,40,124,40+8*7+4)
	
	map(0,16,4,44,16,7)
	
	prnum(perc,3,6*8,44+8*2,true)
end


function drawgamecompleted()
	drawgame()
	
	nicetext(64-15*4,64-15, "congratulations")
	cursor(64-6*4,64-13+14)
	color(1)
	print("final score:")

	cursor(64-6*4-1,64-13-1+14)

	color(7)
	print("final score:")

	prnum(score,4,64-3*8+8*2,64+8,false)
	prnum(score2,2,64-3*8,64+8,false)

	cursor(64-11*4,64+25)
	color(1)
	print("press fire to continue")
	color(7)
	cursor(64-11*4-1,64+25-1)

	print("press fire to continue")

end


function prnum(num,digits,x,y,big)
	local tmp = num
	local h = 1
	local base = 128
	if big then
		h = 2
		base = 144
	end
	for i=digits-1,0,-1 do
		local digit = flr(tmp) % 10
		spr(base+digit,0+i*8+x,y,1,h)
		tmp /= 10
	end
end

chars=" !\"#$%&'()*+,-./0123456789:;<=>?@abcdefghijklmnopqrstuvwxyz[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
-- '
s2c={}
c2s={}
for i=1,95 do
c=i+31
s=sub(chars,i,i)
c2s[c]=s
s2c[s]=c
end

function ord(s,i)
return s2c[sub(s,i or 1,i or 1)]
end

function nicetext(x,y,text)
	local b = ord("a")
	for i=1,#text do
		local c = ord(sub(text,i,i))-b+176
		spr(c,x,y,1,1)
		x+=8
	end
end
