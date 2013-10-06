from __future__ import with_statement
import _tracemalloc
import collections
import datetime
import functools
import gc
import linecache
import operator
import os
import pickle
import sys
import types
try:
    from time import monotonic as _time_monotonic
except ImportError:
    from time import time as _time_monotonic

# Import types and functions implemented in C
from _tracemalloc import *
from _tracemalloc import __version__

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

# On Windows, get_process_memory() is implemented in _tracemalloc
if os.name != "nt":
    def get_process_memory():
        """
        Get the memory usage of the current process as a  (rss: int, vms: int)
        tuple, rss is the Resident Set Size in bytes and vms is the size of the
        virtual memory in bytes

        Return None if the platform is not supported.
        """
        if get_process_memory.support_proc == False:
            return None

        try:
            fp = open("/proc/self/statm", "rb")
        except IOError:
            get_process_memory.support_proc = False
            return None

        try:
            page_size = os.sysconf("SC_PAGE_SIZE")
        except AttributeError:
            get_process_memory.support_proc = False
            return None

        get_process_memory.support_proc = True
        with fp:
            statm = fp.readline().split()
        vms = int(statm[0]) * page_size
        rss = int(statm[1]) * page_size
        return (rss, vms)

    get_process_memory.support_proc = None

def _stat_key(stats):
    return (abs(stats[0]), stats[1], abs(stats[2]), stats[3], stats[4])

class StatsDiff:
    __slots__ = ('differences', 'old_stats', 'new_stats')

    def __init__(self, differences, old_stats, new_stats):
        self.differences = differences
        self.old_stats = old_stats
        self.new_stats = new_stats

    def sort(self):
        self.differences.sort(reverse=True, key=_stat_key)


class GroupedStats:
    __slots__ = ('timestamp', 'stats', 'group_by', 'cumulative', 'metrics')

    def __init__(self, timestamp, stats, group_by,
                 cumulative=False, metrics=None):
        if group_by not in ('filename', 'line', 'address'):
            raise ValueError("invalid group_by value")
        # dictionary {key: stats} where stats is
        # a (size: int, count: int) tuple
        self.stats = stats
        self.group_by = group_by
        self.cumulative = cumulative
        self.timestamp = timestamp
        self.metrics = metrics

    def _create_key(self, key):
        if self.group_by == 'filename':
            if key is None:
                return ''
        elif self.group_by == 'line':
            filename, lineno = key
            if filename is None:
                filename = ''
            if lineno is None:
                lineno = 0
            return (filename, lineno)
        return key

    def compare_to(self, old_stats=None):
        if old_stats is not None:
            previous_dict = old_stats.stats.copy()

            differences = []
            for key, stats in self.stats.items():
                size, count = stats
                previous = previous_dict.pop(key, None)
                key = self._create_key(key)
                if previous is not None:
                    diff = (size - previous[0], size,
                            count - previous[1], count,
                            key)
                else:
                    diff = (size, size, count, count, key)
                differences.append(diff)

            for key, stats in previous_dict.items():
                key = self._create_key(key)
                diff = (-stats[0], 0, -stats[1], 0, key)
                differences.append(diff)
        else:
            differences = [
                (0, stats[0], 0, stats[1], self._create_key(key))
                for key, stats in self.stats.items()]

        return StatsDiff(differences, old_stats, self)


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

        if sys.version_info >= (3, 3):
            names = list(old_metrics.keys() | new_metrics.keys())
        else:
            names = list(set(old_metrics.keys()) | set(new_metrics.keys()))
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

            if format == 'size':
                formatter = _format_size_color
            else:
                text = _format_int

            text = self._format_metric(new_value, format)
            if color:
                text = _FORMAT_BOLD % text
            if old_value is not None:
                diff = self._format_metric(new_value - old_value, format, sign=True)
                if color:
                    diff = _FORMAT_YELLOW % diff
                text = '%s (%s)' % (text, diff)
            log("%s: %s\n" % (name, text))

    def display_top_diff(self, top_diff, count=10, file=None):
        if file is None:
            file = sys.stdout
        log = file.write
        if self.color is None:
            color = file.isatty()
        else:
            color = self.color
        diff_list = top_diff.differences
        top_stats = top_diff.new_stats
        previous_top_stats = top_diff.old_stats
        has_previous = (top_diff.old_stats is not None)
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

        # Sort differences by size and then by count
        top_diff.sort()

        # Display items
        total = [0, 0, 0, 0]
        for index in range(0, count):
            diff = diff_list[index]
            key_text = format_key(diff[4], color)
            diff_text = self._format_diff(diff, has_previous, show_count, color)
            log("#%s: %s: %s\n" % (1 + index, key_text, diff_text))
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

    def display_top_stats(self, top_stats, count=10, file=None):
        top_diff = top_stats.compare_to(self.previous_top_stats)
        self.display_top_diff(top_diff, count=count, file=file)

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
        snapshot = Snapshot.create(traces=traces,
                                   metrics=self.metrics)
        if callback is not None:
            callback(snapshot)

        self.display_snapshot(snapshot,
                              count=count,
                              group_by=group_by,
                              cumulative=cumulative,
                              file=file)
        return snapshot


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


