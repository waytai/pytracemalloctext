"""
Script to converting tracemalloc documentation into PEP format to include it
into the PEP 454.
"""

import sys
import re
import subprocess

with open("Doc/library/tracemalloc.rst") as fp:
    content = fp.read()

pos = content.index("\nAPI\n")
content = content[pos:]

pos = content.index("\nCommand line options\n")
content = content[:pos]

content = content.strip() + "\n\n"

content = content.replace("~", "")

content = re.sub(
    r"^ *\.\. (attribute|class|function|method|classmethod):: (.*)",
    r"``\2`` \1:",
    content,
    flags=re.MULTILINE)

content = re.sub(
    r":(?:func|meth):`([^`]+)`",
    r"``\1()``",
    content)

content = re.sub(
    r":(?:mod|class|attr|option|envvar):`([^`]+)`",
    r"``\1``",
    content)

with open("pep.rst", "w") as fp:
    fp.write(content)

subprocess.call(["rst2html", "pep.rst", "pep.html"])
