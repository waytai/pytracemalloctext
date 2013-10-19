import gc
import sys
import time
import tracemalloc

ALLOC_LOOPS = 3
NOBJECTS = 10**4
BENCH_RUNS = 5

# using DisplayTop, only 2 GroupedStats instances are present in memory at
# the same time
NGET_STATS = 2
NGET_TRACES = 2

def get_location(lineno_delta):
    frame = sys._getframe(1)
    code = frame.f_code
    lineno = frame.f_lineno + lineno_delta
    return (code.co_filename, lineno)

def alloc_objects():
    for loop in range(ALLOC_LOOPS):
        objs = [object() for index in range(NOBJECTS)]
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs = None

def get_stats():
    return tracemalloc.get_stats()
get_stats_location = get_location(-1)

def get_traces():
    x = tracemalloc.get_traces()
    return x
get_traces_location = get_location(-1)

def test_get_stats():
    all_stats = []
    for loop in range(NGET_STATS):
        objs = [object() for index in range(NOBJECTS)]
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        stats = get_stats()
        objs = None
        all_stats.append(stats)
        stats = None
    all_stats = None

def test_get_traces():
    all_traces = []
    for loop in range(NGET_TRACES):
        objs = [object() for index in range(NOBJECTS)]
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        objs.extend([object() for index in range(NOBJECTS)])
        traces = get_traces()
        objs = None
        all_traces.append(traces)
        traces = None
    all_traces = None

def bench(func, trace=True):
    if trace:
        tracemalloc.clear_traces()
        tracemalloc.enable()
    gc.collect()
    best = None
    for run in range(BENCH_RUNS):
        start = time.monotonic()
        func()
        dt = time.monotonic() - start
        if best is not None:
            best = min(best, dt)
        else:
            best = dt
    if trace:
        tracemalloc.disable()
    gc.collect()
    return best * 1e3

def bench_tracing():
    func = alloc_objects

    base = bench(func, False)
    print("no tracing", base)

    tracemalloc.set_traceback_limit(0)
    dt = bench(func)
    print("trace, 0 frames: %.1f, %.1f times slower" % (dt, dt / base))
    tracemalloc.set_traceback_limit(1)

    dt = bench(func)
    print("trace: %.1f, %.1f times slower" % (dt, dt / base))

    for n in (1, 10, 100):
        tracemalloc.enable()
        tasks = [tracemalloc.Task(str) for index in range(n)] # dummy callback
        for task in tasks:
            task.set_delay(60.0)
            task.schedule()
        dt = bench(func)
        print("trace with %s task: %.1f, %.1f times slower" % (n, dt, dt / base))
        tracemalloc.cancel_tasks()

    tracemalloc.add_include_filter(__file__)
    dt = bench(func)
    print("trace with filter including file: %.1f, %.1f times slower" % (dt, dt / base))
    tracemalloc.clear_filters()

    tracemalloc.add_exclude_filter(__file__ + "xxx")
    dt = bench(func)
    print("trace with not matching excluding file: %.1f, %.1f times slower" % (dt, dt / base))
    tracemalloc.clear_filters()

    tracemalloc.add_exclude_filter(__file__)
    dt = bench(func)
    print("trace with filter excluding file: %.1f, %.1f times slower" % (dt, dt / base))
    tracemalloc.clear_filters()

    for nframe in (5, 10, 25, 100):
        tracemalloc.set_traceback_limit(nframe)
        dt = bench(func)
        print("trace, %s frames: %.1f, %.1f times slower" % (nframe, dt, dt / base))
        tracemalloc.set_traceback_limit(1)

def main():
    bench_tracing()
    print("")
    dt = bench(test_get_stats)
    print("get stats: %.1f" % dt)
    dt = bench(test_get_traces)
    print("get traces: %.1f" % dt)

main()
