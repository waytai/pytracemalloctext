:mod:`tracemalloc` --- Trace memory allocations
===============================================

.. module:: tracemalloc
   :synopsis: Trace memory allocations.

The tracemalloc module is a debug tool to trace memory blocks allocated by
Python. It provides the following information:

* Compute the differences between two snapshots to detect memory leaks
* Statistics on allocated memory blocks per filename and per line number:
  total size, number and average size of allocated memory blocks
* Traceback where a memory block was allocated

To trace most memory blocks allocated by Python, the module should be enabled
as early as possible by setting the :envvar:`PYTHONTRACEMALLOC` environment
variable to ``1``. The :func:`tracemalloc.enable` function can also be called to start
tracing Python memory allocations.

By default, a trace of an allocated memory block only stores one frame. Use the
:func:`set_traceback_limit` function to store more frames.

Python memory blocks allocated in the :mod:`tracemalloc` module are also traced
by default. Use ``add_exclude_filter(tracemalloc.__file__)`` to ignore these
these memory allocations.

At fork, the module is automatically disabled in the child process.

Project homepage: https://pypi.python.org/pypi/pytracemalloc

Documentation: http://pytracemalloc.readthedocs.org/

Python module developed by Wyplay: http://www.wyplay.com/


Status of the module
====================

pytracemalloc 0.9.1 contains patches for Python 2.5, 2.7 and 3.4.

Python 3.4 has a new API to replace memory allocators which can be used to
install hooks on memory allocations: `PEP 445 "Add new APIs to customize Python
memory allocators" <http://www.python.org/dev/peps/pep-0445/>`_. In Python 3.4,
the ``pymalloc`` allocator now also have a counter of allocated memory blocks.

The tracemalloc module was proposed for integration in the Python 3.4 standard
library: `PEP 454 "Add a new tracemalloc module to trace Python memory
allocations" <http://www.python.org/dev/peps/pep-0454/>`_.


Installation
============

Patch Python
------------

To install pytracemalloc, you need a modified Python runtime:

* Download Python source code
* Apply a patch (see below):
  patch -p1 < pythonXXX.patch
* Compile and install Python:
  ./configure && make && sudo make install
* It can be installed in a custom directory. For example:
  ./configure --prefix=/opt/mypython

Patches:

* Python 2.7: patches/2.7/pep445.patch
* Python 3.3: patches/3.3/pep445.patch


Compile and install pytracemalloc
---------------------------------

Dependencies:

* `Python <http://www.python.org>`_ 2.5 - 3.4
* `glib <http://www.gtk.org>`_ version 2
* (optional) `psutil <https://pypi.python.org/pypi/psutil>`_ to get the
  process memory. pytracemalloc is able to read the memory usage of the process
  on Linux without psutil.

Install::

    /opt/mypython/bin/python setup.py install


Example of top outputs
======================

Cumulative top 5 of the biggest allocations grouped by filename, compact
output::

    2013-10-03 11:34:39: Cumulative top 5 allocations per filename
    #1: .../Lib/test/regrtest.py: 554 MiB
    #2: .../Lib/unittest/suite.py: 499 MiB
    #3: <frozen importlib._bootstrap>: 401 MiB
    #4: .../test/support/__init__.py: 349 MiB
    #5: .../tracemalloc/Lib/runpy.py: 255 MiB
    1330 more: 822 MiB

Top 5 of the biggest allocations grouped by address, compact output::

    2013-10-03 11:34:39: Top 5 allocations per address
    #1: memory block 0x805e7010: size=80 MiB
    #2: memory block 0x9b531010: size=12 MiB
    #3: memory block 0x1a9b2838: size=1536 KiB
    #4: memory block 0x19dbfd88: size=253 KiB
    #5: memory block 0xa9fdcf0: size=252 KiB
    645844 more: size=56 MiB, average=92 B
    Traced Python memory: size=151 MiB, average=245 B

Top 10 of the biggest allocations grouped by line number, full output::

    2013-10-03 11:34:39: Top 10 allocations per filename and line number
    #1: .../tracemalloc/Lib/lzma.py:120: size=93 MiB, count=13, average=7 MiB
    #2: <frozen importlib._bootstrap>:704: size=24 MiB, count=357474, average=73 B
    #3: .../Lib/unittest/case.py:496: size=2997 KiB, count=7942, average=386 B
    #4: .../tracemalloc/Lib/linecache.py:127: size=2054 KiB, count=26474, average=79 B
    #5: .../Lib/test/test_datetime.py:32: size=1248 KiB, count=27, average=46 KiB
    #6: <frozen importlib._bootstrap>:274: size=989 KiB, count=12989, average=77 B
    #7: .../Lib/test/test_zipfile.py:1319: size=858 KiB, count=5, average=171 KiB
    #8: .../Lib/test/test_enumerate.py:150: size=852 KiB, count=29607, average=29 B
    #9: .../Lib/unittest/case.py:306: size=309 KiB, count=2504, average=126 B
    #10: .../Lib/test/test_zipfile.py:1508: size=307 KiB, count=12, average=25 KiB
    51150 more: size=24 MiB, count=208802, average=120 B
    Traced Python memory: size=151 MiB, count=645849, average=245 B

    gc.objects: 2688709
    process_memory.rss: 828 MiB
    process_memory.vms: 887 MiB
    tracemalloc.arena_size: 294 MiB
    tracemalloc.module.fragmentation: 19.2%
    tracemalloc.module.free: 14 MiB
    tracemalloc.module.size: 77 MiB
    tracemalloc.traced.max_size: 182 MiB
    tracemalloc.traced.size: 151 MiB
    tracemalloc.traces: 645849


