import contextlib
import datetime
import imp
import io
import os
import sys
import time
import tracemalloc
import unittest
from test.script_helper import assert_python_ok

PYTHON3 = (sys.version_info >= (3,))
if PYTHON3:
    from unittest.mock import patch
    from test import support
    from io import StringIO
    INT_TYPES = int
else:
    from test import test_support as support
    try:
        from cStringIO import StringIO
    except ImportError:
        from StringIO import StringIO
    INT_TYPES = (int, long)

EMPTY_STRING_SIZE = sys.getsizeof(b'')

def noop(*args, **kw):
    pass

def get_frames(nframe, lineno_delta):
    frames = []
    frame = sys._getframe(1)
    for index in range(nframe):
        code = frame.f_code
        lineno = frame.f_lineno + lineno_delta
        frames.append((code.co_filename, lineno))
        lineno_delta = 0
        frame = frame.f_back
        if frame is None:
            break
    return tuple(frames)

def allocate_bytes(size):
    nframe = tracemalloc.get_traceback_limit()
    bytes_len = (size - EMPTY_STRING_SIZE)
    frames = get_frames(nframe, 1)
    data = b'x' * bytes_len
    return data, frames

def create_snapshots():
    pid = 123
    traceback_limit = 2

    traceback_a_2 = [('a.py', 2),
                     ('b.py', 4)]
    traceback_a_5 = [('a.py', 5),
                     ('b.py', 4)]
    traceback_b_1 = [('b.py', 1)]
    traceback_c_578 = [('c.py', 30)]
    traceback_none_none = [(None, None)]

    timestamp = datetime.datetime(2013, 9, 12, 15, 16, 17)
    stats = {
        'a.py': {2: (30, 3),
                 5: (2, 1)},
        'b.py': {1: (66, 1)},
        None: {None: (7, 1)},
    }
    traces = {
        0x10001: (10, traceback_a_2),
        0x10002: (10, traceback_a_2),
        0x10003: (10, traceback_a_2),

        0x20001: (2, traceback_a_5),

        0x30001: (66, traceback_b_1),

        0x40001: (7, traceback_none_none),
    }
    snapshot = tracemalloc.Snapshot(timestamp, pid, traceback_limit,
                                    stats, traces)
    snapshot.add_metric('process_memory.rss', 1024, 'size')
    snapshot.add_metric('tracemalloc.size', 100, 'size')
    snapshot.add_metric('my_data', 8, 'int')

    timestamp2 = datetime.datetime(2013, 9, 12, 15, 16, 50)
    stats2 = {
        'a.py': {2: (30, 3),
                 5: (5002, 2)},
        'c.py': {578: (400, 1)},
    }
    traces2 = {
        0x10001: (10, traceback_a_2),
        0x10002: (10, traceback_a_2),
        0x10003: (10, traceback_a_2),

        0x20001: (2, traceback_a_5),
        0x20002: (5000, traceback_a_5),

        0x30001: (400, traceback_c_578),
    }
    snapshot2 = tracemalloc.Snapshot(timestamp2, pid, traceback_limit,
                                     stats2, traces2)
    snapshot2.add_metric('process_memory.rss', 1500, 'size')
    snapshot2.add_metric('tracemalloc.size', 200, 'size')
    snapshot2.add_metric('my_data', 10, 'int')

    return (snapshot, snapshot2)


