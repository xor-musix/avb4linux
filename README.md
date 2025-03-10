AVB4linux
========

Alsa driver and supporting tools based on the OpenAvnu project.

Currently this implementation will create an alsa PCM device and connect a single 8in/8out AVB stream to an AVB endpoint.
The driver currently only works with NICs based on the Intel's i210 chiptset.


LICENSING AND CONTRIBUTION GUIDELINES
======================================
To the extent possible, content is licensed under BSD licensing terms. Linux 
kernel mode components are provided under a GPLv2 license. 
Licensing information is included in the various directories to eliminate confusion. 
Please review the ‘LICENSE’ file included in the head of the 
various subdirectories for details.

PREREQUISITES
=============

Install the kernel headers.
Important: The kernel must have been compiled with ptp 1588 clock and i2c algobit support as modules.

GIT SUBMODULES
==============

Now the avb-linux git repository submodules should be configured:

    git submodule init
    git submodule update

Then compile the jdksavdecc library:

    cd  jdksavdecc-c
    cmake .
    make

And build

    cd ..
    make
    sudo make install

The `sudo make install` will create the directory `$HOME/.avb` and copy the binaries and kernel-module there
The bash scripts `avb_up.sh` and `avb_down.sh` are copied to `/usr/local/bin`.

Before you start, you need to edit the file `avb_up.sh`.
Edit the values of `OWNMAC`, `SOURCEMAC` and `TALKERMAC` to match your NIC and the AVB device. Here is a quick guide on [how to determine the correct MAC addresses](https://sonicarts.ucsd.edu/resources/index.html).

To start, enter in a console:

    sudo avb_up.sh \<ethernet interface name\> \<samplerate\>

To stop

    sudo avb_down.sh \<ethernet interface name\>

RELATED OPEN SOURCE PROJECTS
============================

OpenAvnu
--------

The OpenAvnu project where most of the code originates from.

+ https://github.com/AVnu/OpenAvnu

AVDECC
------
AVDECC library by Jeff Koftinoff

+ https://github.com/jdkoftinoff/jdksavdecc-c