Usage
=====

Display top 25
--------------

Example displaying once the top 50 lines allocating the most memory::

    import tracemalloc
    tracemalloc.enable()
    # ... run your application ...
    tracemalloc.DisplayTop().display(50)

By default, allocations are grouped by filename and line numbers and the top is
written into :data:`sys.stdout`.

See the :class:`DisplayTop` class for more options.


Display top with differences
----------------------------

To watch the evolution of memory allocations, the top allocations can be
displayed regulary using a task. Example displaying the top 50 files when the
traced memory is increased or decreased by more than 5 MB, or every minute,
with a compact output (no count, no average, no metric)::

    import tracemalloc
    task = tracemalloc.DisplayTopTask(25, group_by='filename')
    task.display_top.count = False
    task.display_top.average = False
    task.display_top.metrics = False
    task.set_memory_threshold(5 * 1024 * 1024)
    task.set_delay(60)
    tracemalloc.enable()
    task.schedule()
    # ... run your application ...

See the :class:`DisplayTopTask` class for more options.


Take a snapshot
---------------

The :class:`DisplayTopTask` class creates temporary snapshots which are lost
after the top is displayed. When you don't know what you are looking for, you
can take a snapshot of the allocated memory blocks to analyze it while the
application is running, or analyze it later.

Example taking a snapshot with traces and writing it into a file::

    import tracemalloc
    tracemalloc.enable()
    # ... run your application ...
    snapshot = tracemalloc.Snapshot.create(traces=True)
    snapshot.write('snapshot.pickle')

Use the following command to display the snapshot file::

    python -m tracemalloc snapshot.pickle

See `Command line options`_ for more options. See also
:meth:`Snapshot.apply_filters` and :meth:`DisplayTop.display_snapshot`
methods.


Compare snapshots
-----------------

It is not always easy to find a memory leak using a single snapshot. It is
easier to take multiple snapshots and compare them to see the differences.

Example taking a snapshot with traces when the traced memory is increased or
decreased by more than 5 MB, or every minute::

    import tracemalloc
    task = tracemalloc.TakeSnapshotTask(traces=True)
    task.set_memory_threshold(5 * 1024 * 1024)
    task.set_delay(60)
    tracemalloc.enable()
    task.schedule()
    # ... run your application ...

By default, snapshot files are written in the current directory with the name
``tracemalloc-XXXX.pickle`` where ``XXXX`` is a simple counter.

Use the following command to compare snapshot files::

    python -m tracemalloc tracemalloc-0001.pickle tracemalloc-0002.pickle ...

See `Command line options`_, and :class:`TakeSnapshotTask` and :class:`StatsDiff`
classes for more options.


API
===

The version of the module is ``tracemalloc.__version__`` (``str``, ex:
``"0.9.1"``).

Main Functions
--------------

.. function:: cancel_tasks()

   Cancel scheduled tasks.

   See also the :func:`get_tasks` function.


.. function:: clear_traces()

   Clear traces and statistics on Python memory allocations, and reset the
   :func:`get_arena_size` and :func:`get_traced_memory` counters.


.. function:: disable()

   Stop tracing Python memory allocations and cancel scheduled tasks.

   See also :func:`cancel_tasks`, :func:`enable` and :func:`is_enabled`
   functions.


.. function:: enable()

   Start tracing Python memory allocations.

   At fork, the module is automatically disabled in the child process.

   See also :func:`disable` and :func:`is_enabled` functions.


.. function:: get_stats()

   Get statistics on traced Python memory blocks as a dictionary ``{filename
   (str): {line_number (int): stats}}`` where *stats* in a
   ``(size: int, count: int)`` tuple, *filename* and *line_number* can
   be ``None``.

   Return an empty dictionary if the :mod:`tracemalloc` module is disabled.

   See also the :func:`get_traces` function.


.. function:: get_tasks()

   Get the list of scheduled tasks, list of :class:`Task` instances.


.. function:: is_enabled()

    ``True`` if the :mod:`tracemalloc` module is tracing Python memory
    allocations, ``False`` otherwise.

    See also :func:`enable` and :func:`disable` functions.