class TestTracemallocEnabled(unittest.TestCase):
    def setUp(self):
        if tracemalloc.is_enabled():
            self.skipTest("tracemalloc must be disabled before the test")

        tracemalloc.clear_filters()
        tracemalloc.add_exclude_filter(tracemalloc.__file__)
        tracemalloc.set_traceback_limit(1)
        tracemalloc.enable()

    def tearDown(self):
        tracemalloc.disable()
        tracemalloc.clear_filters()

    def test_get_tracemalloc_memory(self):
        data = [allocate_bytes(123) for count in range(1000)]
        size, free = tracemalloc.get_tracemalloc_memory()
        self.assertGreaterEqual(size, 0)
        self.assertGreaterEqual(free, 0)
        self.assertGreater(size, free)

        tracemalloc.clear_traces()
        size2, free2 = tracemalloc.get_tracemalloc_memory()
        self.assertLessEqual(size2, size)

    def test_get_trace(self):
        tracemalloc.clear_traces()
        obj_size = 12345
        obj, obj_frames = allocate_bytes(obj_size)
        address = tracemalloc.get_object_address(obj)
        trace = tracemalloc.get_trace(address)
        self.assertIsInstance(trace, tuple)
        size, traceback = trace
        self.assertEqual(size, obj_size)
        self.assertEqual(traceback, obj_frames)

    def test_get_object_trace(self):
        tracemalloc.clear_traces()
        obj_size = 12345
        obj, obj_frames = allocate_bytes(obj_size)
        trace = tracemalloc.get_object_trace(obj)
        self.assertIsInstance(trace, tuple)
        size, traceback = trace
        self.assertEqual(size, obj_size)
        self.assertEqual(traceback, obj_frames)

    def test_set_traceback_limit(self):
        obj_size = 10

        nframe = tracemalloc.get_traceback_limit()
        self.addCleanup(tracemalloc.set_traceback_limit, nframe)

        self.assertRaises(ValueError, tracemalloc.set_traceback_limit, -1)

        tracemalloc.clear_traces()
        tracemalloc.set_traceback_limit(0)
        obj, obj_frames = allocate_bytes(obj_size)
        trace = tracemalloc.get_object_trace(obj)
        size, traceback = trace
        self.assertEqual(len(traceback), 0)
        self.assertEqual(traceback, obj_frames)

        tracemalloc.clear_traces()
        tracemalloc.set_traceback_limit(1)
        obj, obj_frames = allocate_bytes(obj_size)
        trace = tracemalloc.get_object_trace(obj)
        size, traceback = trace
        self.assertEqual(len(traceback), 1)
        self.assertEqual(traceback, obj_frames)

        tracemalloc.clear_traces()
        tracemalloc.set_traceback_limit(10)
        obj2, obj2_frames = allocate_bytes(obj_size)
        trace = tracemalloc.get_object_trace(obj2)
        size, traceback = trace
        self.assertEqual(len(traceback), 10)
        self.assertEqual(traceback, obj2_frames)

    def test_get_traces(self):
        tracemalloc.clear_traces()
        obj_size = 12345
        obj, obj_frames = allocate_bytes(obj_size)

        traces = tracemalloc.get_traces()
        address = tracemalloc.get_object_address(obj)
        self.assertIn(address, traces)
        trace = traces[address]

        self.assertIsInstance(trace, tuple)
        size, traceback = trace
        self.assertEqual(size, obj_size)
        self.assertEqual(traceback, obj_frames)

    def test_get_traces_intern_traceback(self):
        # dummy wrappers to get more useful and identical frames in the traceback
        def allocate_bytes2(size):
            return allocate_bytes(size)
        def allocate_bytes3(size):
            return allocate_bytes2(size)
        def allocate_bytes4(size):
            return allocate_bytes3(size)

        # Ensure that two identical tracebacks are not duplicated
        tracemalloc.clear_traces()
        tracemalloc.set_traceback_limit(4)
        obj_size = 123
        obj1, obj1_frames = allocate_bytes4(obj_size)
        obj2, obj2_frames = allocate_bytes4(obj_size)

        traces = tracemalloc.get_traces()

        address1 = tracemalloc.get_object_address(obj1)
        address2 = tracemalloc.get_object_address(obj2)
        trace1 = traces[address1]
        trace2 = traces[address2]
        size1, traceback1 = trace1
        size2, traceback2 = trace2
        self.assertEqual(traceback2, traceback1)
        self.assertIs(traceback2, traceback1)

    def test_get_process_memory(self):
        tracemalloc.clear_traces()
        obj_size = 5 * 1024 * 1024

        orig = tracemalloc.get_process_memory()
        if orig is None:
            self.skipTest("get_process_memory is not supported")
        self.assertGreater(orig[0], 0)
        self.assertGreater(orig[1], 0)

        obj, obj_frames = allocate_bytes(obj_size)
        curr = tracemalloc.get_process_memory()
        self.assertGreaterEqual(curr[0], orig[0])
        self.assertGreaterEqual(curr[1], orig[1])

    def test_get_traced_memory(self):
        # get the allocation location to filter allocations
        size = 12345
        obj, frames = allocate_bytes(size)
        filename, lineno = frames[0]
        tracemalloc.add_include_filter(filename, lineno)

        # allocate one object
        tracemalloc.clear_traces()
        obj, obj_frames = allocate_bytes(size)
        self.assertEqual(tracemalloc.get_traced_memory(), (size, size))

        # destroy the object
        obj = None
        self.assertEqual(tracemalloc.get_traced_memory(), (0, size))

        # clear_traces() must reset traced memory counters
        tracemalloc.clear_traces()
        self.assertEqual(tracemalloc.get_traced_memory(), (0, 0))

        # allocate another object
        tracemalloc.clear_traces()
        obj, obj_frames = allocate_bytes(size)
        self.assertEqual(tracemalloc.get_traced_memory(), (size, size))

        # disable() rests also traced memory counters
        tracemalloc.disable()
        self.assertEqual(tracemalloc.get_traced_memory(), (0, 0))

    def test_get_stats(self):
        tracemalloc.clear_traces()
        total_size = 0
        total_count = 0
        objs = []
        for index in range(5):
            size = 1234
            obj, obj_frames = allocate_bytes(size)
            objs.append(obj)
            total_size += size
            total_count += 1

            stats = tracemalloc.get_stats()
            for filename, line_stats in stats.items():
                for lineno, line_stat in line_stats.items():
                    # stats can be huge, one test per file should be enough
                    self.assertIsInstance(line_stat, tuple)
                    size, count = line_stat
                    self.assertIsInstance(size, INT_TYPES)
                    self.assertIsInstance(count, INT_TYPES)
                    self.assertGreaterEqual(size, 0)
                    self.assertGreaterEqual(count, 1)
                    break

            filename, lineno = obj_frames[0]
            self.assertIn(filename, stats)
            line_stats = stats[filename]
            self.assertIn(lineno, line_stats)
            size, count = line_stats[lineno]
            self.assertEqual(size, total_size)
            self.assertEqual(count, total_count)

    def test_clear_traces(self):
        tracemalloc.clear_traces()
        obj_size = 1234
        obj, obj_frames = allocate_bytes(obj_size)

        stats = tracemalloc.get_stats()
        filename, lineno = obj_frames[0]
        line_stats = stats[filename][lineno]
        size, count = line_stats
        self.assertEqual(size, obj_size)
        self.assertEqual(count, 1)

        tracemalloc.clear_traces()
        stats2 = tracemalloc.get_stats()
        self.assertNotIn(lineno, stats2[filename])

    def test_task_is_scheduled(self):
        task = tracemalloc.Task(noop)
        task.set_delay(60)

        task.schedule()
        self.assertTrue(task.is_scheduled())
        scheduled = tracemalloc.get_tasks()
        self.assertIn(task, scheduled)

        task.cancel()
        self.assertFalse(task.is_scheduled())
        scheduled = tracemalloc.get_tasks()
        self.assertNotIn(task, scheduled)

        # second cancel() should not fail
        task.cancel()

        # reschedule
        task.schedule()
        self.assertTrue(task.is_scheduled())

        # schedule twice should not fail
        task.schedule()

        # cannot schedule if the tracemalloc module is disabled
        tracemalloc.disable()
        with self.assertRaises(RuntimeError) as cm:
            task.schedule()
        self.assertEqual(str(cm.exception),
                         "the tracemalloc module must be enabled "
                         "to schedule a task")

    def test_cancel_tasks(self):
        task = tracemalloc.Task(noop)
        task.set_delay(60)

        task.schedule()
        self.assertTrue(task.is_scheduled())

        tracemalloc.cancel_tasks()
        self.assertFalse(task.is_scheduled())

    def test_task_delay(self):
        calls = []
        def log_func(*args, **kw):
            calls.append(log_func)

        args = (1, 2, 3)
        kwargs = {'arg': 4}
        task = tracemalloc.Task(log_func)
        task.set_delay(1)
        task.schedule()
        time.sleep(1)
        obj, source = allocate_bytes(123)
        self.assertEqual(len(calls), 1)
        self.assertIs(calls[0], log_func)

    def test_task_memory_threshold(self):
        diffs = []
        def log_func(*args, **kw):
            size, max_size = tracemalloc.get_traced_memory()
            diffs.append(size - old_size)

        obj_size  = 1024 * 1024
        threshold = int(obj_size * 0.75)
        args = (1, 2, 3)
        kwargs = {'arg': 4}

        old_size, max_size = tracemalloc.get_traced_memory()
        task = tracemalloc.Task(log_func, args, kwargs)
        task.set_memory_threshold(threshold)
        before = tracemalloc.get_traced_memory()[0]
        task.schedule()

        # allocate
        obj, source = allocate_bytes(obj_size)
        after = tracemalloc.get_traced_memory()[0]
        print("DIFF", after - before, "obj", obj_size, threshold)
        self.assertEqual(len(diffs), 1)
        print(diffs)
        self.assertGreaterEqual(diffs[0], obj_size)

        # release
        del diffs[:]
        old_size, max_size = tracemalloc.get_traced_memory()
        obj = None
        size, max_size = tracemalloc.get_traced_memory()
        self.assertEqual(len(diffs), 1)
        self.assertLessEqual(diffs[0], obj_size)

    def test_task_repeat(self):
        calls = []
        def log_func():
            calls.append(log_func)

        task = tracemalloc.Task(log_func)
        task.set_memory_threshold(1)

        # allocate at least 100 memory blocks, but the task should only be
        # called 3 times
        task.schedule(3)
        objects = [object() for n in range(100)]
        self.assertEqual(len(calls), 3)

    def test_disable_scheduled_tasks(self):
        task = tracemalloc.Task(noop)
        task.set_delay(60)
        task.schedule()
        self.assertTrue(task.is_scheduled())
        tracemalloc.disable()
        self.assertFalse(task.is_scheduled())

    def test_task_callback_error(self):
        calls = []
        def failing_func():
            calls.append((failing_func,))
            raise ValueError("oops")

        task = tracemalloc.Task(failing_func)
        task.set_memory_threshold(1)

        # If the task raises an exception, the exception should be logged to
        # sys.stderr (don't raise an exception at a random place)
        with support.captured_stderr() as stderr:
            task.schedule()

            obj, source = allocate_bytes(123)
            obj2, source = allocate_bytes(456)
            # the timer should not be rescheduler on error
            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0], (failing_func,))
        output = stderr.getvalue()
        self.assertRegexpMatches(output, 'ValueError.*oops')

        self.assertFalse(task.is_scheduled())

    def test_is_enabled(self):
        tracemalloc.clear_traces()
        tracemalloc.disable()
        self.assertFalse(tracemalloc.is_enabled())

        tracemalloc.enable()
        self.assertTrue(tracemalloc.is_enabled())

    def test_snapshot(self):
        def compute_nstats(stats):
            return sum(len(line_stats)
                       for filename, line_stats in stats.items())

        tracemalloc.clear_traces()
        obj, source = allocate_bytes(123)

        stats1 = tracemalloc.get_stats()
        traces = tracemalloc.get_stats()
        nstat1 = compute_nstats(stats1)

        # take a snapshot with traces
        snapshot = tracemalloc.Snapshot.create(traces=True)
        nstat2 = compute_nstats(snapshot.stats)
        self.assertGreaterEqual(nstat2, nstat2)
        self.assertEqual(snapshot.pid, os.getpid())
        process_rss = snapshot.get_metric('process_rss')
        if process_rss is not None:
            self.assertGreater(process_rss, 0)
        process_vms = snapshot.get_metric('process_vms')
        if process_rss is not None:
            self.assertGreater(process_vms, 0)

        self.assertIsInstance(snapshot.metrics, dict)
        for key in ('tracemalloc.module.free',
                    'tracemalloc.module.size',
                    'tracemalloc.module.fragmentation',
                    'tracemalloc.traced.max_size',
                    'tracemalloc.traced.size'):
            self.assertIn(key, snapshot.metrics)

        # write on disk
        snapshot.write(support.TESTFN)
        self.addCleanup(support.unlink, support.TESTFN)

        # load with traces
        snapshot2 = tracemalloc.Snapshot.load(support.TESTFN)
        self.assertEqual(snapshot2.timestamp, snapshot.timestamp)
        self.assertEqual(snapshot2.pid, snapshot.pid)
        self.assertEqual(snapshot2.traces, snapshot.traces)
        self.assertEqual(snapshot2.stats, snapshot.stats)
        self.assertEqual(snapshot2.metrics, snapshot.metrics)

        # load without traces
        snapshot2 = tracemalloc.Snapshot.create()
        self.assertIsNone(snapshot2.traces)

        # tracemalloc must be enabled to take a snapshot
        tracemalloc.disable()
        with self.assertRaises(RuntimeError) as cm:
            tracemalloc.Snapshot.create()
        self.assertEqual(str(cm.exception),
                         "the tracemalloc module must be enabled "
                         "to take a snapshot")

    def test_snapshot_metrics(self):
        now = datetime.datetime.now()
        snapshot = tracemalloc.Snapshot(now, 123, 1, {})

        metric = snapshot.add_metric('key', 3, 'size')
        self.assertRaises(ValueError, snapshot.add_metric, 'key', 4, 'size')
        self.assertEqual(snapshot.get_metric('key'), 3)
        self.assertIn('key', snapshot.metrics)
        self.assertIs(metric, snapshot.metrics['key'])

    def test_take_snapshot(self):
        def callback(snapshot):
            snapshot.add_metric('callback', 5, 'size')

        with support.temp_cwd() as temp_dir:
            task = tracemalloc.TakeSnapshotTask(callback=callback)
            for index in range(1, 4):
                snapshot, filename = task.take_snapshot()
                self.assertEqual(snapshot.get_metric('callback'), 5)
                self.assertEqual(filename,
                                 'tracemalloc-%04d.pickle' % index)
                self.assertTrue(os.path.exists(filename))

    def test_filters(self):
        tracemalloc.clear_filters()
        tracemalloc.add_exclude_filter(tracemalloc.__file__)
        # test multiple inclusive filters
        tracemalloc.add_include_filter('should never match 1')
        tracemalloc.add_include_filter('should never match 2')
        tracemalloc.add_include_filter(__file__)
        tracemalloc.clear_traces()
        size = 1000
        obj, obj_frames = allocate_bytes(size)
        trace = tracemalloc.get_object_trace(obj)
        self.assertIsNotNone(trace)

        # test exclusive filter, based on previous filters
        filename, lineno = obj_frames[0]
        tracemalloc.add_exclude_filter(filename, lineno)
        tracemalloc.clear_traces()
        obj, obj_frames = allocate_bytes(size)
        trace = tracemalloc.get_object_trace(obj)
        self.assertIsNone(trace)

    def fork_child(self):
        # tracemalloc must be disabled after fork
        enabled = tracemalloc.is_enabled()
        if enabled:
            return 2

        # ensure that tracemalloc can be reenabled after fork
        tracemalloc.enable()

        # check that tracemalloc is still working
        obj_size = 12345
        obj, obj_frames = allocate_bytes(obj_size)
        trace = tracemalloc.get_object_trace(obj)
        if trace is None:
            return 3

        # everything is fine
        return 0

    @unittest.skipUnless(hasattr(os, 'fork'), 'need os.fork()')
    def test_fork(self):
        pid = os.fork()
        if not pid:
            # child
            exitcode = 1
            try:
                exitcode = self.fork_child()
            finally:
                os._exit(exitcode)
        else:
            pid2, status = os.waitpid(pid, 0)
            self.assertTrue(os.WIFEXITED(status))
            exitcode = os.WEXITSTATUS(status)
            self.assertEqual(exitcode, 0)


