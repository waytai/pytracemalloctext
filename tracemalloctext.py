import atexit
import gc
import linecache
import os
import signal
import sys
import threading
import tracemalloc
import weakref
try:
    from time import monotonic as _time_monotonic
except ImportError:
    from time import time as _time_monotonic

__version__ = '0.92'

def _format_timestamp(timestamp):
    return str(timestamp).split(".", 1)[0]

def _format_size(size, sign=False):
    for unit in ('B', 'KiB', 'MiB', 'GiB'):
        if abs(size) < 5 * 1024:
            if sign:
                return "%+i %s" % (size, unit)
            else:
                return "%i %s" % (size, unit)
        size /= 1024

    if sign:
        return "%+i TiB" % size
    else:
        return "%i TiB" % size

if os.name != "nt":
    _FORMAT_YELLOW = '\x1b[1;33m%s\x1b[0m'
    _FORMAT_BOLD = '\x1b[1m%s\x1b[0m'
    _FORMAT_CYAN = '\x1b[36m%s\x1b[0m'
else:
    _FORMAT_YELLOW = _FORMAT_BOLD = _FORMAT_CYAN = "%s"

def _format_size_color(size, color):
    text = _format_size(size)
    if color:
        text = _FORMAT_YELLOW % text
    return text

def _format_size_diff(size, diff, color):
    text = _format_size(size)
    if diff is not None:
        if color:
            text = _FORMAT_BOLD % text
        textdiff = _format_size(diff, sign=True)
        if color:
            textdiff = _FORMAT_YELLOW % textdiff
        text += " (%s)" % textdiff
    else:
        if color:
            text = _FORMAT_YELLOW % text
    return text

def _format_address(address, color):
    address = '0x%x' % address
    if color:
        address = _FORMAT_BOLD % address
    return address

def _format_int(value, color):
    text = '%i' % value
    if color:
        text = _FORMAT_YELLOW % text
    return text

def _colorize_filename(filename):
    path, basename = os.path.split(filename)
    if path:
        path += os.path.sep
    return _FORMAT_CYAN % path + basename

def _format_filename(filename, max_parts, color):
    if filename:
        parts = filename.split(os.path.sep)
        if max_parts < len(parts):
            parts = ['...'] + parts[-max_parts:]
        filename = os.path.sep.join(parts)
    else:
        # filename is None or an empty string
        filename = '???'
    if color:
        filename = _colorize_filename(filename)
    return filename

def _format_lineno(lineno):
    if lineno:
        return str(lineno)
    else:
        # lineno is None or an empty string
        return '?'

def _format_traceback(traceback, filename_parts, color):
    if traceback is None:
        return ('(empty traceback)',)
    lines = ['Traceback (most recent call first):']
    if traceback is not None:
        for frame in traceback:
            filename, lineno = frame
            if filename and lineno:
                line = linecache.getline(filename, lineno)
                line = line.strip()
            else:
                line = None

            filename = _format_filename(filename, filename_parts, color)
            lineno = _format_lineno(lineno)
            lines.append('  File "%s", line %s' % (filename, lineno))
            if line:
                lines.append('    ' + line)
    else:
        filename = _format_filename(None, filename_parts, color)
        lineno = _format_lineno(None)
        lines.append('  File "%s", line %s' % (filename, lineno))
    return lines


def add_process_memory_metrics(snapshot):
    # FIXME: support more platforms
    if sys.platform != "linux":
        return

    page_size = os.sysconf("SC_PAGE_SIZE")
    metrics = {}
    with open("/proc/self/status", "rb") as  fp:
        for line in fp:
            if not line.startswith(b"Vm"):
                continue
            key, value = line.split(b":")
            key = key[2:].decode("ascii")
            value = value.strip()
            if not value.endswith(b" kB"):
                continue
            value = int(value[:-3]) * 1024
            metrics[key] = (value, 'size')

    for key, value_format in metrics.items():
        value, format = value_format
        snapshot.add_metric('process_memory.%s' % key, value, format)

def add_pymalloc_metrics(snapshot):
    # FIXME: test python version
    snapshot.add_metric('pymalloc.blocks', sys.getallocatedblocks(), 'int')

def add_gc_metrics(snapshot):
    snapshot.add_metric('gc.objects', len(gc.get_objects()), 'int')

