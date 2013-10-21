:mod:`tracemalloctext` --- Text UI of the tracemalloc module
============================================================

.. module:: tracemalloctext
   :synopsis: Text UI of the tracemalloc module.

The tracemalloctext module is a text-based interface for the the
:mod:`tracemalloc` module.

See also the :mod:`tracemalloc` module.

.. versionadded:: 3.4


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
    pymalloc.allocated: 277 MiB
    pymalloc.blocks: 5125645
    pymalloc.fragmentation: 4.2%
    pymalloc.free: 12 MiB
    pymalloc.max_size: 563 MiB
    pymalloc.size: 294 MiB
    tracemalloc.arena_size: 294 MiB
    tracemalloc.module.fragmentation: 19.2%
    tracemalloc.module.free: 14 MiB
    tracemalloc.module.size: 77 MiB
    tracemalloc.traced.max_size: 182 MiB
    tracemalloc.traced.size: 151 MiB
    tracemalloc.traces: 645849
    unicode_interned.len: 48487
    unicode_interned.size: 1536 KiB


Usage
=====

Display top 25
--------------

Example displaying once the top 50 lines allocating the most memory::

    import tracemalloc
    import tracemalloctext
    tracemalloc.enable()
    # ... run your application ...
    tracemalloctext.DisplayTop().display(50)

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
    import tracemalloctext
    task = tracemalloctext.DisplayTopTask(25, group_by='filename')
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
    import tracemalloctext
    tracemalloc.enable()
    # ... run your application ...
    snapshot = tracemalloctext.Snapshot.create(traces=True)
    snapshot.write('snapshot.pickle')

Use the following command to display the snapshot file::

    python -m tracemalloctext snapshot.pickle

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
    import tracemalloctext
    task = tracemalloctext.TakeSnapshotTask(traces=True)
    task.set_memory_threshold(5 * 1024 * 1024)
    task.set_delay(60)
    tracemalloc.enable()
    task.schedule()
    # ... run your application ...

By default, snapshot files are written in the current directory with the name
``tracemalloc-XXXX.pickle`` where ``XXXX`` is a simple counter.

Use the following command to compare snapshot files::

    python -m tracemalloctext tracemalloc-0001.pickle tracemalloc-0002.pickle ...

See `Command line options`_, and :class:`TakeSnapshotTask` and :class:`StatsDiff`
classes for more options.


API
===

Functions
---------

.. function:: cancel_tasks()

   Cancel scheduled tasks.

   See also the :func:`get_tasks` function.


.. function:: get_tasks()

   Get the list of scheduled tasks, list of :class:`Task` instances.


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


   .. method:: is_scheduled()

      Return ``True`` if the task is scheduled, ``False`` otherwise.


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


Command line options
====================

The ``python -m tracemalloctext`` command can be used to display, analyze and
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

