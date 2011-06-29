a = 'a'
proc = Proc.new{ p (a = 'do_finalize'); a.free }
ObjectSpace.define_finalizer(a, aProc=proc)
a.free
p (b = "doo")
b.free
proc.free
