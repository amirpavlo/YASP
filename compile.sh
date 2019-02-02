#!/bin/bash

root_dir=$PWD
install_dir=$PWD/sphinxinstall

#build YASP
export PKG_CONFIG_PATH=$install_dir/lib/pkgconfig/

gcc -Wall -Werror -g -o src/yasp src/yasp.c src/cJSON.c -I $root_dir/pocketsphinx/src/libpocketsphinx/  \
    -I $root_dir/include -I $root_dir/sphinxbase/include/sphinxbase/ \
    -DMODELDIR=\"`pkg-config --variable=modeldir pocketsphinx`\" \
    `pkg-config --cflags --libs pocketsphinx sphinxbase`

