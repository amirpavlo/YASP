#!/bin/bash

root_dir=$PWD
install_dir=$PWD/sphinxinstall

# check if pocketsphinx directory doesn't exist.
# if not then clone it
[ ! -d pocketsphinx ] && git clone https://github.com/cmusphinx/pocketsphinx.git

# check if sphinxbase directory exists if not, then clone it
[ ! -d sphinxbase ] && git clone https://github.com/cmusphinx/sphinxbase.git

# create a sphinxinstall directory here.
[ ! -d sphinxinstall ] && mkdir sphinxinstall

# build sphinxbase
cd sphinxbase
./autogen.sh || exit 1
./configure --prefix=$install_dir || exit 1
make || exit 1
make install || exit 1
cd ..

# build pocketsphinx
cd pocketsphinx
./autogen.sh || exit 1
./configure --prefix=$install_dir || exit 1
make clean all || exit 1
make install || exit 1
cd ..