Trace Functions
---------------

.. function:: get_traceback_limit()

   Get the maximum number of frames stored in the traceback of a trace of a
   memory block.

   Use the :func:`set_traceback_limit` function to change the limit.


.. function:: get_object_address(obj)

   Get the address of the main memory block of the specified Python object.

   A Python object can be composed by multiple memory blocks, the function only
   returns the address of the main memory block.

   See also :func:`get_object_trace` and :func:`gc.get_referrers` functions.


.. function:: get_object_trace(obj)

   Get the trace of a Python object *obj* as a ``(size: int, traceback)`` tuple
   where *traceback* is a tuple of ``(filename: str, lineno: int)`` tuples,
   *filename* and *lineno* can be ``None``.

   The function only returns the trace of the main memory block of the object.
   The *size* of the trace is smaller than the total size of the object if the
   object is composed by more than one memory block.

   Return ``None`` if the :mod:`tracemalloc` module did not trace the
   allocation of the object.

   See also :func:`get_object_address`, :func:`get_trace`, :func:`get_traces`,
   :func:`gc.get_referrers` and :func:`sys.getsizeof` functions.


.. function:: get_trace(address)

   Get the trace of a memory block as a ``(size: int, traceback)`` tuple where
   *traceback* is a tuple of ``(filename: str, lineno: int)`` tuples,
   *filename* and *lineno* can be ``None``.

   Return ``None`` if the :mod:`tracemalloc` module did not trace the
   allocation of the memory block.

   See also :func:`get_object_trace`, :func:`get_stats` and :func:`get_traces`
   functions.


.. function:: get_traces()

   Get traces of Python memory allocations as a dictionary ``{address
   (int): trace}`` where *trace* is a
   ``(size: int, traceback)`` and *traceback* is a list of
   ``(filename: str, lineno: int)``.
   *traceback* can be empty, *filename* and *lineno* can be None.

   Return an empty dictionary if the :mod:`tracemalloc` module is disabled.

   See also :func:`get_object_trace`, :func:`get_stats` and :func:`get_trace`
   functions.


.. function:: set_traceback_limit(nframe: int)

   Set the maximum number of frames stored in the traceback of a trace of a
   memory block.

   Storing the traceback of each memory allocation has an important overhead on
   the memory usage. Use the :func:`get_tracemalloc_memory` function to measure
   the overhead and the :func:`add_filter` function to select which memory
   allocations are traced.

   Use the :func:`get_traceback_limit` function to get the current limit.


Filter Functions
----------------

.. function:: add_filter(filter)

   Add a new filter on Python memory allocations, *filter* is a :class:`Filter`
   instance.

   All inclusive filters are applied at once, a memory allocation is only
   ignored if no inclusive filter match its trace. A memory allocation is
   ignored if at least one exclusive filter matchs its trace.

   The new filter is not applied on already collected traces. Use the
   :func:`clear_traces` function to ensure that all traces match the new
   filter.

.. function:: add_include_filter(filename: str, lineno: int=None, traceback: bool=False)

   Add an inclusive filter: helper for the :meth:`add_filter` method creating a
   :class:`Filter` instance with the :attr:`~Filter.include` attribute set to
   ``True``.

   Example: ``tracemalloc.add_include_filter(tracemalloc.__file__)`` only
   includes memory blocks allocated by the :mod:`tracemalloc` module.


.. function:: add_exclude_filter(filename: str, lineno: int=None, traceback: bool=False)

   Add an exclusive filter: helper for the :meth:`add_filter` method creating a
   :class:`Filter` instance with the :attr:`~Filter.include` attribute set to
   ``False``.

   Example: ``tracemalloc.add_exclude_filter(tracemalloc.__file__)`` ignores
   memory blocks allocated by the :mod:`tracemalloc` module.


.. function:: clear_filters()

   Reset the filter list.

   See also the :func:`get_filters` function.


.. function:: get_filters()

   Get the filters on Python memory allocations as list of :class:`Filter`
   instances.

   See also the :func:`clear_filters` function.


Metric Functions
----------------

The following functions can be used to add metrics to a snapshot, see
the :meth:`Snapshot.add_metric` method.

.. function:: get_arena_size()

   Get the size in bytes of traced arenas.


.. function:: get_process_memory()

   Get the memory usage of the current process as a ``(rss: int, vms: int)``
   tuple, *rss* is the "Resident Set Size" in bytes and *vms* is the size of
   the virtual memory in bytes

   Return ``None`` if the platform is not supported.


.. function:: get_traced_memory()

   Get the current size and maximum size of memory blocks traced by the
   :mod:`tracemalloc` module as a tuple: ``(size: int, max_size: int)``.


.. function:: get_tracemalloc_memory()

   Get the memory usage in bytes of the :mod:`tracemalloc` module as a
   tuple: ``(size: int, free: int)``.

   * *size*: total size of bytes allocated by the module,
     including *free* bytes
   * *free*: number of free bytes available to store data


