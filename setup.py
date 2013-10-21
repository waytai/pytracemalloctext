#!/usr/bin/env python

# Todo list to prepare a release:
#  - run unit tests
#  - update __version__ in tracemalloctext.py
#  - set release date in the README.rst file
#  - git commit -a
#  - git tag -a VERSION
#  - git push --tags
#  - python setup.py register sdist upload
#
# After the release:
#  - set version to n+1
#  - add a new empty section in the changelog for version n+1
#  - git commit
#  - git push

from __future__ import with_statement
from distutils.core import setup

CLASSIFIERS = [
    'Development Status :: 3 - Alpha',
    'Intended Audience :: Developers',
    'License :: OSI Approved :: MIT License',
    'Natural Language :: English',
    'Operating System :: OS Independent',
    'Programming Language :: C',
    'Programming Language :: Python',
    'Topic :: Security',
    'Topic :: Software Development :: Debuggers',
    'Topic :: Software Development :: Libraries :: Python Modules',
]

def main():
    with open('README.rst') as f:
        long_description = f.read().strip()

    import tracemalloctext
    VERSION = tracemalloctext.__version__

    options = {
        'name': 'pytracemalloc_text',
        'version': VERSION,
        'license': 'MIT license',
        'description': 'Track memory allocations per Python file',
        'long_description': long_description,
        'url': 'http://www.wyplay.com/',
        'download_url': 'https://github.com/haypo/pytracemalloc_text',
        'author': 'Victor Stinner',
        'author_email': 'victor.stinner@gmail.com',
        'classifiers': CLASSIFIERS,
        'py_modules': ["tracemalloc_text"],
    }
    setup(**options)

if __name__ == "__main__":
    main()

