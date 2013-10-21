# From http://www.evanjones.ca/memoryallocator/
# Improving Python's Memory Allocator
# Evan Jones

import gc, time

if 0:
    import tracemalloc
    import tracemalloctext

    tracemalloc.enable()
    if 1:
        task = tracemalloctext.DisplayTopTask(10)
        task.display()

        def dump_memory(what):
            print("***** %s *****" % what)
            task.display()
    else:
        def dump_memory(what):
            pass

elif 1:
    import tracemalloc
    import tracemalloctext

    def task():
        size, max_size = tracemalloc.get_traced_memory()
        print("Traced memory: %.1f MB"
              % (size / 1024.0 ** 2))

    tracemalloctext.start_task(None, 10 * 1024 * 1024, task)

    def dump_memory(what):
        print("***** %s *****" % what)

else:
    def dump_memory(what):
        print("***** %s *****" % what)
        with open("/proc/self/status") as fp:
            for line in fp:
                if "VmRSS" not in line:
                    continue
                print(line.rstrip())
                break

iterations = 2000000

start = time.time()
l = []
for i in range( iterations ):
        l.append( None )

dump_memory("None (1)")
print()

for i in range( iterations ):
        l[i] = {}

dump_memory("empty dict (1)")
print()

for i in range( iterations ):
        l[i] = None

dump_memory("None (2)")
print()

for i in range( iterations ):
        l[i] = {}


dump_memory("empty dict (2)")
print()

l = None
gc.collect()
dt = time.time() - start

dump_memory("free memory")
print()

print("Total time: %.1f sec" % dt)