.. function:: get_unicode_interned()

   Get the size in bytes and the length of the dictionary of Unicode interned
   strings as a ``(size: int, length: int)`` tuple.

   The size is the size of the dictionary, excluding the size of strings.


DisplayTop
----------

.. class:: DisplayTop()

   Display the top of allocated memory blocks.

   .. method:: display(count=10, group_by="line", cumulative=False, file=None, callback=None)

      Take a snapshot and display the top *count* biggest allocated memory
      blocks grouped by *group_by*.

      *callback* is an optional callable object which can be used to add
      metrics to a snapshot. It is called with only one parameter: the newly
      created snapshot instance. Use the :meth:`Snapshot.add_metric` method to
      add new metric.

      Return the snapshot, a :class:`Snapshot` instance.

   .. method:: display_snapshot(snapshot, count=10, group_by="line", cumulative=False, file=None)

      Display a snapshot of memory blocks allocated by Python, *snapshot* is a
      :class:`Snapshot` instance.

   .. method:: display_top_diff(top_diff, count=10, file=None)

      Display differences between two :class:`GroupedStats` instances,
      *top_diff* is a :class:`StatsDiff` instance.

   .. method:: display_top_stats(top_stats, count=10, file=None)

      Display the top of allocated memory blocks grouped by the
      :attr:`~GroupedStats.group_by` attribute of *top_stats*, *top_stats* is a
      :class:`GroupedStats` instance.

   .. attribute:: average

      If ``True`` (default value), display the average size of memory blocks.

   .. attribute:: color

      If ``True``, always use colors. If ``False``, never use colors. The
      default value is ``None``: use colors if the *file* parameter is a TTY
      device.

   .. attribute:: compare_to_previous

      If ``True`` (default value), compare to the previous snapshot. If
      ``False``, compare to the first snapshot.

   .. attribute:: filename_parts

      Number of displayed filename parts (int, default: ``3``). Extra parts
      are replaced with ``'...'``.

   .. attribute:: metrics

      If ``True`` (default value), display metrics: see
      :attr:`Snapshot.metrics`.

   .. attribute:: previous_top_stats

      Previous :class:`GroupedStats` instance, or first :class:`GroupedStats`
      instance if :attr:`compare_to_previous` is ``False``, used to display the
      differences between two snapshots.

   .. attribute:: size

      If ``True`` (default value), display the size of memory blocks.


DisplayTopTask
--------------

.. class:: DisplayTopTask(count=10, group_by="line", cumulative=False, file=sys.stdout, callback=None)

   Task taking temporary snapshots and displaying the top *count* memory
   allocations grouped by *group_by*.

   :class:`DisplayTopTask` is based on the :class:`Task` class and so inherit
   all attributes and methods, especially:

   * :meth:`~Task.cancel`
   * :meth:`~Task.schedule`
   * :meth:`~Task.set_delay`
   * :meth:`~Task.set_memory_threshold`

   Modify the :attr:`display_top` attribute to customize the display.

   .. method:: display()

      Take a snapshot and display the top :attr:`count` biggest allocated
      memory blocks grouped by :attr:`group_by` using the :attr:`display_top`
      attribute.

      Return the snapshot, a :class:`Snapshot` instance.

   .. attribute:: callback

      *callback* is an optional callable object which can be used to add
      metrics to a snapshot. It is called with only one parameter: the newly
      created snapshot instance. Use the :meth:`Snapshot.add_metric` method to
      add new metric.

   .. attribute:: count

      Maximum number of displayed memory blocks.

   .. attribute:: cumulative

      If ``True``, cumulate size and count of memory blocks of all frames of
      each trace, not only the most recent frame. The default value is
      ``False``.

      The option is ignored if the traceback limit is less than ``2``, see
      the :func:`get_traceback_limit` function.

   .. attribute:: display_top

      Instance of :class:`DisplayTop`.

   .. attribute:: file

      The top is written into *file*.

   .. attribute:: group_by

      Determine how memory allocations are grouped: see :attr:`Snapshot.top_by`
      for the available values.


Filter
------