class TestSnapshot(unittest.TestCase):
    # FIXME: test on Python 2
    @unittest.skipUnless(PYTHON3, 'need python 3')
    def test_create_snapshot(self):
        stats = {'a.py': {1: (5, 1)}}
        traces = {0x123: (5, ('a.py', 1))}

        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(tracemalloc, 'is_enabled', return_value=True))
            stack.enter_context(patch.object(os, 'getpid', return_value=77))
            stack.enter_context(patch.object(tracemalloc, 'get_traceback_limit', return_value=5))
            stack.enter_context(patch.object(tracemalloc, 'get_stats', return_value=stats))
            stack.enter_context(patch.object(tracemalloc, 'get_traces', return_value=traces))

            snapshot = tracemalloc.Snapshot.create(traces=True, metrics=False)
            self.assertIsInstance(snapshot.timestamp, datetime.datetime)
            self.assertEqual(snapshot.pid, 77)
            self.assertEqual(snapshot.traceback_limit, 5)
            self.assertEqual(snapshot.stats, stats)
            self.assertEqual(snapshot.traces, traces)
            self.assertEqual(snapshot.metrics, {})

class TestFilters(unittest.TestCase):
    maxDiff = 2048
    def test_add_clear_filter(self):
        old_filters = tracemalloc.get_filters()
        try:
            # test add_filter()
            tracemalloc.clear_filters()
            tracemalloc.add_filter(tracemalloc.Filter(True, "abc", 3))
            tracemalloc.add_filter(tracemalloc.Filter(False, "12345", 0))
            self.assertEqual(tracemalloc.get_filters(),
                             [tracemalloc.Filter(True, 'abc', 3, False),
                              tracemalloc.Filter(False, '12345', None, False)])

            # test add_include_filter(), add_exclude_filter()
            tracemalloc.clear_filters()
            tracemalloc.add_include_filter("abc", 3)
            tracemalloc.add_exclude_filter("12345", 0)
            tracemalloc.add_exclude_filter("6789", None)
            tracemalloc.add_exclude_filter("def#", 55)
            tracemalloc.add_exclude_filter("trace", 123, True)
            self.assertEqual(tracemalloc.get_filters(),
                             [tracemalloc.Filter(True, 'abc', 3, False),
                              tracemalloc.Filter(False, '12345', None, False),
                              tracemalloc.Filter(False, '6789', None, False),
                              tracemalloc.Filter(False, "def#", 55, False),
                              tracemalloc.Filter(False, "trace", 123, True)])

            # test filename normalization (.pyc/.pyo)
            tracemalloc.clear_filters()
            tracemalloc.add_include_filter("abc.pyc")
            tracemalloc.add_include_filter("name.pyo")
            self.assertEqual(tracemalloc.get_filters(),
                             [tracemalloc.Filter(True, 'abc.py', None, False),
                              tracemalloc.Filter(True, 'name.py', None, False) ])

            # test filename normalization ('*' joker character)
            tracemalloc.clear_filters()
            tracemalloc.add_include_filter('a****b')
            tracemalloc.add_include_filter('***x****')
            tracemalloc.add_include_filter('1*2**3***4')
            self.assertEqual(tracemalloc.get_filters(),
                             [tracemalloc.Filter(True, 'a*b', None, False),
                              tracemalloc.Filter(True, '*x*', None, False),
                              tracemalloc.Filter(True, '1*2*3*4', None, False)])

            # ignore duplicated filters
            tracemalloc.clear_filters()
            tracemalloc.add_include_filter('a.py')
            tracemalloc.add_include_filter('a.py', 5)
            tracemalloc.add_include_filter('a.py')
            tracemalloc.add_include_filter('a.py', 5)
            tracemalloc.add_exclude_filter('b.py')
            tracemalloc.add_exclude_filter('b.py', 10)
            tracemalloc.add_exclude_filter('b.py')
            tracemalloc.add_exclude_filter('b.py', 10, True)
            self.assertEqual(tracemalloc.get_filters(),
                             [tracemalloc.Filter(True, 'a.py', None, False),
                              tracemalloc.Filter(True, 'a.py', 5, False),
                              tracemalloc.Filter(False, 'b.py', None, False),
                              tracemalloc.Filter(False, 'b.py', 10, False),
                              tracemalloc.Filter(False, 'b.py', 10, True)])

            # Windows: test filename normalization (lower case, slash)
            if os.name == "nt":
                tracemalloc.clear_filters()
                tracemalloc.add_include_filter("aBcD\xC9")
                tracemalloc.add_include_filter("MODule.PYc")
                tracemalloc.add_include_filter(r"path/to\file")
                self.assertEqual(tracemalloc.get_filters(),
                                 [tracemalloc.Filter(True, 'abcd\xe9', None, False),
                                  tracemalloc.Filter(True, 'module.py', None, False),
                                  tracemalloc.Filter(True, r'path\to\file', None, False)])

            # test clear_filters()
            tracemalloc.clear_filters()
            self.assertEqual(tracemalloc.get_filters(), [])
        finally:
            tracemalloc.clear_filters()
            for trace_filter in old_filters:
                tracemalloc.add_filter(trace_filter)

    def test_filter_attributes(self):
        # test default values
        f = tracemalloc.Filter(True, "abc")
        self.assertEqual(f.include, True)
        self.assertEqual(f.pattern, "abc")
        self.assertIsNone(f.lineno)
        self.assertEqual(f.traceback, False)

        # test custom values
        f = tracemalloc.Filter(False, "test.py", 123, True)
        self.assertEqual(f.include, False)
        self.assertEqual(f.pattern, "test.py")
        self.assertEqual(f.lineno, 123)
        self.assertEqual(f.traceback, True)

        # attributes are read-only
        self.assertRaises(AttributeError, setattr, f, "include", True)
        self.assertRaises(AttributeError, setattr, f, "pattern", True)
        self.assertRaises(AttributeError, setattr, f, "lineno", True)
        self.assertRaises(AttributeError, setattr, f, "traceback", True)

    def test_filter_match(self):
        f = tracemalloc.Filter(True, "abc")
        self.assertTrue(f.match("abc", 5))
        self.assertTrue(f.match("abc", None))
        self.assertFalse(f.match("12356", 5))
        self.assertFalse(f.match("12356", None))
        self.assertFalse(f.match(None, 5))
        self.assertFalse(f.match(None, None))

        f = tracemalloc.Filter(False, "abc")
        self.assertFalse(f.match("abc", 5))
        self.assertFalse(f.match("abc", None))
        self.assertTrue(f.match("12356", 5))
        self.assertTrue(f.match("12356", None))
        self.assertTrue(f.match(None, 5))
        self.assertTrue(f.match(None, None))

        f = tracemalloc.Filter(True, "abc", 5)
        self.assertTrue(f.match("abc", 5))
        self.assertFalse(f.match("abc", 10))
        self.assertFalse(f.match("abc", None))
        self.assertFalse(f.match("12356", 5))
        self.assertFalse(f.match("12356", 10))
        self.assertFalse(f.match("12356", None))
        self.assertFalse(f.match(None, 5))
        self.assertFalse(f.match(None, 10))
        self.assertFalse(f.match(None, None))

        f = tracemalloc.Filter(False, "abc", 5)
        self.assertFalse(f.match("abc", 5))
        self.assertTrue(f.match("abc", 10))
        self.assertTrue(f.match("abc", None))
        self.assertTrue(f.match("12356", 5))
        self.assertTrue(f.match("12356", 10))
        self.assertTrue(f.match("12356", None))
        self.assertTrue(f.match(None, 5))
        self.assertTrue(f.match(None, 10))
        self.assertTrue(f.match(None, None))

    def test_filter_match_lineno(self):
        f = tracemalloc.Filter(True, "unused")
        self.assertTrue(f.match_lineno(5))
        self.assertTrue(f.match_lineno(10))
        self.assertTrue(f.match_lineno(None))

        f = tracemalloc.Filter(True, "unused", 5)
        self.assertTrue(f.match_lineno(5))
        self.assertFalse(f.match_lineno(10))
        self.assertFalse(f.match_lineno(None))

        f = tracemalloc.Filter(False, "unused")
        self.assertTrue(f.match_lineno(5))
        self.assertTrue(f.match_lineno(10))
        self.assertTrue(f.match_lineno(None))

        f = tracemalloc.Filter(False, "unused", 5)
        self.assertFalse(f.match_lineno(5))
        self.assertTrue(f.match_lineno(10))
        self.assertTrue(f.match_lineno(None))

    def test_filter_match_filename(self):
        f = tracemalloc.Filter(True, "abc")
        self.assertTrue(f.match_filename("abc"))
        self.assertFalse(f.match_filename("12356"))
        self.assertFalse(f.match_filename(None))

        f = tracemalloc.Filter(False, "abc")
        self.assertFalse(f.match_filename("abc"))
        self.assertTrue(f.match_filename("12356"))
        self.assertTrue(f.match_filename(None))

        f = tracemalloc.Filter(True, "abc")
        self.assertTrue(f.match_filename("abc"))
        self.assertFalse(f.match_filename("12356"))
        self.assertFalse(f.match_filename(None))

        f = tracemalloc.Filter(False, "abc")
        self.assertFalse(f.match_filename("abc"))
        self.assertTrue(f.match_filename("12356"))
        self.assertTrue(f.match_filename(None))

    def test_filter_match_filename_joker(self):
        def fnmatch(filename, pattern):
            filter = tracemalloc.Filter(True, pattern)
            return filter.match_filename(filename)

        # no *
        self.assertTrue(fnmatch('abc', 'abc'))
        self.assertFalse(fnmatch('abc', 'abcd'))
        self.assertFalse(fnmatch('abc', 'def'))

        # a*
        self.assertTrue(fnmatch('abc', 'a*'))
        self.assertTrue(fnmatch('abc', 'abc*'))
        self.assertFalse(fnmatch('abc', 'b*'))
        self.assertFalse(fnmatch('abc', 'abcd*'))

        # a*b
        self.assertTrue(fnmatch('abc', 'a*c'))
        self.assertTrue(fnmatch('abcdcx', 'a*cx'))
        self.assertFalse(fnmatch('abb', 'a*c'))
        self.assertFalse(fnmatch('abcdce', 'a*cx'))

        # a*b*c
        self.assertTrue(fnmatch('abcde', 'a*c*e'))
        self.assertTrue(fnmatch('abcbdefeg', 'a*bd*eg'))
        self.assertFalse(fnmatch('abcdd', 'a*c*e'))
        self.assertFalse(fnmatch('abcbdefef', 'a*bd*eg'))

        # replace .pyc and .pyo suffix with .py
        self.assertTrue(fnmatch('a.pyc', 'a.py'))
        self.assertTrue(fnmatch('a.pyo', 'a.py'))
        self.assertTrue(fnmatch('a.py', 'a.pyc'))
        self.assertTrue(fnmatch('a.py', 'a.pyo'))

        if os.name == 'nt':
            # case insensitive
            self.assertTrue(fnmatch('aBC', 'ABc'))
            self.assertTrue(fnmatch('aBcDe', 'Ab*dE'))

            self.assertTrue(fnmatch('a.pyc', 'a.PY'))
            self.assertTrue(fnmatch('a.PYO', 'a.py'))
            self.assertTrue(fnmatch('a.py', 'a.PYC'))
            self.assertTrue(fnmatch('a.PY', 'a.pyo'))
        else:
            # case sensitive
            self.assertFalse(fnmatch('aBC', 'ABc'))
            self.assertFalse(fnmatch('aBcDe', 'Ab*dE'))

            self.assertFalse(fnmatch('a.pyc', 'a.PY'))
            self.assertFalse(fnmatch('a.PYO', 'a.py'))
            self.assertFalse(fnmatch('a.py', 'a.PYC'))
            self.assertFalse(fnmatch('a.PY', 'a.pyo'))

        if os.name == 'nt':
            # normalize alternate separator "/" to the standard separator "\"
            self.assertTrue(fnmatch(r'a/b', r'a\b'))
            self.assertTrue(fnmatch(r'a\b', r'a/b'))
            self.assertTrue(fnmatch(r'a/b\c', r'a\b/c'))
            self.assertTrue(fnmatch(r'a/b/c', r'a\b\c'))
        else:
            # there is no alternate separator
            self.assertFalse(fnmatch(r'a/b', r'a\b'))
            self.assertFalse(fnmatch(r'a\b', r'a/b'))
            self.assertFalse(fnmatch(r'a/b\c', r'a\b/c'))
            self.assertFalse(fnmatch(r'a/b/c', r'a\b\c'))

        # a******b
        N = 10 ** 6
        self.assertTrue (fnmatch('a' * N,       '*' * N))
        self.assertTrue (fnmatch('a' * N + 'c', '*' * N))
        self.assertTrue (fnmatch('a' * N,       'a' + '*' * N + 'a'))
        self.assertTrue (fnmatch('a' * N + 'b', 'a' + '*' * N + 'b'))
        self.assertFalse(fnmatch('a' * N + 'b', 'a' + '*' * N + 'c'))

        # a*a*a*a*
        self.assertTrue(fnmatch('a' * 10, 'a*' * 10))
        self.assertFalse(fnmatch('a' * 10, 'a*' * 10 + 'b'))
        with self.assertRaises(ValueError) as cm:
            fnmatch('abc', 'a*' * 101)
        self.assertEqual(str(cm.exception),
                         "too many joker characters in the filename pattern")

    def test_filter_match_trace(self):
        t1 = (("a.py", 2), ("b.py", 3))
        t2 = (("b.py", 4), ("b.py", 5))

        f = tracemalloc.Filter(True, "b.py", traceback=True)
        self.assertTrue(f.match_traceback(t1))
        self.assertTrue(f.match_traceback(t2))

        f = tracemalloc.Filter(True, "b.py", traceback=False)
        self.assertFalse(f.match_traceback(t1))
        self.assertTrue(f.match_traceback(t2))

        f = tracemalloc.Filter(False, "b.py", traceback=True)
        self.assertFalse(f.match_traceback(t1))
        self.assertFalse(f.match_traceback(t2))

        f = tracemalloc.Filter(False, "b.py", traceback=False)
        self.assertTrue(f.match_traceback(t1))
        self.assertFalse(f.match_traceback(t2))


