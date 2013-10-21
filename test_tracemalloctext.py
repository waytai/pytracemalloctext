from test import support
from unittest.mock import patch
import datetime
import io
import os
import sys
import time
import tracemalloc
import tracemalloctext
import unittest

EMPTY_STRING_SIZE = sys.getsizeof(b'')
MEMORY_CHECK_DELAY = 0.1

def allocate_bytes(size):
    return b'x' * (size - EMPTY_STRING_SIZE)

def noop(*args, **kw):
    pass

def create_snapshots():
    traceback_limit = 2

    timestamp = datetime.datetime(2013, 9, 12, 15, 16, 17)
    stats = {
        'a.py': {2: (30, 3),
                 5: (2, 1)},
        'b.py': {1: (66, 1)},
        None: {None: (7, 1)},
    }
    traces = {
        0x10001: (10, (('a.py', 2), ('b.py', 4))),
        0x10002: (10, (('a.py', 2), ('b.py', 4))),
        0x10003: (10, (('a.py', 2), ('b.py', 4))),

        0x20001: (2, (('a.py', 5), ('b.py', 4))),

        0x30001: (66, (('b.py', 1),)),

        0x40001: (7, ((None, None),)),
    }
    snapshot = tracemalloc.Snapshot(timestamp, traceback_limit,
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
        0x10001: (10, (('a.py', 2), ('b.py', 4))),
        0x10002: (10, (('a.py', 2), ('b.py', 4))),
        0x10003: (10, (('a.py', 2), ('b.py', 4))),

        0x20001: (2, (('a.py', 5), ('b.py', 4))),
        0x20002: (5000, (('a.py', 5), ('b.py', 4))),

        0x30001: (400, (('c.py', 30),)),
    }
    snapshot2 = tracemalloc.Snapshot(timestamp2, traceback_limit,
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
        tracemalloc.add_exclusive_filter(tracemalloc.__file__)
        tracemalloc.set_traceback_limit(1)
        tracemalloc.enable()

    def tearDown(self):
        tracemalloctext.cancel_tasks()
        tracemalloc.disable()
        tracemalloc.clear_filters()

    def test_task_is_scheduled(self):
        task = tracemalloctext.Task(noop)
        task.set_delay(60)

        task.schedule()
        self.assertTrue(task.is_scheduled())

        # schedule twice should not fail
        task.schedule()

        task.cancel()
        self.assertFalse(task.is_scheduled())

        # second cancel() should not fail
        task.cancel()

        # cannot schedule if the tracemalloc module is disabled
        tracemalloc.disable()
        with self.assertRaises(RuntimeError) as cm:
            task.schedule()
        self.assertEqual(str(cm.exception),
                         "the tracemalloc module must be enabled "
                         "to schedule a task")

    def test_task_get_tasks(self):
        task = tracemalloctext.Task(noop)
        task.set_delay(60)

        task.schedule()
        self.assertIn(task, tracemalloctext.get_tasks())

        task.cancel()
        self.assertNotIn(task, tracemalloctext.get_tasks())

    def test_clear_tasks(self):
        task = tracemalloctext.Task(noop)
        task.set_delay(60)
        task.schedule()
        scheduled = tracemalloctext.get_tasks()
        self.assertEqual(len(scheduled), 1)

        # cancel_tasks() should cancel all tasks
        tracemalloctext.cancel_tasks()
        scheduled = tracemalloctext.get_tasks()
        self.assertEqual(len(scheduled), 0)

        task.schedule()
        scheduled = tracemalloctext.get_tasks()
        self.assertEqual(len(scheduled), 1)

    def test_task_delay(self):
        calls = []
        def log_func(*args, **kw):
            calls.append(log_func)

        task = tracemalloctext.Task(log_func)
        task.set_delay(1)
        task.schedule()
        time.sleep(1)
        obj = allocate_bytes(123)
        self.assertEqual(len(calls), 1)
        self.assertIs(calls[0], log_func)

    def test_task_memory_threshold(self):
        diff = None
        def log_func():
            nonlocal diff
            size, max_size = tracemalloc.get_traced_memory()
            diff = (size - old_size)

        obj_size  = 1024 * 1024
        threshold = int(obj_size * 0.75)

        old_size, max_size = tracemalloc.get_traced_memory()
        task = tracemalloctext.Task(log_func)
        task.set_memory_threshold(threshold)
        task.schedule()

        # allocate
        obj = allocate_bytes(obj_size)
        time.sleep(MEMORY_CHECK_DELAY)
        self.assertIsNotNone(diff)
        self.assertGreaterEqual(diff, threshold)

        # release
        diff = None
        old_size, max_size = tracemalloc.get_traced_memory()
        obj = None
        time.sleep(MEMORY_CHECK_DELAY)
        size, max_size = tracemalloc.get_traced_memory()
        self.assertIsNotNone(diff)
        self.assertLessEqual(diff, threshold)

    def test_task_repeat(self):
        calls = []
        def log_func():
            calls.append(log_func)

        task = tracemalloctext.Task(log_func)
        task.set_delay(0.1)

        # allocate at least 100 memory blocks, but the task should only be
        # called 3 times
        task.schedule(3)
        time.sleep(1.0)
        self.assertEqual(len(calls), 3)

    def test_task_callback_error(self):
        calls = []
        def failing_func():
            calls.append((failing_func,))
            raise ValueError("oops")

        task = tracemalloctext.Task(failing_func)
        task.set_memory_threshold(1)

        # If the task raises an exception, the exception should be logged to
        # sys.stderr (don't raise an exception at a random place)
        with support.captured_stderr() as stderr:
            task.schedule()

            obj = allocate_bytes(123)
            obj2 = allocate_bytes(456)
            # the timer should not be rescheduler on error
            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0], (failing_func,))
        output = stderr.getvalue()
        self.assertRegex(output, 'ValueError.*oops')
        #self.assertEqual(output.count('Traceback'), 1)

        self.assertFalse(task.is_scheduled())

    def test_take_snapshot(self):
        def callback(snapshot):
            snapshot.add_metric('callback', 5, 'size')

        with support.temp_cwd():
            task = tracemalloctext.TakeSnapshotTask(callback=callback)
            for index in range(1, 4):
                snapshot, filename = task.take_snapshot()
                self.assertEqual(snapshot.get_metric('callback'), 5)
                self.assertEqual(filename,
                                 'tracemalloc-%04d.pickle' % index)
                self.assertTrue(os.path.exists(filename))