.. class:: Filter(include: bool, pattern: str, lineno: int=None, traceback: bool=False)

   Filter to select which memory allocations are traced. Filters can be used to
   reduce the memory usage of the :mod:`tracemalloc` module, which can be read
   using the :func:`get_tracemalloc_memory` function.

   .. method:: match(filename: str, lineno: int)

      Return ``True`` if the filter matchs the filename and line number,
      ``False`` otherwise.

   .. method:: match_filename(filename: str)

      Return ``True`` if the filter matchs the filename, ``False`` otherwise.

   .. method:: match_lineno(lineno: int)

      Return ``True`` if the filter matchs the line number, ``False``
      otherwise.

   .. method:: match_traceback(traceback)

      Return ``True`` if the filter matchs the *traceback*, ``False``
      otherwise.

      *traceback* is a tuple of ``(filename: str, lineno: int)`` tuples.

   .. attribute:: include

      If *include* is ``True``, only trace memory blocks allocated in a file
      with a name matching filename :attr:`pattern` at line number
      :attr:`lineno`.

      If *include* is ``False``, ignore memory blocks allocated in a file with
      a name matching filename :attr`pattern` at line number :attr:`lineno`.

   .. attribute:: lineno

      Line number (``int``). If is is ``None`` or less than ``1``, it matches
      any line number.

   .. attribute:: pattern

      The filename *pattern* can contain one or many ``*`` joker characters
      which match any substring, including an empty string. The ``.pyc`` and
      ``.pyo`` file extensions are replaced with ``.py``. On Windows, the
      comparison is case insensitive and the alternative separator ``/`` is
      replaced with the standard separator ``\``.

   .. attribute:: traceback

      If *traceback* is ``True``, all frames of the traceback are checked. If
      *traceback* is ``False``, only the most recent frame is checked.

      This attribute is ignored if the traceback limit is less than ``2``.
      See the :func:`get_traceback_limit` function.


GroupedStats
------------

.. class:: GroupedStats(timestamp: datetime.datetime, stats: dict, group_by: str, cumulative=False, metrics: dict=None)

   Top of allocated memory blocks grouped by *group_by* as a dictionary.

   The :meth:`Snapshot.top_by` method creates a :class:`GroupedStats` instance.

   .. method:: compare_to(old_stats: GroupedStats=None)

      Compare to an older :class:`GroupedStats` instance.
      Return a :class:`StatsDiff` instance.

      The :attr:`StatsDiff.differences` list is not sorted: call
      the :meth:`StatsDiff.sort` method to sort the list.

      ``None`` values are replaced with an empty string for filenames or zero
      for line numbers, because :class:`str` and :class:`int` cannot be
      compared to ``None``.

   .. attribute:: cumulative

      If ``True``, cumulate size and count of memory blocks of all frames of
      the traceback of a trace, not only the most recent frame.

   .. attribute:: metrics

      Dictionary storing metrics read when the snapshot was created:
      ``{name (str): metric}`` where *metric* type is :class:`Metric`.

   .. attribute:: group_by

      Determine how memory allocations were grouped: see
      :attr:`Snapshot.top_by` for the available values.

   .. attribute:: stats

      Dictionary ``{key: stats}`` where the *key* type depends on the
      :attr:`group_by` attribute and *stats* is a ``(size: int, count: int)``
      tuple.

      See the :meth:`Snapshot.top_by` method.

   .. attribute:: timestamp

      Creation date and time of the snapshot, :class:`datetime.datetime`
      instance.


Metric
------

.. class:: Metric(name: str, value: int, format: str)

   Value of a metric when a snapshot is created.

   .. attribute:: name

      Name of the metric.

   .. attribute:: value

      Value of the metric.

   .. attribute:: format

      Format of the metric:

      * ``'int'``: a number
      * ``'percent'``: percentage, ``1.0`` means ``100%``
      * ``'size'``: a size in bytes


Snapshot
--------