def add_tracemalloc_metrics(snapshot):
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

def add_metrics(snapshot):
    add_process_memory_metrics(snapshot)
    add_pymalloc_metrics(snapshot)
    add_gc_metrics(snapshot)
    # tracemalloc metrics uses the traces attribute
    add_tracemalloc_metrics(snapshot)

class DisplayTop:
    def __init__(self):
        self.size = True
        self.count = True
        self.average = True
        self.metrics = True
        self.filename_parts = 3
        self.color = None
        self.compare_to_previous = True
        self.previous_top_stats = None

    def _format_diff(self, diff, show_diff, show_count, color):
        if not show_count and not self.average:
            if show_diff:
                return _format_size_diff(diff[1], diff[0], color)
            else:
                return _format_size_color(diff[1], color)

        parts = []
        if self.size:
            if show_diff:
                text = _format_size_diff(diff[1], diff[0], color)
            else:
                text = _format_size_color(diff[1], color)
            parts.append("size=%s" % text)
        if show_count and (diff[3] or diff[2]):
            text = "count=%s" % diff[3]
            if show_diff:
                text += " (%+i)" % diff[2]
            parts.append(text)
        if (self.average
        and diff[3] > 1):
            parts.append('average=%s' % _format_size_color(diff[1] // diff[3], False))
        return ', '.join(parts)

    def _format_filename(self, key, color):
        return _format_filename(key, self.filename_parts, color)

    def _format_address(self, key, color):
        return 'memory block %s' % _format_address(key, color)

    def _format_traceback(self, key, color):
        return 'memory block %s' % _format_address(key[0], color)

    def _format_filename_lineno(self, key, color):
        filename, lineno = key
        filename = _format_filename(filename, self.filename_parts, color)
        lineno = _format_lineno(lineno)
        return "%s:%s" % (filename, lineno)

    def _format_metric(self, value, format, sign=False):
        if format == 'size':
            return _format_size(value, sign=sign)
        elif format == 'percent':
            if sign:
                return "%+.1f%%" % (value * 100)
            else:
                return "%.1f%%" % (value * 100)
        else:
            if sign:
                return "%+i" % value
            else:
                return "%i" % value

    def _display_metrics(self, log, previous_top_stats, top_stats, color):
        if top_stats.metrics is None and previous_top_stats is None:
            return

        if previous_top_stats is not None:
            old_metrics = previous_top_stats.metrics
        else:
            old_metrics  = {}
        if top_stats is not None:
            new_metrics = top_stats.metrics
        else:
            new_metrics  = {}

        names = list(old_metrics.keys() | new_metrics.keys())
        names.sort()
        if not names:
            return

        log("\n")
        for name in names:
            old_metric = old_metrics.get(name)
            if old_metric is not None:
                old_value = old_metric.value
                name = old_metric.name
                format = old_metric.format
            else:
                old_value = None

            new_metric = new_metrics.get(name)
            if new_metric is not None:
                new_value = new_metric.value
                name = new_metric.name
                format = new_metric.format
            else:
                new_value = 0

            text = self._format_metric(new_value, format)
            if color:
                text = _FORMAT_BOLD % text
            if old_value is not None:
                diff = self._format_metric(new_value - old_value, format, sign=True)
                if color:
                    diff = _FORMAT_YELLOW % diff
                text = '%s (%s)' % (text, diff)
            log("%s: %s\n" % (name, text))

    def display_top_stats(self, top_stats, count=10, file=None):
        previous_top_stats = self.previous_top_stats
        diff_list = top_stats.compare_to(previous_top_stats)

        if file is None:
            file = sys.stdout
        log = file.write
        if self.color is None:
            color = file.isatty()
        else:
            color = self.color
        has_previous = (previous_top_stats is not None)
        if top_stats.group_by == 'address':
            show_count = False
        else:
            show_count = self.count

        if top_stats.group_by == 'filename':
            format_key = self._format_filename
            per_text = "filename"
        elif top_stats.group_by == 'address':
            format_key = self._format_address
            per_text = "address"
        elif top_stats.group_by == 'traceback':
            format_key = self._format_traceback
            per_text = "traceback"
        else:
            format_key = self._format_filename_lineno
            per_text = "filename and line number"

        # Write the header
        nother = max(len(diff_list) - count, 0)
        count = min(count, len(diff_list))
        if top_stats.cumulative:
            text = "Cumulative top %s allocations per %s" % (count, per_text)
        else:
            text = "Top %s allocations per %s" % (count, per_text)
        if color:
            text = _FORMAT_CYAN % text
        if previous_top_stats is not None:
            text += ' (compared to %s)' % _format_timestamp(previous_top_stats.timestamp)
        name = _format_timestamp(top_stats.timestamp)
        if color:
            name = _FORMAT_BOLD % name
        file.write("%s: %s\n" % (name, text))

        # Display items
        total = [0, 0, 0, 0]
        for index in range(0, count):
            diff = diff_list[index]
            key = diff[4]
            key_text = format_key(key, color)
            diff_text = self._format_diff(diff, has_previous, show_count, color)
            log("#%s: %s: %s\n" % (1 + index, key_text, diff_text))
            if top_stats.group_by == 'traceback':
                for line in _format_traceback(key[1], self.filename_parts, color):
                    log(line + "\n")
                log("\n")

            total[0] += diff[0]
            total[1] += diff[1]
            total[2] += diff[2]
            total[3] += diff[3]

        other = tuple(total)
        for index in range(count, len(diff_list)):
            diff = diff_list[index]
            total[0] += diff[0]
            total[1] += diff[1]
            total[2] += diff[2]
            total[3] += diff[3]

        # Display "xxx more"
        if nother > 0:
            other = [
                total[0] - other[0],
                total[1] - other[1],
                total[2] - other[2],
                total[3] - other[3],
            ]
            other = self._format_diff(other, has_previous, show_count, color)
            text = "%s more" % nother
            if color:
                text = _FORMAT_CYAN % text
            log("%s: %s\n" % (text, other))

        if not top_stats.cumulative:
            text = self._format_diff(total, has_previous, show_count, color)
            log("Traced Python memory: %s\n" % text)

        if self.metrics:
            self._display_metrics(log, previous_top_stats, top_stats, color)

        log("\n")
        file.flush()

        # store the current top stats as the previous top stats for later
        # comparison with a newer top stats
        if self.compare_to_previous:
            self.previous_top_stats = top_stats
        else:
            if self.previous_top_stats is None:
                self.previous_top_stats = top_stats

    def display_snapshot(self, snapshot, count=10, group_by="line",
                         cumulative=False, file=None):
        top_stats = snapshot.top_by(group_by, cumulative)
        self.display_top_stats(top_stats, count=count, file=file)

    def display(self, count=10, group_by="line", cumulative=False, file=None,
                callback=None):
        if group_by == 'address':
            traces = True
        elif cumulative:
            traces = (tracemalloc.get_traceback_limit() > 1)
        else:
            traces = False
        snapshot = tracemalloc.Snapshot.create(traces=traces)
        if self.metrics:
            add_metrics(snapshot)
        if callback is not None:
            callback(snapshot)

        self.display_snapshot(snapshot,
                              count=count,
                              group_by=group_by,
                              cumulative=cumulative,
                              file=file)
        return snapshot


class _TaskThread(threading.Thread):
    def __init__(self, task, ncall):
        threading.Thread.__init__(self)
        self.daemon = True

        self.run_lock = threading.Lock()
        self.stop_lock = threading.Lock()
        self.sleep_lock = threading.Condition()
        self._task_ref = weakref.ref(task)
        self.memory_delay = 0.1
        self.ncall = ncall

        self.min_memory = None
        self.max_memory = None
        self.timeout = None

    def schedule(self):
        task = self._task_ref()
        memory_threshold = task.get_memory_threshold()
        delay = task.get_delay()

        if memory_threshold is not None:
            traced = tracemalloc.get_traced_memory()[0]
            self.min_memory = traced - memory_threshold
            self.max_memory = traced + memory_threshold
        else:
            self.min_memory = None
            self.max_memory = None

        if delay is not None:
            self.timeout = _time_monotonic() + delay
        else:
            self.timeout = None

    def interrupt_sleep(self):
        with self.sleep_lock:
            self.sleep_lock.notify()

    def reschedule(self):
        assert self.is_alive()
        # FIXME: reschedule using old traced and time, not new
        self.schedule()
        self.interrupt_sleep()

    def once(self):
        delay = None

        if self.min_memory is not None:
            traced = tracemalloc.get_traced_memory()[0]
            if traced <= self.min_memory:
                return None
            if traced >= self.max_memory:
                return None
            delay = self.memory_delay

        if self.timeout is not None:
            dt = (self.timeout - _time_monotonic())
            if dt <= 0:
                return None
            if delay is not None:
                delay = min(delay, dt)
            else:
                delay = dt

        return delay

    def _run(self):
        if hasattr(signal, 'pthread_sigmask'):
            # this thread should not handle any signal
            mask = range(1, signal.NSIG)
            signal.pthread_sigmask(signal.SIG_BLOCK, mask)

        self.schedule()

        while self.stop_lock.acquire(0):
            self.stop_lock.release()
            delay = self.once()
            if delay is not None:
                assert delay > 0.0
                with self.sleep_lock:
                    interrupted = self.sleep_lock.wait(timeout=delay)
                if interrupted:
                    break
                continue

            task = self._task_ref()
            try:
                task.call()
            except Exception as err:
                # the task is not rescheduled on error
                exc_type, exc_value, exc_tb = sys.exc_info()
                # FIXME: log the traceback
                print(("%s: %s" % (exc_type, exc_value)), file=sys.stderr)
                break
            if self.ncall is not None:
                self.ncall -= 1
                if self.ncall <= 0:
                    break
            self.schedule()

    def run(self):
        with self.run_lock:
            self._run()

    def stop(self):
        if not self.is_alive():
            return

        # ask to stop the thread
        self.stop_lock.acquire()

        # interrupt sleep
        self.interrupt_sleep()

        # wait until run() exited
        self.run_lock.acquire()

        # release locks
        self.run_lock.release()
        self.stop_lock.release()

    def _task(self):
        pass

_scheduled_tasks = {}

def get_tasks():
    tasks = []
    refs = list(_scheduled_tasks.values())
    for ref in refs:
        task = ref()
        if task is None:
            continue
        tasks.append(task)
    return tasks

def cancel_tasks():
    tasks = get_tasks()
    for task in tasks:
        task.cancel()
    _scheduled_tasks.clear()
cancel_tasks._registered = False


class Task:
    def __init__(self, func, *args, **kwargs):
        self._thread = None
        self._memory_threshold = None
        self._delay = None
        self._func_ref = weakref.ref(func)
        self.func_args = args
        self.func_kwargs = kwargs

    def __del__(self):
        self.cancel()

    def _get_func(self):
        return self._func_ref()
    def _set_func(self, func):
        self._func_ref = weakref.ref(func)
    func = property(_get_func, _set_func)

    def call(self):
        func = self._func_ref()
        func(*self.func_args, **self.func_kwargs)

    def get_delay(self):
        return self._delay

    def _cancel(self):
        self._thread.stop()
        self._thread.join()
        self._thread = None
        del _scheduled_tasks[id(self)]

    def is_scheduled(self):
        if self._thread is None:
            return False
        if not self._thread.is_alive():
            self._cancel()
            return False
        return True

    def _reschedule(self):
        if self.is_scheduled():
            self._thread.reschedule()

    def set_delay(self, delay):
        if delay <= 0.0:
            raise ValueError("delay must greater than 0")
        self._delay = delay
        self._reschedule()

    def get_memory_threshold(self):
        return self._memory_threshold

    def set_memory_threshold(self, size):
        if size < 1:
            raise ValueError("threshold must greater than 0")
        self._memory_threshold = size
        self._reschedule()

    def schedule(self, ncall=None):
        if self._delay is None and self._memory_threshold is None:
            raise ValueError("need a delay or a memory threshold")

        if not tracemalloc.is_enabled():
            raise RuntimeError("the tracemalloc module must be enabled "
                               "to schedule a task")

        self.cancel()
        self._thread = _TaskThread(self, ncall)
        self._thread.start()
        _scheduled_tasks[id(self)] = weakref.ref(self)
        if not cancel_tasks._registered:
            cancel_tasks._registered = True
            atexit.register(cancel_tasks)

    def cancel(self):
        if self._thread is None:
            return
        self._cancel()


class DisplayTopTask(Task):
    def __init__(self, count, group_by="line", cumulative=False,
                 file=None, callback=None):
        Task.__init__(self, self.display)
        self.display_top = DisplayTop()
        self.count = count
        self.group_by = group_by
        self.cumulative = cumulative
        self.file = file
        self.callback = callback
        self._task = None

    def display(self):
        return self.display_top.display(self.count, self.group_by,
                                        self.cumulative, self.file,
                                        self.callback)


class TakeSnapshotTask(Task):
    def __init__(self, filename_template="tracemalloc-$counter.pickle",
                 traces=False, metrics=True,
                 callback=None):
        Task.__init__(self, self.take_snapshot)
        self.filename_template = filename_template
        self.traces = traces
        self.metrics = metrics
        self.callback = callback
        self.counter = 1

    def create_filename(self, snapshot):
        filename = self.filename_template
        filename = filename.replace("$pid", str(os.getpid()))

        timestamp = _format_timestamp(snapshot.timestamp)
        timestamp = timestamp.replace(" ", "-")
        filename = filename.replace("$timestamp", timestamp)

        filename = filename.replace("$counter", "%04i" % self.counter)
        self.counter += 1
        return filename

    def take_snapshot(self):
        snapshot = tracemalloc.Snapshot.create(traces=self.traces)
        if self.metrics:
            add_metrics(snapshot)
        if self.callback is not None:
            self.callback(snapshot)

        filename = self.create_filename(snapshot)
        snapshot.dump(filename)
        return snapshot, filename


def main():
    from optparse import OptionParser

    parser = OptionParser(usage="%prog trace1.pickle [trace2.pickle  trace3.pickle ...]")
    parser.add_option("-a", "--address",
        help="Group memory allocations by address, "
              "instead of grouping by line number",
        action="store_true", default=False)
    parser.add_option("-f", "--file",
        help="Group memory allocations per filename, "
             "instead of grouping by line number",
        action="store_true", default=False)
    parser.add_option("-n", "--number",
        help="Number of traces displayed per top (default: 10)",
        type="int", action="store", default=10)
    parser.add_option("--first",
        help="Compare with the first trace, instead of with the previous trace",
        action="store_true", default=False)
    parser.add_option("-c", "--cumulative",
        help="Cumulate size and count of memory blocks using "
             "all frames, not only the most recent frame. The option has only "
             "an effect if the snapshot contains traces and the traceback limit"
             "was greater than 1",
        action="store_true", default=False)
    parser.add_option("-b", "--block", metavar="ADDRESS",
        help="Get the memory block at address ADDRESS, display its size and "
             "the traceback where it was allocated.",
        action="store", type="int", default=None)
    parser.add_option("-t", "--traceback",
        help="Group memmory allocations by address and display the size and "
             "the traceback of the NUMBER biggest allocated memory blocks",
        action="store_true", default=False)
    parser.add_option("-i", "--include", metavar="FILENAME[:LINENO]",
        help="Only show memory block allocated in a file with a name matching "
             "FILENAME pattern at line number LINENO. Ony check the most "
             "recent frame. The option can be specified multiple times.",
        action="append", type=str, default=[])
    parser.add_option("-I", "--include-traceback", metavar="FILENAME[:LINENO]",
        help="Similar to --include, but check all frames of the traceback.",
        action="append", type=str, default=[])
    parser.add_option("-x", "--exclude", metavar="FILENAME[:LINENO]",
        help="Exclude filenames matching FILENAME pattern at line number "
             "LINENO. Only check the most recent frame. The option can be "
             "specified multiple times.",
        action="append", type=str, default=[])
    parser.add_option("-X", "--exclude-traceback", metavar="FILENAME[:LINENO]",
        help="Similar to --exclude, but check all frames of the traceback.",
        action="append", type=str, default=[])
    parser.add_option("-S", "--hide-size",
        help="Hide the size of allocations",
        action="store_true", default=False)
    parser.add_option("-C", "--hide-count",
        help="Hide the number of allocations",
        action="store_true", default=False)
    parser.add_option("-A", "--hide-average",
        help="Hide the average size of allocations",
        action="store_true", default=False)
    parser.add_option("-M", "--hide-metrics",
        help="Hide metrics",
        action="store_true", default=False)
    parser.add_option("-P", "--filename-parts",
        help="Number of displayed filename parts (default: 3)",
        type="int", action="store", default=3)
    parser.add_option("--color",
        help="Always use colors",
        action="store_true", default=False)
    parser.add_option("--no-color",
        help="Never use colors",
        action="store_true", default=False)

    options, filenames = parser.parse_args()
    if not filenames:
        parser.print_help()
        sys.exit(1)

    if options.traceback:
        group_by = "traceback"
    elif options.address:
        group_by = "address"
    elif options.file:
        group_by = "filename"
    else:
        group_by = "line"

    # use set() to delete duplicate filters
    filters = set()
    for include, values, traceback in (
        (True, options.include, False),
        (True, options.include_traceback, True),
        (False, options.exclude, False),
        (False, options.exclude_traceback, True),
    ):
        for value in values:
            if ':' in value:
                pattern, lineno = value.rsplit(':', 1)
                lineno = int(lineno)
            else:
                pattern = value
                lineno = None
            filters.add(tracemalloc.Filter(include, pattern, lineno, traceback))

    def log(message, *args):
        if args:
            message = message % args
        sys.stderr.write(message + "\n")
        sys.stderr.flush()

    snapshots = []
    for filename in filenames:
        load_traces = (options.block or options.address
                       or options.traceback or options.cumulative)

        start = _time_monotonic()
        if load_traces:
            load_text = "Load snapshot %s" % filename
        else:
            load_text = "Load snapshot %s without traces" % filename
        log(load_text)
        try:
            snapshot = tracemalloc.Snapshot.load(filename, load_traces)
        except Exception:
            err = sys.exc_info()[1]
            print("ERROR: Failed to load %s: [%s] %s"
                  % (filename, type(err).__name__, err))
            sys.exit(1)

        info = []
        if snapshot.stats is not None:
            info.append('%s files' % len(snapshot.stats))
        if snapshot.traces is not None:
            info.append('%s traces (limit=%s frames)'
                        % (len(snapshot.traces), snapshot.traceback_limit))
        dt = _time_monotonic() - start
        log("Load snapshot %s: %s (%.1f sec)",
             filename, ', '.join(info), dt)

        if options.block is not None or options.traceback:
            if snapshot.traces is None:
                print("ERROR: The snapshot %s does not contain traces, "
                      "only stats" % filename)
                sys.exit(1)

        if filters:
            start = _time_monotonic()
            text = ("Apply %s filter%s on snapshot %s..."
                   % (len(filters),
                      's' if not filters or len(filters) > 1 else '',
                      _format_timestamp(snapshot.timestamp)))
            log(text)
            snapshot.apply_filters(filters)
            dt = _time_monotonic() - start
            log(text + " done (%.1f sec)" % dt)

        snapshots.append(snapshot)
    snapshots.sort(key=lambda snapshot: snapshot.timestamp)

    stream = sys.stdout
    if options.color:
        color = True
    elif options.no_color:
        color = False
    else:
        color = stream.isatty()

    log("")
    if options.block is not None:
        address = options.block

        for snapshot in snapshots:
            log("")
            trace = snapshot.traces.get(address)
            timestamp = _format_timestamp(snapshot.timestamp)
            address = _format_address(address, color)
            if color:
                timestamp = _FORMAT_CYAN % timestamp
            if trace is not None:
                size = _format_size_color(trace[0], color)
            else:
                size = '(not found)'
                if color:
                    size = _FORMAT_YELLOW % size
            print("%s, memory block %s: %s"
                  % (timestamp, address, size))
            if trace is not None:
                for line in _format_traceback(trace[1], options.filename_parts, color):
                    print(line)

    else:
        top = DisplayTop()
        top.filename_parts = options.filename_parts
        top.average = not options.hide_average
        top.count = not options.hide_count
        top.size = not options.hide_size
        top.metrics = not options.hide_metrics
        top.compare_to_previous = not options.first
        top.color = color

        for snapshot in snapshots:
            log("Group stats by %s ...", group_by)
            start = _time_monotonic()
            top_stats = snapshot.top_by(group_by, options.cumulative)
            dt = _time_monotonic() - start
            if dt > 0.5:
                log("Group stats by %s (%.1f sec)", group_by, dt)
            top.display_top_stats(top_stats, count=options.number, file=stream)

    print("%s snapshots" % len(snapshots))


if __name__ == "__main__":
    if 0:
        import cProfile
        cProfile.run('main()', sort='tottime')
    else:
        main()

