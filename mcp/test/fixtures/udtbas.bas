defint a-z

type Vtx
    x as integer
    y as integer
    tag as long
end type

dim shared home as Vtx
dim shared nodes(3) as Vtx
dim i as integer
dim total as long

home.x = 3
home.y = 4
home.tag = 100

for i = 0 to 3
    nodes(i).x = i + 1
    nodes(i).y = (i + 1) * 2
    nodes(i).tag = (i + 1) * 10&
next i

total = 0
for i = 0 to 3
    total = total + nodes(i).x
next i

print "home="; home.x; home.y; "sum="; total
end