.. class:: Snapshot(timestamp: datetime.datetime, pid: int, traces: dict=None, stats: dict=None, metrics: dict=None)

   Snapshot of traces and statistics on memory blocks allocated by Python.

   Use :class:`TakeSnapshotTask` to take regulary snapshots.

   .. method:: add_gc_metrics()

      Add a metric on the garbage collector:

      * ``gc.objects``: total number of Python objects

      See the :mod:`gc` module.


   .. method:: add_metric(name: str, value: int, format: str)

      Helper to add a :class:`Metric` instance to :attr:`Snapshot.metrics`.
      Return the newly created :class:`Metric` instance.

      Raise an exception if the name is already present in
      :attr:`Snapshot.metrics`.


   .. method:: add_process_memory_metrics()

      Add metrics on the process memory:

      * ``process_memory.rss``: Resident Set Size
      * ``process_memory.vms``: Virtual Memory Size

      These metrics are only available if the :func:`get_process_memory`
      function is available on the platform.


   .. method:: add_tracemalloc_metrics()

      Add metrics on the :mod:`tracemalloc` module:

      * ``tracemalloc.traced.size``: size of memory blocks traced by the
        :mod:`tracemalloc` module
      * ``tracemalloc.traced.max_size``: maximum size of memory blocks traced
        by the :mod:`tracemalloc` module
      * ``tracemalloc.traces``: number of traces of Python memory blocks
      * ``tracemalloc.module.size``: total size of bytes allocated by the
        :mod:`tracemalloc` module, including free bytes
      * ``tracemalloc.module.free``: number of free bytes available for
        the :mod:`tracemalloc` module
      * ``tracemalloc.module.fragmentation``: percentage of fragmentation of
        the memory allocated by the :mod:`tracemalloc` module
      * ``tracemalloc.arena_size``: size of traced arenas

      ``tracemalloc.traces`` metric is only present if the snapshot was created
      with traces.


   .. method:: apply_filters(filters)

      Apply filters on the :attr:`traces` and :attr:`stats` dictionaries,
      *filters* is a list of :class:`Filter` instances.


   .. classmethod:: create(traces=False, metrics=True)

      Take a snapshot of traces and/or statistics of allocated memory blocks.

      If *traces* is ``True``, :func:`get_traces` is called and its result
      is stored in the :attr:`Snapshot.traces` attribute. This attribute
      contains more information than :attr:`Snapshot.stats` and uses more
      memory and more disk space. If *traces* is ``False``,
      :attr:`Snapshot.traces` is set to ``None``.

      If *metrics* is ``True``, fill :attr:`Snapshot.metrics` with metrics
      using the following methods:

      * :meth:`add_gc_metrics`
      * :meth:`add_process_memory_metrics`
      * :meth:`add_tracemalloc_metrics`

      If *metrics* is ``False``, :attr:`Snapshot.metrics` is set to an empty
      dictionary.

      Tracebacks of traces are limited to :attr:`traceback_limit` frames. Call
      :func:`set_traceback_limit` before calling :meth:`~Snapshot.create` to
      store more frames.

      The :mod:`tracemalloc` module must be enabled to take a snapshot. See the
      the :func:`enable` function.

   .. method:: get_metric(name, default=None)

      Get the value of the metric called *name*. Return *default* if the metric
      does not exist.


   .. classmethod:: load(filename, traces=True)

      Load a snapshot from a file.

      If *traces* is ``False``, don't load traces.


   .. method:: top_by(group_by: str, cumulative: bool=False)

      Compute top statistics grouped by *group_by* as a :class:`GroupedStats`
      instance:

      =====================  ========================  ==============
      group_by               description               key type
      =====================  ========================  ==============
      ``'filename'``         filename                  ``str``
      ``'line'``             filename and line number  ``(str, int)``
      ``'address'``          memory block address      ``int``
      =====================  ========================  ==============

      If *cumulative* is ``True``, cumulate size and count of memory blocks of
      all frames of the traceback of a trace, not only the most recent frame.
      The *cumulative* parameter is ignored if *group_by* is ``'address'`` or
      if the traceback limit is less than ``2``.


   .. method:: write(filename)

      Write the snapshot into a file.


   .. attribute:: metrics

      Dictionary storing metrics read when the snapshot was created:
      ``{name (str): metric}`` where *metric* type is :class:`Metric`.

   .. attribute:: pid

      Identifier of the process which created the snapshot, result of
      :func:`os.getpid`.

   .. attribute:: stats

      Statistics on traced Python memory, result of the :func:`get_stats`
      function.

   .. attribute:: traceback_limit

      Maximum number of frames stored in a trace of a memory block allocated by
      Python.

   .. attribute:: traces

      Traces of Python memory allocations, result of the :func:`get_traces`
      function, can be ``None``.

   .. attribute:: timestamp

      Creation date and time of the snapshot, :class:`datetime.datetime`
      instance.


StatsDiff
---------

.. class:: StatsDiff(differences, old_stats, new_stats)

   Differences between two :class:`GroupedStats` instances.

   The :meth:`GroupedStats.compare_to` method creates a :class:`StatsDiff`
   instance.

   .. method:: sort()

      Sort the :attr:`differences` list from the biggest difference to the
      smallest difference. Sort by ``abs(size_diff)``, *size*,
      ``abs(count_diff)``, *count* and then by *key*.

   .. attribute:: differences

      Differences between :attr:`old_stats` and :attr:`new_stats` as a list of
      ``(size_diff, size, count_diff, count, key)`` tuples. *size_diff*,
      *size*, *count_diff* and *count* are ``int``. The key type depends on the
      :attr:`~GroupedStats.group_by` attribute of :attr:`new_stats`: see the
      :meth:`Snapshot.top_by` method.

   .. attribute:: old_stats

      Old :class:`GroupedStats` instance, can be ``None``.

   .. attribute:: new_stats

      New :class:`GroupedStats` instance.


Task
----

