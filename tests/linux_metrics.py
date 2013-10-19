import os

def add_process_memory_metrics(snapshot):
    if sys.platform != "linux":
        return

    page_size = os.sysconf("SC_PAGE_SIZE")
    metrics = {}
    with open("/proc/snapshot/status", "rb") as  fp:
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

    if 'RSS' in metrics and 'HWM' in metrics:
        size = metrics['RSS'][0]
        max_size = metrics.pop('HWM')[0]
        metrics['RSS'] = ((size, max_size), 'size_peak')

    if 'Size' in metrics and 'Peak' in metrics:
        size = metrics['Size'][0]
        max_size = metrics.pop('Peak')[0]
        metrics['Size'] = ((size, max_size), 'size_peak')

    for key, value_format in metrics.items():
        value, format = value_format
        snapshot.add_metric('process_memory.%s' % key, value, format)

