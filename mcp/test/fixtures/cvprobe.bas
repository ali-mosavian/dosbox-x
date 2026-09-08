defint a-z
option explicit

declare function pr_add ( byval a as integer, byval b as integer ) as integer
declare sub pr_fill ( n as integer, v() as integer )
declare sub pr_show ( s as string )

dim shared pr_count as integer
dim pr_tab(15) as integer
dim pr_i as integer
dim pr_sum as integer

pr_count = 16
pr_fill pr_count, pr_tab()
pr_sum = 0
for pr_i = 0 to pr_count - 1
    pr_sum = pr_add( pr_sum, pr_tab( pr_i ) )
next pr_i
pr_show "sum=" + str$( pr_sum )
end

function pr_add ( byval a as integer, byval b as integer ) as integer
    dim t as integer
    t = a + b
    pr_add = t
end function

sub pr_fill ( n as integer, v() as integer ) static
    dim k as integer
    for k = 0 to n - 1
        v( k ) = k * 2
    next k
end sub

sub pr_show ( s as string ) static
    print s
end sub