.. class:: Task(func, \*args, \*\*kw)

   Task calling ``func(*args, **kw)``. When scheduled, the task is called when
   the traced memory is increased or decreased by more than *threshold* bytes,
   or after *delay* seconds.

   .. method:: call()

      Call ``func(*args, **kw)`` and return the result.


   .. method:: cancel()

      Cancel the task.

      Do nothing if the task is not scheduled.


   .. method:: get_delay()

      Get the delay in seconds. If the delay is ``None``, the timer is
      disabled.


   .. method:: get_memory_threshold()

      Get the threshold of the traced memory. When scheduled, the task is
      called when the traced memory is increased or decreased by more than
      *threshold* bytes. The memory threshold is disabled if *threshold* is
      ``None``.

      See also the :meth:`set_memory_threshold` method and the
      :func:`get_traced_memory` function.


   .. method:: schedule(repeat: int=None)

      Schedule the task *repeat* times. If *repeat* is ``None``, the task is
      rescheduled after each call until it is cancelled.

      If the method is called twice, the task is rescheduled with the new
      *repeat* parameter.

      The task must have a memory threshold or a delay: see :meth:`set_delay`
      and :meth:`set_memory_threshold` methods. The :mod:`tracemalloc` must be
      enabled to schedule a task: see the :func:`enable` function.

      The task is cancelled if the :meth:`call` method raises an exception.
      The task can be cancelled using the :meth:`cancel` method or the
      :func:`cancel_tasks` function.


   .. method:: set_delay(seconds: int)

      Set the delay in seconds before the task will be called. Set the delay to
      ``None`` to disable the timer.

      The timer is based on the Python memory allocator, it is not real time.
      The task is called after at least *delay* seconds, it is not called
      exactly after *delay* seconds if no Python memory allocation occurred.
      The timer has a resolution of 1 second.

      The task is rescheduled if it was scheduled.


   .. method:: set_memory_threshold(size: int)

      Set the threshold of the traced memory. When scheduled, the task is
      called when the traced memory is increased or decreased by more than
      *threshold* bytes. Set the threshold to ``None`` to disable it.

      The task is rescheduled if it was scheduled.

      See also the :meth:`get_memory_threshold` method and the
      :func:`get_traced_memory` function.


   .. attribute:: func

      Function, callable object.

   .. attribute:: func_args

      Function arguments, :class:`tuple`.

   .. attribute:: func_kwargs

      Function keyword arguments, :class:`dict`. It can be ``None``.


TakeSnapshotTask
----------------

.. class:: TakeSnapshotTask(filename_template: str="tracemalloc-$counter.pickle", traces: bool=False, metrics: bool=True, callback: callable=None)

   Task taking snapshots of Python memory allocations and writing them into
   files.

   :class:`TakeSnapshotTask` is based on the :class:`Task` class and so inherit
   all attributes and methods, especially:

   * :meth:`~Task.cancel`
   * :meth:`~Task.schedule`
   * :meth:`~Task.set_delay`
   * :meth:`~Task.set_memory_threshold`

   .. method:: take_snapshot()

      Take a snapshot and write it into a file.
      Return ``(snapshot, filename)`` where *snapshot* is a :class:`Snapshot`
      instance and filename type is :class:`str`.

   .. attribute:: callback

      *callback* is an optional callable object which can be used to add
      metrics to a snapshot. It is called with only one parameter: the newly
      created snapshot instance. Use the :meth:`Snapshot.add_metric` method to
      add new metric.

   .. attribute:: filename_template

      Template to create a filename. The template supports the following
      variables:

      * ``$pid``: identifier of the current process
      * ``$timestamp``: current date and time
      * ``$counter``: counter starting at 1 and incremented at each snapshot,
        formatted as 4 decimal digits

      The default template is ``'tracemalloc-$counter.pickle'``.

   .. attribute:: metrics

      Parameter passed to the :meth:`Snapshot.create` function.

   .. attribute:: traces

      Parameter passed to the :meth:`Snapshot.create` function.


Command line options
====================

The ``python -m tracemalloc`` command can be used to display, analyze and
compare snapshot files.

The command has the following options.

``-a``, ``--address`` option:

    Group memory allocations by address, instead of grouping by line number.

``-f``, ``--file`` option:

    Group memory allocations per filename, instead of grouping by line number.

``-n NUMBER``, ``--number NUMBER`` option:

    Number of traces displayed per top (default: 10): set the *count* parameter
    of the :meth:`DisplayTop.display_snapshot` method.

``--first`` option:

    Compare with the first snapshot, instead of comparing with the previous
    snapshot: set the :attr:`DisplayTop.compare_to_previous` attribute to
    ``False``.

``-c``, ``--cumulative`` option:

    Cumulate size and count of allocated memory blocks using all frames, not
    only the most recent frame: set *cumulative* parameter of the
    :meth:`DisplayTop.display_snapshot` method to ``True``.

    The option has only an effect if the snapshot
    contains traces and if the traceback limit was greater than ``1``.

``-b ADDRESS``, ``--block=ADDRESS`` option:

    Get the memory block at address *ADDRESS*, display its size and the
    traceback where it was allocated.

    The option can only be used on snapshots created with traces.

``-t``, ``--traceback`` option:

    Group memory allocations by address, display the size and the traceback
    of the *NUMBER* biggest allocated memory blocks.

    The option can only be used on snapshots created with traces. By default,
    the traceback limit is ``1`` frame: set a greater limit with the
    :func:`set_traceback_limit` function before taking snapshots to get more
    frames.

    See the ``--number`` option for *NUMBER*.

