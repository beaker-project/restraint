.. _tasks:

Tasks
=====

Restraint doesn't require tasks to be written in any particular language. In
fact, most tests are written in a mixture of shell, python and C code. You do
need to provide some metadata in order for things to work best.

Restraint Metadata File
-----------------------

`Restraint` will look for a file called metadata in the task directory. The
format for that file is a simple `ini` file which most people should be familiar
with.

::

 [General]
 name=/restraint/env/metadata
 owner=User ABC1 <userabc1@example.com>
 description=just reports env variables
 license=GPLv2
 confidential=no
 destructive=no

 [restraint]
 entry_point=./runtest.sh
 max_time=5m
 dependencies=gcc;emacs
 softDependencies=numactl;numactl-devel
 environment=META_VAR1=var1value;META_VAR2=var2value;META_VAR3=var3value
 repoRequires=general/include;filesystems/include
 no_localwatchdog=true
 use_pty=false

`restraintd` does not require any metadata fields to be present. In other words,
there are no checks and reporting of errors if metadata is not present.  This allows
flexibility in your configuration.

The `General` section is mostly used for informational data. The only
element that `Restraint` will process is the `name` attribute. If defined,
this will overwrite the task name specified from the job XML.

The `restraint` section has the following elements which can be defined:

entry_point
~~~~~~~~~~~

This tells Restraint how it should start running the task. If you don't
specify a program to run it will default to 'make run' which is what legacy
RHTS (Red Hat Test System) would do. This would require you provide a
Makefile. Other examples of entry points::

 * entry_point=autotest-local control-file
 * entry_point=STAF local PROCESS START SHELL COMMAND "ps | grep test | wc >testcount.txt"

max_time
~~~~~~~~

The maximum time a task is expected to run. When restraintd runs a task it
sets up a localwatchdog which will kill the task after this time has expired.
When run in Beaker this is also used for the external watchdog (typically 20-30
minutes later than the local watchdog time). Time units can be specified as
follows::

 * d for days
 * h for hours
 * m for minutes
 * s for seconds

To set a max run time for 2 days you would use the following:

::

 max_time=2d

dependencies
~~~~~~~~~~~~

A semicolon-delimited (``;``) list of additional packages (needed to run this
task) to be installed on the system. The task will abort if the dependencies
fail to install.

::

 dependencies=lib-virt;httpd;postgresql;nfs-utils;net-tools;net-snmp;ethereal;wireshark;tcpdump;rusers;bzip2;gcc

environment
~~~~~~~~~~~

A semicolon-delimited (``;``) list of task environment variables to be set
on the system.

::

 environment=META_VAR1=var1value;META_VAR2=var2value;META_VAR3=var3value

softDependencies
~~~~~~~~~~~~~~~~

A semicolon-delimited (``;``) list of optional additional packages to be
installed on the system. The task will proceed even if the soft dependencies
fail to install. This is useful for a task that is intended to run on multiple
platforms, and the task can test platform-specific features (e.g., NUMA) if the
appropriate support packages are installed, but the task will not abort on the
other platforms where the support packages do not exist.

::

 softDependencies=numactl;numactl-devel

repoRequires
~~~~~~~~~~~~

A semicolon-delimited (``;``) list of additional tasks needed for this task to
run.

::

 repoRequires=general/include;filesystems/include

**Note:** When fetching from git (see :ref:`fetch-label`), this is the
``#subdirectory`` portion of the URL, so do *not* use a leading ``/`` character
as was done with RhtsRequires in testinfo.desc for Legacy RHTS tasks.

no_localwatchdog
~~~~~~~~~~~~~~~~

Normally Restraint will setup a localwatchdog which will attempt to recover
from a hung task before the external watchdog (if running under Beaker)
triggers. But you can tell Restraint to not setup a localwatchdog monitor by
including this key with a value of ``true``. Only ``true`` or ``false`` are
valid values.

::

 no_localwatchdog=true

use_pty
~~~~~~~

Before version 0.1.24 Restraint would execute all tasks from a pty. This meant
that programs thought they were running in an interactive terminal and might
produce ANSI codes for coloring and line positioning. Now the default is not to
use a pty which will give much cleaner output. If you find your test is failing
because it expects a pty you can enable the old behavior by setting this.

::

    use_pty=true

OSMajor Specific Options
~~~~~~~~~~~~~~~~~~~~~~~~

Any of the above elements can be overridden with OSMajor specific options. In
order for this to work the OSMajor (or "OS family") attribute must be filled in
the job.xml. If the job was run through Beaker this will have been filled in
for you. If you run a stand-alone job (with restraint-client) you can set the
value in the family attribute of the recipe tag. For example:

::

 <job>
   <recipeSet>
     <recipe family="RedHatEnterpriseLinuxServer5">
       ...

For example, if a task is known to take twice as long on
RedHatEnterpriseLinuxServer5 then you could use following:

::

 max_time=5m
 max_time[RedHatEnterpriseLinuxServer5]=10m

Another example where we will install RHDB on RedHatEnterpriseLinuxServer5 and
PostgreSQL on everything else.

::

 dependencies=postgresql
 dependencies[RedHatEnterpriseLinuxServer5]=rhdb

Legacy Metadata File
--------------------

Prior to the `Restraint` harness, users defined `testinfo.desc` file as the
metadata file in their job tasks and restraint supported that file.  This
is being deprecated and the substitute for this file and variables
within can be found in :ref:`legacy_metadata`.