class TestTop(unittest.TestCase):
    maxDiff = 2048

    def test_display_top_by_line(self):
        snapshot, snapshot2 = create_snapshots()

        # top per line
        output = io.StringIO()
        top = tracemalloctext.DisplayTop()
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
        output = io.StringIO()
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
        output = io.StringIO()
        top = tracemalloctext.DisplayTop()
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
        output = io.StringIO()
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
        output = io.StringIO()
        top = tracemalloctext.DisplayTop()
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

    def test_display_top_task(self):
        def callback(snapshot):
            snapshot.add_metric('task', 700, 'size')

        snapshot, snapshot2 = create_snapshots()

        # top per file (default options)
        output = io.StringIO()
        top = tracemalloctext.DisplayTop()
        top.metrics = False

        with patch.object(tracemalloctext.tracemalloc.Snapshot,
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
        task = tracemalloctext.Task(noop, 1, 2, 3, key='value')
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
        task = tracemalloctext.Task(noop)
        self.assertIsNone(task.get_memory_threshold())

        task.set_memory_threshold(1024 * 1024)
        self.assertEqual(task.get_memory_threshold(), 1024 * 1024)

        self.assertRaises(ValueError, task.set_memory_threshold, 0)
        self.assertRaises(ValueError, task.set_memory_threshold, -1)
        self.assertRaises(TypeError, task.set_memory_threshold, "str")

    def test_delay(self):
        task = tracemalloctext.Task(noop)
        self.assertIsNone(task.get_delay())

        task.set_delay(9.9)
        self.assertEqual(task.get_delay(), 9.9)

        task.set_delay(60)
        self.assertEqual(task.get_delay(), 60)

        self.assertRaises(ValueError, task.set_delay, -1)
        self.assertRaises(ValueError, task.set_delay, 0)
        self.assertRaises(TypeError, task.set_delay, "str")

    def test_call(self):
        calls = []
        def log_func(*args, **kwargs):
            calls.append((args, kwargs))

        task = tracemalloctext.Task(log_func, 1, 2, 3, key='value')
        task.call()
        self.assertEqual(len(calls), 1)
        args, kwargs = calls[0]
        self.assertEqual(args, (1, 2, 3))
        self.assertEqual(kwargs, {'key': 'value'})


def test_main():
    support.run_unittest(
        TestTracemallocEnabled,
        TestTop,
        TestTask,
    )

if __name__ == "__main__":
    test_main()