``-i FILENAME[:LINENO]``, ``--include FILENAME[:LINENO]`` option:

    Only include traces of files with a name matching *FILENAME* pattern at
    line number *LINENO*.  Only check the most recent frame. The option can be
    specified multiple times.

    See the :func:`add_include_filter` function for the syntax of a filter.

``-I FILENAME[:LINENO]``, ``--include-traceback FILENAME[:LINENO]`` option:

    Similar to ``--include`` option, but check all frames of the traceback.

``-x FILENAME[:LINENO]``, ``--exclude FILENAME[:LINENO]`` option:

    Exclude traces of files with a name matching *FILENAME* pattern at line
    number *LINENO*.  Only check the most recent frame. The option can be
    specified multiple times.

    See the :func:`add_exclude_filter` method for the syntax of a filter.

``-X FILENAME[:LINENO]``, ``--exclude-traceback FILENAME[:LINENO]`` option:

    Similar to ``--exclude`` option, but check all frames of the traceback.

``-S``, ``--hide-size`` option:

    Hide the size of allocations: set :attr:`DisplayTop.size` attribute to
    ``False``.

``-C``, ``--hide-count`` option:

    Hide the number of allocations: set :attr:`DisplayTop.count` attribute
    to ``False``.

``-A``, ``--hide-average`` option:

    Hide the average size of allocations: set :attr:`DisplayTop.average`
    attribute to ``False``.

``-M``, ``--hide-metrics`` option:

    Hide metrics, see :attr:`DisplayTop.metrics`.

``-P PARTS``, ``--filename-parts=PARTS`` option:

    Number of displayed filename parts (default: 3): set
    :attr:`DisplayTop.filename_parts` attribute.

``--color`` option:

    Always use colors, even if :data:`sys.stdout` is not a TTY device: set the
    :attr:`DisplayTop.color` attribute to ``True``.

``--no-color`` option:

    Never use colors, even if :data:`sys.stdout` is a TTY device: set the
    :attr:`DisplayTop.color` attribute to ``False``.


Changelog
=========

Development version:

- Rewrite the API to prepare the PEP 454

Version 0.9.1 (2013-06-01)

- Add ``PYTRACEMALLOC`` environment variable to trace memory allocation as
  early as possible at Python startup
- Disable the timer while calling its callback to not call the callback
  while it is running
- Fix pythonXXX_track_free_list.patch patches for zombie frames
- Use also MiB, GiB and TiB units to format a size, not only B and KiB

Version 0.9 (2013-05-31)

- Tracking free lists is now the recommended method to patch Python
- Fix code tracking Python free lists and python2.7_track_free_list.patch
- Add patches tracking free lists for Python 2.5.2 and 3.4.

Version 0.8.1 (2013-03-23)

- Fix python2.7.patch and python3.4.patch when Python is not compiled in debug
  mode (without --with-pydebug)
- Fix :class:`DisplayTop`: display "0 B" instead of an empty string if the size is zero
  (ex: trace in user data)
- setup.py automatically detects which patch was applied on Python

Version 0.8 (2013-03-19)

- The top uses colors and displays also the memory usage of the process
- Add :class:`DisplayGarbage` class
- Add :func:`get_process_memory` function
- Support collecting arbitrary user data using a callback:
  :meth:`Snapshot.create`, :class:`DisplayTop` and :class:`TakeSnapshot` have
  has an optional user_data_callback parameter/attribute
- Display the name of the previous snapshot when comparing two snapshots
- Command line (``-m tracemalloc``):

  * Add ``--color`` and ``--no-color`` options
  * ``--include`` and ``--exclude`` command line options can now be specified
    multiple times

- Automatically disable tracemalloc at exit
- Remove :func:`get_source` and :func:`get_stats` functions: they are now
  private

Version 0.7 (2013-03-04)

- First public version


Similar Projects
================

* `Meliae: Python Memory Usage Analyzer
  <https://pypi.python.org/pypi/meliae>`_
* `Guppy-PE: umbrella package combining Heapy and GSL
  <http://guppy-pe.sourceforge.net/>`_
* `PySizer <http://pysizer.8325.org/>`_: developed for Python 2.4
* `memory_profiler <https://pypi.python.org/pypi/memory_profiler>`_
* `pympler <http://code.google.com/p/pympler/>`_
* `memprof <http://jmdana.github.io/memprof/>`_:
  based on sys.getsizeof() and sys.settrace()
* `Dozer <https://pypi.python.org/pypi/Dozer>`_: WSGI Middleware version of
  the CherryPy memory leak debugger
* `objgraph <http://mg.pov.lt/objgraph/>`_
* `caulk <https://github.com/smartfile/caulk/>`_