class TestCommandLine(unittest.TestCase):
    def test_env_var(self):
        # disabled by default
        code = 'import tracemalloc; print(tracemalloc.is_enabled())'
        ok, stdout, stderr = assert_python_ok('-c', code)
        stdout = stdout.rstrip()
        self.assertEqual(stdout, b'False')

        # PYTHON* environment varibles must be ignored when -E option is
        # present
        code = 'import tracemalloc; print(tracemalloc.is_enabled())'
        ok, stdout, stderr = assert_python_ok('-E', '-c', code, PYTHONTRACEMALLOC='1')
        stdout = stdout.rstrip()
        self.assertEqual(stdout, b'False')

        # enabled by default
        code = 'import tracemalloc; print(tracemalloc.is_enabled())'
        ok, stdout, stderr = assert_python_ok('-c', code, PYTHONTRACEMALLOC='1')
        stdout = stdout.rstrip()
        self.assertEqual(stdout, b'True')


class TestTop(unittest.TestCase):
    maxDiff = 2048

    def test_snapshot_top_by_line(self):
        snapshot, snapshot2 = create_snapshots()

        # stats per file and line
        top_stats = snapshot.top_by('line')
        self.assertEqual(top_stats.stats, {
            ('a.py', 2): (30, 3),
            ('a.py', 5): (2, 1),
            ('b.py', 1): (66, 1),
            (None, None): (7, 1),
        })
        self.assertEqual(top_stats.group_by, 'line')
        self.assertEqual(top_stats.timestamp, snapshot.timestamp)
        self.assertEqual(top_stats.cumulative, False)
        self.assertEqual(top_stats.metrics, snapshot.metrics)

        # stats per file and line (2)
        top_stats2 = snapshot2.top_by('line')
        self.assertEqual(top_stats2.stats, {
            ('a.py', 2): (30, 3),
            ('a.py', 5): (5002, 2),
            ('c.py', 578): (400, 1),
        })
        self.assertEqual(top_stats2.group_by, 'line')
        self.assertEqual(top_stats2.timestamp, snapshot2.timestamp)
        self.assertEqual(top_stats2.cumulative, False)
        self.assertEqual(top_stats2.metrics, snapshot2.metrics)

        # stats diff per file and line
        top_diff = top_stats2.compare_to(top_stats)
        self.assertIsInstance(top_diff, tracemalloc.StatsDiff)
        top_diff.sort()
        self.assertEqual(top_diff.differences, [
            (5000, 5002, 1, 2, ('a.py', 5)),
            (400, 400, 1, 1, ('c.py', 578)),
            (-66, 0, -1, 0, ('b.py', 1)),
            (-7, 0, -1, 0, ('', 0)),
            (0, 30, 0, 3, ('a.py', 2)),
        ])

    def test_snapshot_top_by_file(self):
        snapshot, snapshot2 = create_snapshots()

        # stats per file
        top_stats = snapshot.top_by('filename')
        self.assertEqual(top_stats.stats, {
            'a.py': (32, 4),
            'b.py': (66, 1),
            None: (7, 1),
        })
        self.assertEqual(top_stats.group_by, 'filename')
        self.assertEqual(top_stats.timestamp, snapshot.timestamp)
        self.assertEqual(top_stats.cumulative, False)
        self.assertEqual(top_stats.metrics, snapshot.metrics)

        # stats per file (2)
        top_stats2 = snapshot2.top_by('filename')
        self.assertEqual(top_stats2.stats, {
            'a.py': (5032, 5),
            'c.py': (400, 1),
        })
        self.assertEqual(top_stats2.group_by, 'filename')
        self.assertEqual(top_stats2.timestamp, snapshot2.timestamp)
        self.assertEqual(top_stats2.cumulative, False)
        self.assertEqual(top_stats2.metrics, snapshot2.metrics)

        # stats diff per file
        top_diff = top_stats2.compare_to(top_stats)
        self.assertIsInstance(top_diff, tracemalloc.StatsDiff)
        top_diff.sort()
        self.assertEqual(top_diff.differences, [
            (5000, 5032, 1, 5, 'a.py'),
            (400, 400, 1, 1, 'c.py'),
            (-66, 0, -1, 0, 'b.py'),
            (-7, 0, -1, 0, ''),
        ])

    def test_snapshot_top_by_address(self):
        snapshot, snapshot2 = create_snapshots()

        # stats per address
        top_stats = snapshot.top_by('address')
        self.assertEqual(top_stats.stats, {
            0x10001: (10, 1),
            0x10002: (10, 1),
            0x10003: (10, 1),
            0x20001: (2, 1),
            0x30001: (66, 1),
            0x40001: (7, 1),
        })
        self.assertEqual(top_stats.group_by, 'address')
        self.assertEqual(top_stats.timestamp, snapshot.timestamp)
        self.assertEqual(top_stats.cumulative, False)
        self.assertEqual(top_stats.metrics, snapshot.metrics)

        # stats per address (2)
        top_stats2 = snapshot2.top_by('address')
        self.assertEqual(top_stats2.stats, {
            0x10001: (10, 1),
            0x10002: (10, 1),
            0x10003: (10, 1),
            0x20001: (2, 1),
            0x20002: (5000, 1),
            0x30001: (400, 1),
        })
        self.assertEqual(top_stats2.group_by, 'address')
        self.assertEqual(top_stats2.timestamp, snapshot2.timestamp)
        self.assertEqual(top_stats2.cumulative, False)
        self.assertEqual(top_stats2.metrics, snapshot2.metrics)

        # diff
        top_diff = top_stats2.compare_to(top_stats)
        self.assertIsInstance(top_diff, tracemalloc.StatsDiff)
        top_diff.sort()
        self.assertEqual(top_diff.differences, [
            (5000, 5000, 1, 1, 0x20002),
            (334, 400, 0, 1, 0x30001),
            (-7, 0, -1, 0, 0x40001),
            (0, 10, 0, 1, 0x10003),
            (0, 10, 0, 1, 0x10002),
            (0, 10, 0, 1, 0x10001),
            (0, 2, 0, 1, 0x20001),
        ])

        with self.assertRaises(ValueError) as cm:
            snapshot.traces = None
            snapshot.top_by('address')
        self.assertEqual(str(cm.exception), "need traces")

    def test_snapshot_top_cumulative(self):
        snapshot, snapshot2 = create_snapshots()

        # per file
        top_stats = snapshot.top_by('filename', True)
        self.assertEqual(top_stats.stats, {
            'a.py': (32, 4),
            'b.py': (98, 5),
            None: (7, 1),
        })
        self.assertEqual(top_stats.group_by, 'filename')
        self.assertEqual(top_stats.timestamp, snapshot.timestamp)
        self.assertEqual(top_stats.cumulative, True)
        self.assertEqual(top_stats.metrics, snapshot.metrics)

        # per line
        top_stats2 = snapshot.top_by('line', True)
        self.assertEqual(top_stats2.stats, {
            ('a.py', 2): (30, 3),
            ('a.py', 5): (2, 1),
            ('b.py', 1): (66, 1),
            ('b.py', 4): (32, 4),
            (None, None): (7, 1),
        })
        self.assertEqual(top_stats2.group_by, 'line')
        self.assertEqual(top_stats2.timestamp, snapshot.timestamp)
        self.assertEqual(top_stats2.cumulative, True)
        self.assertEqual(top_stats2.metrics, snapshot.metrics)

        with self.assertRaises(ValueError) as cm:
            snapshot.traces = None
            snapshot.top_by('filename', True)
        self.assertEqual(str(cm.exception), "need traces")

    def test_display_top_by_line(self):
        snapshot, snapshot2 = create_snapshots()

        # top per line
        output = StringIO()
        top = tracemalloc.DisplayTop()
        top.display_snapshot(snapshot, file=output)
        text = output.getvalue()
        self.assertEqual(text, '''
2013-09-12 15:16:17: Top 4 allocations per filename and line number
#1: b.py:1: size=66 B, count=1
#2: a.py:2: size=30 B, count=3, average=10 B
#3: ???:?: size=7 B, count=1
#4: a.py:5: size=2 B, count=1
Traced Python memory: size=105 B, count=6, average=17 B

my_data: 8
process_memory.rss: 1024 B
tracemalloc.size: 100 B
        '''.strip() + '\n\n')

        # diff per line
        output = StringIO()
        top.display_snapshot(snapshot2, count=3, file=output)
        text = output.getvalue()
        self.assertEqual(text, '''
2013-09-12 15:16:50: Top 3 allocations per filename and line number (compared to 2013-09-12 15:16:17)
#1: a.py:5: size=5002 B (+5000 B), count=2 (+1), average=2501 B
#2: c.py:578: size=400 B (+400 B), count=1 (+1)
#3: b.py:1: size=0 B (-66 B), count=0 (-1)
2 more: size=30 B (-7 B), count=3 (-1), average=10 B
Traced Python memory: size=5 KiB (+5 KiB), count=6 (+0), average=905 B

my_data: 10 (+2)
process_memory.rss: 1500 B (+476 B)
tracemalloc.size: 200 B (+100 B)
        '''.strip() + '\n\n')

    def test_display_top_by_file(self):
        snapshot, snapshot2 = create_snapshots()

        # group per file
        output = StringIO()
        top = tracemalloc.DisplayTop()
        top.display_snapshot(snapshot, group_by='filename', file=output)
        text = output.getvalue()
        self.assertEqual(text, '''
2013-09-12 15:16:17: Top 3 allocations per filename
#1: b.py: size=66 B, count=1
#2: a.py: size=32 B, count=4, average=8 B
#3: ???: size=7 B, count=1
Traced Python memory: size=105 B, count=6, average=17 B

my_data: 8
process_memory.rss: 1024 B
tracemalloc.size: 100 B
        '''.strip() + '\n\n')

        # diff per file
        output = StringIO()
        top.display_snapshot(snapshot2, group_by='filename', file=output)
        text = output.getvalue()
        self.assertEqual(text, '''
2013-09-12 15:16:50: Top 4 allocations per filename (compared to 2013-09-12 15:16:17)
#1: a.py: size=5032 B (+5000 B), count=5 (+1), average=1006 B
#2: c.py: size=400 B (+400 B), count=1 (+1)
#3: b.py: size=0 B (-66 B), count=0 (-1)
#4: ???: size=0 B (-7 B), count=0 (-1)
Traced Python memory: size=5 KiB (+5 KiB), count=6 (+0), average=905 B

my_data: 10 (+2)
process_memory.rss: 1500 B (+476 B)
tracemalloc.size: 200 B (+100 B)
        '''.strip() + '\n\n')

    def test_display_top_options(self):
        snapshot, snapshot2 = create_snapshots()
        output = StringIO()
        top = tracemalloc.DisplayTop()
        top.metrics = False
        top.average = False
        top.count = False
        top.display_snapshot(snapshot, file=output)
        text = output.getvalue()
        self.assertEqual(text, '''
2013-09-12 15:16:17: Top 4 allocations per filename and line number
#1: b.py:1: 66 B
#2: a.py:2: 30 B
#3: ???:?: 7 B
#4: a.py:5: 2 B
Traced Python memory: 105 B
        '''.strip() + '\n\n')

    # FIXME: test on Python 2
    @unittest.skipUnless(PYTHON3, 'need python 3')
    def test_display_top_task(self):
        def callback(snapshot):
            snapshot.add_metric('task', 700, 'size')

        snapshot, snapshot2 = create_snapshots()

        # top per file (default options)
        output = StringIO()
        top = tracemalloc.DisplayTop()

        with patch.object(tracemalloc.Snapshot,
                          'create', return_value=snapshot):
            top.display(2, group_by='filename', file=output, callback=callback)
        text = output.getvalue()
        self.assertEqual(text, '''
2013-09-12 15:16:17: Top 2 allocations per filename
#1: b.py: size=66 B, count=1
#2: a.py: size=32 B, count=4, average=8 B
1 more: size=7 B, count=1
Traced Python memory: size=105 B, count=6, average=17 B

my_data: 8
process_memory.rss: 1024 B
task: 700 B
tracemalloc.size: 100 B
        '''.strip() + '\n\n')