def _compute_stats_frame(stats, group_per_file, size, frame):
    if not group_per_file:
        if frame is not None:
            key = frame
        else:
            key = (None, None)
    else:
        if frame is not None:
            key = frame[0]
        else:
            key = None
    if key in stats:
        stat_size, count = stats[key]
        size += stat_size
        count = count + 1
    else:
        count = 1
    stats[key] = (size, count)


class Metric:
    __slots__ = ('name', 'value', 'format')

    def __init__(self, name, value, format):
        self.name = name
        self.value = value
        self.format = format

    def __eq__(self, other):
        return (self.name == other.name and self.value == other.value)

    def __repr__(self):
        return ('<Metric name=%r value=%r format=%r>'
                % (self.name, self.value, self.format))


class Snapshot:
    FORMAT_VERSION = (3, 4)
    __slots__ = ('timestamp', 'pid', 'traceback_limit',
                 'stats', 'traces', 'metrics')

    def __init__(self, timestamp, pid, traceback_limit,
                 stats=None, traces=None, metrics=None):
        if traces is None and stats is None:
            raise ValueError("traces and stats cannot be None at the same time")
        self.timestamp = timestamp
        self.pid = pid
        self.traceback_limit = traceback_limit
        self.stats = stats
        self.traces = traces
        if metrics:
            self.metrics = metrics
        else:
            self.metrics = {}

    def add_metric(self, name, value, format):
        if name in self.metrics:
            raise ValueError("name already present: %r" % (name,))
        metric = Metric(name, value, format)
        self.metrics[metric.name] = metric
        return metric

    def add_tracemalloc_metrics(self):
        size, max_size = get_traced_memory()
        self.add_metric('tracemalloc.traced.size', size, 'size')
        self.add_metric('tracemalloc.traced.max_size', max_size, 'size')

        if self.traces:
            self.add_metric('tracemalloc.traces', len(self.traces), 'int')

        size, free = get_tracemalloc_memory()
        self.add_metric('tracemalloc.module.size', size, 'size')
        self.add_metric('tracemalloc.module.free', free, 'size')
        if size:
            frag = free / size
            self.add_metric('tracemalloc.module.fragmentation', frag, 'percent')

    def add_process_memory_metrics(self):
        process_memory = get_process_memory()
        if process_memory is not None:
            self.add_metric('process_memory.rss', process_memory[0], 'size')
            self.add_metric('process_memory.vms', process_memory[1], 'size')

    def add_gc_metrics(self):
        self.add_metric('gc.objects', len(gc.get_objects()), 'int')

    def get_metric(self, name, default=None):
        if name in self.metrics:
            return self.metrics[name].value
        else:
            return default

    @classmethod
    def create(cls, traces=False, metrics=True):
        if not is_enabled():
            raise RuntimeError("the tracemalloc module must be enabled "
                               "to take a snapshot")
        timestamp = datetime.datetime.now()
        pid = os.getpid()
        traceback_limit = get_traceback_limit()
        if traces:
            traces = get_traces()
        else:
            traces = None
        stats = get_stats()

        snapshot = cls(timestamp, pid, traceback_limit, stats, traces)
        if metrics:
            snapshot.add_tracemalloc_metrics()
            snapshot.add_gc_metrics()
            snapshot.add_process_memory_metrics()
        return snapshot

    @classmethod
    def load(cls, filename, traces=True):
        with open(filename, "rb") as fp:
            data = pickle.load(fp)

            try:
                if data['format_version'] != cls.FORMAT_VERSION:
                    raise TypeError("unknown format version")

                timestamp = data['timestamp']
                pid = data['pid']
                stats = data['stats']
                traceback_limit = data['traceback_limit']
                metrics = data.get('metrics')
            except KeyError:
                raise TypeError("invalid file format")

            if traces:
                traces = pickle.load(fp)
            else:
                traces = None

        return cls(timestamp, pid, traceback_limit, stats, traces, metrics)

    def write(self, filename):
        data = {
            'format_version': self.FORMAT_VERSION,
            'timestamp': self.timestamp,
            'pid': self.pid,
            'traceback_limit': self.traceback_limit,
            'stats': self.stats,
        }
        if self.metrics:
            data['metrics'] = self.metrics

        try:
            with open(filename, "wb") as fp:
                pickle.dump(data, fp, pickle.HIGHEST_PROTOCOL)
                pickle.dump(self.traces, fp, pickle.HIGHEST_PROTOCOL)
        except:
            # Remove corrupted pickle file
            if os.path.exists(filename):
                os.unlink(filename)
            raise

    def _filter_traces(self, include, filters):
        new_traces = {}
        for address, trace in self.traces.items():
            if include:
                match = any(trace_filter.match_traceback(trace[1])
                            for trace_filter in filters)
            else:
                match = all(trace_filter.match_traceback(trace[1])
                            for trace_filter in filters)
            if match:
                new_traces[address] = trace
        return new_traces

    def _filter_stats(self, include, filters):
        file_stats = {}
        for filename, line_stats in self.stats.items():
            if include:
                match = any(trace_filter.match_filename(filename)
                            for trace_filter in filters)
            else:
                match = all(trace_filter.match_filename(filename)
                            for trace_filter in filters)
            if not match:
                continue

            new_line_stats = {}
            for lineno, line_stat in line_stats.items():
                if include:
                    match = any(trace_filter.match(filename, lineno)
                                for trace_filter in filters)
                else:
                    match = all(trace_filter.match(filename, lineno)
                                for trace_filter in filters)
                if match:
                    new_line_stats[lineno] = line_stat

            file_stats[filename] = new_line_stats
        return file_stats

    def _apply_filters(self, include, filters):
        if not filters:
            return
        self.stats = self._filter_stats(include, filters)
        if self.traces is not None:
            self.traces = self._filter_traces(include, filters)

    def apply_filters(self, filters):
        include_filters = []
        exclude_filters = []
        for trace_filter in filters:
            if trace_filter.include:
                include_filters.append(trace_filter)
            else:
                exclude_filters.append(trace_filter)
        self._apply_filters(True, include_filters)
        self._apply_filters(False, exclude_filters)

    def top_by(self, group_by, cumulative=False):
        if cumulative and self.traceback_limit < 2:
            cumulative = False

        stats = {}
        if group_by == 'address':
            cumulative = False

            if self.traces is None:
                raise ValueError("need traces")

            for address, trace in self.traces.items():
                stats[address] = (trace[0], 1)
        else:
            if group_by == 'filename':
                group_per_file = True
            elif group_by == 'line':
                group_per_file = False
            else:
                raise ValueError("unknown group_by value: %r" % (group_by,))

            if not cumulative:
                for filename, line_dict in self.stats.items():
                    if not group_per_file:
                        for lineno, line_stats in line_dict.items():
                            key = (filename, lineno)
                            stats[key] = line_stats
                    else:
                        key = filename
                        total_size = total_count = 0
                        for size, count in line_dict.values():
                            total_size += size
                            total_count += count
                        stats[key] = (total_size, total_count)
            else:
                if self.traces is None:
                    raise ValueError("need traces")

                for trace in self.traces.values():
                    size, traceback = trace
                    if traceback:
                        for frame in traceback:
                            _compute_stats_frame(stats, group_per_file, size, frame)
                    else:
                        _compute_stats_frame(stats, group_per_file, size, None)

        return GroupedStats(self.timestamp, stats, group_by,
                            cumulative, self.metrics)


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
        filename = filename.replace("$pid", str(snapshot.pid))

        timestamp = _format_timestamp(snapshot.timestamp)
        timestamp = timestamp.replace(" ", "-")
        filename = filename.replace("$timestamp", timestamp)

        filename = filename.replace("$counter", "%04i" % self.counter)
        self.counter += 1
        return filename

    def take_snapshot(self):
        snapshot = Snapshot.create(traces=self.traces,
                                   metrics=self.metrics)
        if self.callback is not None:
            self.callback(snapshot)

        filename = self.create_filename(snapshot)
        snapshot.write(filename)
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

    if options.address or options.traceback:
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
            filters.add(Filter(include, pattern, lineno, traceback))

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
            snapshot = Snapshot.load(filename, load_traces)
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

    pids = set(snapshot.pid for snapshot in snapshots)
    if len(pids) > 1:
        pids = ', '.join(map(str, sorted(pids)))
        print("WARNING: Traces generated by different processes: %s" % pids)
        print("")

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

    elif options.traceback:
        for snapshot in snapshots:
            log("Sort traces of snapshot %s",
                _format_timestamp(snapshot.timestamp))
            traces = [(trace[0], address, trace[1])
                      for address, trace in snapshot.traces.items()]
            traces.sort(reverse=True)
            displayed_traces = traces[:options.number]

            timestamp = _format_timestamp(snapshot.timestamp)
            number = len(displayed_traces)
            if color:
                number = _FORMAT_BOLD % number
            log("")
            print("%s: Traceback of the top %s biggest memory blocks"
                  % (timestamp, number))
            print()

            for size, address, traceback in displayed_traces:
                address = _format_address(address, color)
                size = _format_size_color(size, color)
                print("Memory block %s: %s" % (address, size))
                for line in _format_traceback(traceback, options.filename_parts, color):
                    print(line)
                print()

            ignored = len(traces) - len(displayed_traces)
            if ignored:
                ignored_size = sum(size for size, address, traceback in traces[options.number:])
                size = _format_size_color(ignored_size, color)
                print("%s more memory blocks: size=%s" % (ignored, size))

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

