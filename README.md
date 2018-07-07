# DNA - Dropbox for Nautilus on Arch
Unofficial Dropbox extension for Nautilus on Arch Linux.

## What is DNA?
DNA stands for Dropbox for Nautilus on Arch Linux, which, as the name suggests, is an (unofficial) Nautilus extension that allows integration with Dropbox.

## Origin
There already is an official Dropbox extension for Nautilus, but unfortunately it relies on outdated technologies and does not work properly on most modern installations of Arch Linux without modifying the source code. Rather than writing a custom extension from scratch or submitting a large pull request, I decided to fork the project and update the source code so it works out of the box on Arch Linux.

The original extension, released under the GNU Public License version 3, can be found [here](https://github.com/dropbox/nautilus-dropbox). If you're on another distribution, please check them out!

## Goals
The goals of this fork are:
* To replace C code with C++ where possible
* To update Python 2 code to Python 3
* To make the extension work out of the box on most Arch Linux installations
* To clean up the code