class TestTask(unittest.TestCase):
    def test_func_args(self):
        def func2(*args, **kw):
            pass

        # constructor
        task = tracemalloc.Task(noop, 1, 2, 3, key='value')
        self.assertIs(task.func, noop)
        self.assertEqual(task.func_args, (1, 2, 3))
        self.assertEqual(task.func_kwargs, {'key': 'value'})

        # func
        task.func = str
        self.assertIs(task.func, str)
        self.assertRaises(TypeError, setattr, task, 'func', 5)

        # func_args
        task.func_args = ("Hello", "World!")
        self.assertEqual(task.func_args, ("Hello", "World!"))
        self.assertRaises(TypeError, task.func_args, 5)

        # func_kwargs
        task.func_kwargs = {'flush': True}
        self.assertEqual(task.func_kwargs, {'flush': True})
        task.func_kwargs = None
        self.assertIsNone(task.func_kwargs)
        self.assertRaises(TypeError, task.func_kwargs, 5)

    def test_memory_threshold(self):
        task = tracemalloc.Task(noop)
        self.assertIsNone(task.get_memory_threshold())

        task.set_memory_threshold(1024 * 1024)
        self.assertEqual(task.get_memory_threshold(), 1024 * 1024)

        self.assertRaises(ValueError, task.set_memory_threshold, 0)
        self.assertRaises(ValueError, task.set_memory_threshold, -1)
        self.assertRaises(TypeError, task.set_memory_threshold, 99.9)
        self.assertRaises(TypeError, task.set_memory_threshold, "str")

    def test_delay(self):
        task = tracemalloc.Task(noop)
        self.assertIsNone(task.get_delay())

        task.set_delay(60)
        self.assertEqual(task.get_delay(), 60)

        task.set_delay(9.9)
        self.assertEqual(task.get_delay(), 9)
        self.assertRaises(ValueError, task.set_delay, -1)
        self.assertRaises(ValueError, task.set_delay, 0)
        self.assertRaises(TypeError, task.set_delay, "str")

    def test_call(self):
        calls = []
        def log_func(*args, **kwargs):
            calls.append((args, kwargs))

        task = tracemalloc.Task(log_func, 1, 2, 3, key='value')
        task.call()
        self.assertEqual(len(calls), 1)
        args, kwargs = calls[0]
        self.assertEqual(args, (1, 2, 3))
        self.assertEqual(kwargs, {'key': 'value'})


class TestVersion(unittest.TestCase):
    def test_version(self):
        filename = os.path.join(os.path.dirname(__file__), 'setup.py')
        if sys.version_info >= (3, 4):
            import importlib
            loader = importlib.machinery.SourceFileLoader('setup', filename)
            setup_py = loader.load_module()
        else:
            setup_py = imp.load_source('setup', filename)
        self.assertEqual(tracemalloc.__version__, setup_py.VERSION)


def test_main():
    support.run_unittest(
        TestTracemallocEnabled,
        TestSnapshot,
        TestFilters,
        TestCommandLine,
        TestTop,
        TestTask,
        TestVersion,
    )

if __name__ == "__main__":
    test_main()
