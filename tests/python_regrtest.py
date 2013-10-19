"""
Add a task to take snapshots and display the top of memory allocations.
"""

def test_task(fast=False, traces=True):
    import gc
    import time
    import traceback
    import tracemalloctext

    def add_metrics(snapshot):
        size, max_size = tracemalloc.get_traced_memory()
        snapshot.add_metric('tracemalloc.traced.size', size, 'size')
        snapshot.add_metric('tracemalloc.traced.max_size', max_size, 'size')

        if snapshot.traces:
            snapshot.add_metric('tracemalloc.traces', len(snapshot.traces), 'int')

        size, free = tracemalloc.get_tracemalloc_memory()
        snapshot.add_metric('tracemalloc.module.size', size, 'size')
        snapshot.add_metric('tracemalloc.module.free', free, 'size')
        if size:
            frag = free / size
            snapshot.add_metric('tracemalloc.module.fragmentation', frag, 'percent')

        snapshot.add_metric('gc.objects', len(gc.get_objects()), 'int')
        snapshot.add_metric('pymalloc.blocks', sys.getallocatedblocks(), 'int')

    if traces:
        tracemalloc.set_traceback_limit(25)

    top = tracemalloctext.DisplayTop()
    take = tracemalloctext.TakeSnapshotTask(traces=traces, callback=add_metrics)

    stream = sys.__stderr__
    def taskfunc(take, top, stream, monotonic, count):
        try:
            start = monotonic()
            snapshot, filename = take.take_snapshot()
            dt = monotonic() - start
            stream.write("%s: Write a snapshot of memory allocations into %s (%.1f sec)\n"
                             % (snapshot.timestamp, filename, dt))
            stream.flush()

            top.display_snapshot(snapshot, count, file=stream)
        except Exception as err:
            stream.write("Failed to take a snapshot: %s\n" % err)
            text = traceback.format_exc().rstrip()
            for line in text.splitlines():
                stream.write("> %s" % line)
            stream.flush()

    take.filename_template = "/tmp/tracemalloc-$pid-$counter.pickle"
    if fast:
        delay = 10
        top_count = 5
        threshold = 5 * 1024 * 1024
    else:
        delay = 120
        top_count = 5
        threshold = 5 * 1024 * 1024

    task = tracemalloctext.Task(taskfunc, take, top, stream, time.monotonic, top_count)
    task.call()

    task.set_memory_threshold(threshold)
    task.set_delay(delay)
    task.schedule()

